# Compaction Job Detection — Technical Walkthrough

This document explains precisely how the compaction oracle detects and identifies
compaction jobs: where a job boundary is defined, how it gets a stable identity,
how its type is classified, and at which call sites each lifecycle event is emitted.

---

## 1. What Counts as a "Job"

A **compaction job** in this implementation is any single invocation of
`BackgroundCompaction()` that results in a non-null `Compaction*` object.  There
are two structurally distinct paths:

| Path | Entry condition | Work function |
|------|-----------------|---------------|
| **Trivial move** | `!is_manual && c->IsTrivialMove()` | Stays in `BackgroundCompaction()` |
| **Full compaction** | All other non-null `c` | Delegated to `DoCompactionWork()` |

Both paths share the same identity model: a monotonically incrementing `job_id`
counter (`next_compaction_job_id_`) owned by `DBImpl`, incremented once per job
at the moment of dispatch.

---

## 2. Job Identity Assignment

`next_compaction_job_id_` is a plain `uint64_t` (not atomic) because it is only
ever touched while `mutex_` is held.

```cpp
// db/db_impl.h
CompactionTraceWriter* trace_writer_;
uint64_t next_compaction_job_id_;

// db/db_impl.cc — constructor initializer list
next_compaction_job_id_(1)
```

### Full compaction path — identity injected into `CompactionState`

```cpp
// db/db_impl.cc  BackgroundCompaction() ~line 902
CompactionState* compact = new CompactionState(c);

// Populate trace context before entering DoCompactionWork.
if (trace_writer_ != nullptr) {
  compact->job_id = next_compaction_job_id_++;
  compact->is_manual = is_manual;
  compact->compaction_reason = compaction_reason;
  compact->bg_thread_id = CurrentOsThreadId();
}

status = DoCompactionWork(compact);
```

`CompactionState` carries the trace context for the entire lifetime of the job:

```cpp
// db/db_impl.cc  struct DBImpl::CompactionState
// Trace context (populated by BackgroundCompaction before DoCompactionWork).
uint64_t job_id;
bool is_manual;
std::string compaction_reason;
uint64_t start_ts_us;
uint64_t bg_thread_id;
```

### Trivial move path — identity stored in local variables

```cpp
// db/db_impl.cc  BackgroundCompaction() trivial-move block ~line 802
uint64_t trivial_job_id = 0;
uint64_t trivial_start_ts = 0;
uint64_t trivial_tid = 0;
if (trace_writer_ != nullptr) {
  trivial_job_id = next_compaction_job_id_++;
  trivial_start_ts = env_->NowMicros();
  trivial_tid = CurrentOsThreadId();
  // ... emit job_start, job_input ...
}
```

The trivial-move path does not use `CompactionState` (it never enters
`DoCompactionWork`), so the identity lives in stack-local variables scoped to the
`BackgroundCompaction` call frame.

---

## 3. Compaction Reason Derivation

Reason classification happens **before** `PickCompaction()` is called, while
`mutex_` is still held and `versions_->current()` reflects the current version
state. This is the only safe window to inspect level scores and seek-compaction
candidates without a race.

```cpp
// db/db_impl.cc  BackgroundCompaction() ~line 766
const char* compaction_reason = "manual";
if (!is_manual) {
  if (versions_->current()->compaction_score() >= 1) {
    compaction_reason = "size";
  } else {
    compaction_reason = "seek";
  }
}
```

The two non-manual reasons map directly onto LevelDB's two automatic triggers:

- **`size`** — a level's total file size has exceeded its target
  (`compaction_score >= 1.0`). Set by `VersionSet::Finalize()`.
- **`seek`** — a file has accumulated too many point-lookup misses
  (`file_to_compact != nullptr`). Set by `Version::UpdateStats()`.

`compaction_score()` and `file_to_compact()` are `const` accessors added to
`Version` in `db/version_set.h` specifically for this derivation, avoiding any
modification to `VersionSet::PickCompaction()`.

---

## 4. Event Emission — Hook Sites

Seven event types are emitted across four functions.

### 4.1 `job_start` — job boundary open

**Full path** (`DoCompactionWork`, ~line 1174):

```cpp
if (trace_writer_ != nullptr) {
  compact->start_ts_us = start_micros;

  // Compute the compaction-wide key range from the union of all inputs.
  const InternalKey* job_smallest = nullptr;
  const InternalKey* job_largest = nullptr;
  const InternalKeyComparator& icmp = internal_comparator_;
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
      const FileMetaData* f = compact->compaction->input(which, i);
      if (job_smallest == nullptr ||
          icmp.Compare(f->smallest, *job_smallest) < 0)
        job_smallest = &f->smallest;
      if (job_largest == nullptr ||
          icmp.Compare(f->largest, *job_largest) > 0)
        job_largest = &f->largest;
    }
  }

  CompactionTraceWriter::Row r;
  r.trace_ts_us = start_micros;
  r.job_id = compact->job_id;
  r.event_type = "job_start";
  r.db_name = dbname_;
  r.is_manual = compact->is_manual ? 1 : 0;
  r.is_trivial_move = 0;
  r.compaction_reason = compact->compaction_reason;
  r.source_level = compact->compaction->level();
  r.target_level = compact->compaction->level() + 1;
  r.bg_thread_id = compact->bg_thread_id;
  if (job_smallest != nullptr && job_largest != nullptr)
    TraceSetKeyRange(&r, *job_smallest, *job_largest);
  trace_writer_->Write(r);
```

Key point: `job_start` is emitted while `mutex_` is **still held** (before the
`mutex_.Unlock()` that precedes the merge iterator loop). This guarantees that
`job_input` rows for all selected files are written atomically with `job_start`
before any output activity can be observed.

**Trivial-move path** (same fields, `is_trivial_move = 1`, emitted in
`BackgroundCompaction()`).

### 4.2 `job_input` — one row per selected SST

```cpp
// Inside the same mutex_-held block in DoCompactionWork
for (int which = 0; which < 2; which++) {
  int src_level = compact->compaction->level() + which;
  for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
    const FileMetaData* f = compact->compaction->input(which, i);
    CompactionTraceWriter::Row r;
    r.trace_ts_us = env_->NowMicros();
    r.job_id = compact->job_id;
    r.event_type = "job_input";
    r.source_level = src_level;
    r.target_level = compact->compaction->level() + 1;
    r.bg_thread_id = compact->bg_thread_id;
    r.file_number = f->number;
    r.file_size = f->file_size;
    TraceSetKeyRange(&r, f->smallest, f->largest);
    trace_writer_->Write(r);
  }
}
```

`which == 0` → source level files; `which == 1` → target level files that
overlap. Both are recorded because both are physically read and compacted.

### 4.3 `job_output_create` — output SST opened

Emitted in `OpenCompactionOutputFile()` after the `WritableFile` is successfully
created:
  
```cpp
// db/db_impl.cc  OpenCompactionOutputFile() ~line 986
if (s.ok() && trace_writer_ != nullptr) {
  CompactionTraceWriter::Row r;
  r.trace_ts_us = env_->NowMicros();
  r.job_id = compact->job_id;
  r.event_type = "job_output_create";
  // ... file_number, file_name, output_level ...
  trace_writer_->Write(r);
}
```

### 4.4 `job_output_finish` — output SST sealed

Emitted in `FinishCompactionOutputFile()` only when the file was non-empty and
the builder status is OK:

```cpp
// db/db_impl.cc  FinishCompactionOutputFile() ~line 1053
if (s.ok() && trace_writer_ != nullptr && current_entries > 0) {
  const CompactionState::Output& out = *compact->current_output();
  CompactionTraceWriter::Row r;
  r.trace_ts_us = env_->NowMicros();
  r.job_id = compact->job_id;
  r.event_type = "job_output_finish";
  // ... file_number, file_size, key range, output_level ...
  trace_writer_->Write(r);
}
```

The `current_entries > 0` guard prevents phantom rows for files that were opened
but written no entries (possible when `ShouldStopBefore` fires immediately).

### 4.5 `job_install` and `job_input_delete` — MANIFEST commit

Both are emitted in `InstallCompactionResults()`, immediately after
`versions_->LogAndApply()` returns:

```cpp
// db/db_impl.cc  InstallCompactionResults() ~line 1090
Status s = versions_->LogAndApply(compact->compaction->edit(), &mutex_);

if (trace_writer_ != nullptr) {
  // job_install — emitted regardless of success/failure
  {
    CompactionTraceWriter::Row r;
    r.event_type = "job_install";
    r.status = s.ok() ? "ok" : s.ToString();
    trace_writer_->Write(r);
  }

  // job_input_delete — only on successful install
  if (s.ok()) {
    for (int which = 0; which < 2; which++) {
      int src_level = compact->compaction->level() + which;
      for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
        const FileMetaData* f = compact->compaction->input(which, i);
        CompactionTraceWriter::Row r;
        r.event_type = "job_input_delete";
        r.source_level = src_level;
        r.file_number = f->number;
        r.file_size = f->file_size;
        TraceSetKeyRange(&r, f->smallest, f->largest);
        r.notes = "logical install-time obsolescence";
        trace_writer_->Write(r);
      }
    }
  }
}
```

`job_input_delete` represents **logical obsolescence at MANIFEST-commit time**, not
the physical `unlink(2)` syscall (which `RemoveObsoleteFiles()` defers until the
next background pass). The `notes` column records this distinction explicitly.

### 4.6 `job_end` — job boundary close

```cpp
// db/db_impl.cc  DoCompactionWork() ~line 1356
if (trace_writer_ != nullptr) {
  int total_inputs = 0;
  for (int which = 0; which < 2; which++)
    total_inputs += compact->compaction->num_input_files(which);

  CompactionTraceWriter::Row r;
  r.trace_ts_us = env_->NowMicros();
  r.job_id = compact->job_id;
  r.event_type = "job_end";
  r.status = status.ok() ? "ok" : status.ToString();
  r.bytes_read_logical  = static_cast<uint64_t>(stats.bytes_read);
  r.bytes_written_logical = static_cast<uint64_t>(stats.bytes_written);
  r.input_count  = total_inputs;
  r.output_count = static_cast<int>(compact->outputs.size());
  trace_writer_->Write(r);
}
```

`bytes_read_logical` / `bytes_written_logical` are SST file-size sums (the same
values LevelDB tracks in its internal `CompactionStats`), not record-level decoded
byte counts.

---

## 5. Key Extraction Helpers

Three file-local helpers in `db/db_impl.cc` avoid duplicating InternalKey
decoding at each site:

```cpp
// Extracts the raw user-key bytes from an InternalKey.
static std::string TraceUserKey(const InternalKey& ikey) {
  Slice uk = ikey.user_key();
  return std::string(uk.data(), uk.size());
}

// Extracts the sequence number from an InternalKey.
// Encoding: user_key || fixed64((seqno << 8) | value_type)
static uint64_t TraceSeqno(const InternalKey& ikey) {
  Slice enc = ikey.Encode();
  if (enc.size() < 8) return 0;
  return DecodeFixed64(enc.data() + enc.size() - 8) >> 8;
}

// Fills the key-range fields of a Row from an InternalKey pair.
static void TraceSetKeyRange(CompactionTraceWriter::Row* row,
                             const InternalKey& smallest,
                             const InternalKey& largest) {
  row->has_file_keys = true;
  row->smallest_user_key = TraceUserKey(smallest);
  row->largest_user_key = TraceUserKey(largest);
  row->seqno_smallest = TraceSeqno(smallest);
  row->seqno_largest = TraceSeqno(largest);
}
```

User-key bytes are stored raw in the `Row` and only hex-encoded during CSV
serialisation in `CompactionTraceWriter::Write()`, keeping the extraction helpers
binary-safe.

---

## 6. Thread-Safe CSV Serialisation

`CompactionTraceWriter::Write()` holds `mu_` for the entire serialisation and
`file_->Append()` call, making it safe for concurrent background threads:

```cpp
void CompactionTraceWriter::Write(const Row& row) {
  MutexLock lock(&mu_);
  uint64_t idx = next_event_index_++;   // global monotonic event counter

  std::string line;
  line.reserve(512);

  // --- per-column serialisation (26 columns) ---
  // Sentinel conventions:
  //   int field < 0  → emit empty cell
  //   uint64_t == 0  → emit empty cell (file_number, file_size, bg_thread_id, bytes_*)
  //   has_file_keys == false → emit four empty cells for key-range columns

  // Column 16–19: key range (hex-encoded)
  if (row.has_file_keys) {
    str_csv(HexEncode(row.smallest_user_key));
    str_csv(HexEncode(row.largest_user_key));
    // seqno_smallest, seqno_largest ...
  } else {
    line += ",,,,";   // four empty columns
  }

  file_->Append(line);
  file_->Flush();     // flush after every row — no buffering loss on crash
}
```

`event_index` is assigned inside the lock, so even when two threads write
simultaneously with identical `trace_ts_us` values the ordering in the file is
deterministic and the `event_index` column can be used as a tiebreaker.

---

## 7. Full Event Sequence

### Full compaction

```
job_start
job_input × (num_input_files(0) + num_input_files(1))
  [mutex_.Unlock() — merge iterator begins]
  [per output file]:
    job_output_create
    job_output_finish
  [mutex_.Lock()]
job_install
job_input_delete × (same file set as job_input)
job_end
```

### Trivial move

```
job_start          (is_trivial_move=1)
job_input          (single file)
  [versions_->LogAndApply() — MANIFEST commit]
job_install
job_input_delete   (single file)
job_end            (bytes_written_logical=0, output_count=0)
```

Trivial moves have no `job_output_create` or `job_output_finish` because no new
SST is written — the file is re-registered at the target level in the MANIFEST
without any data movement.

---

## 8. Opt-In Activation

Tracing is entirely opt-in and zero-overhead when disabled. The writer pointer
is checked at every emission site:

```cpp
// db/db_impl.cc — constructor
trace_writer_(raw_options.compaction_trace_path != nullptr
                  ? CompactionTraceWriter::Open(raw_options.env,
                                                raw_options.compaction_trace_path)
                  : nullptr),
next_compaction_job_id_(1)

// Every emission site:
if (trace_writer_ != nullptr) { ... }
```

Enable from `db_bench`:

```
--compaction_trace_path=/tmp/compaction.csv
```

Or programmatically via `Options`:

```cpp
leveldb::Options opts;
opts.compaction_trace_path = "/tmp/compaction.csv";
leveldb::DB* db;
leveldb::DB::Open(opts, "/tmp/mydb", &db);
```
