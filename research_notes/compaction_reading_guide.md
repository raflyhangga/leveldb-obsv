# LevelDB Compaction Source Code Reading Guide

## Background Concepts

Before reading code, understand these three structures:

- **`mem_`** — active memtable (RAM, skip list). All writes land here after the WAL.
- **`imm_`** — immutable memtable (RAM). Previous `mem_` frozen while waiting for disk flush.
- **SST files** — sorted string tables on disk, organized into levels L0–L6.

When `mem_` fills up, it rotates into `imm_`, and a background thread flushes it to a Level-0 SST. That's a **memtable compaction**. When L0 accumulates too many files, a **full compaction** merges SSTs from L_n into L_n+1.

---

## Reading Order

### 1. Trigger — `MaybeScheduleCompaction()` (`db/db_impl.cc:714`)

Entry point. Checks four guard conditions before scheduling the background thread. Read this first to understand *when* compaction is even considered.

Key conditions:
- Already scheduled? Skip.
- Shutting down or error? Skip.
- `imm_ == nullptr` AND no manual compaction AND `!NeedsCompaction()`? Nothing to do.

Otherwise, posts `BGWork` to the thread pool via `env_->Schedule()`.

---

### 2. Background thread dispatch — `BackgroundCall()` (`db/db_impl.cc:735`)

Thin wrapper. Holds mutex, calls `BackgroundCompaction()`, then calls `MaybeScheduleCompaction()` again — compactions can cascade (one level spills into the next).

---

### 3. Job selection — `BackgroundCompaction()` (`db/db_impl.cc:754`)

**Most important function to understand.** Three paths:

```
imm_ != nullptr  →  CompactMemTable()   (flush memtable, return early)
IsTrivialMove    →  LogAndApply only    (rename file to next level, zero I/O)
else             →  DoCompactionWork()  (full merge compaction)
```

Also read `VersionSet::PickCompaction()` (`db/version_set.cc`) to see how files are selected. Two triggers:
- **Size-triggered**: `compaction_score >= 1.0` (level is over its size budget)
- **Seek-triggered**: a file has been accessed too many times without finding data (`allowed_seeks` exhausted)

---

### 4. Memtable flush — `CompactMemTable()` (`db/db_impl.cc:595`)

Short function. Calls `WriteLevel0Table()` to serialize `imm_` (skip list) into an SST, then calls `LogAndApply()` to register it. Clears `imm_` when done, unblocking stalled writers.

---

### 5. Full compaction — `DoCompactionWork()` (`db/db_impl.cc:1138`)

The main merge loop. Read carefully — this is where key dropping happens and new SSTs are produced.

#### 5.1 Setup (mutex held, ~1138–1156)

```cpp
const uint64_t start_micros = env_->NowMicros();
int64_t imm_micros = 0;
```
`start_micros` timestamps the job. `imm_micros` accumulates time spent flushing `imm_` mid-loop — subtracted from final stats so memtable flushes don't inflate compaction timing.

```cpp
compact->smallest_snapshot = snapshots_.empty()
    ? versions_->LastSequence()
    : snapshots_.oldest()->sequence_number();
```
**GC horizon.** Any key version with seqno strictly below this threshold is invisible to all live readers and can be physically dropped. If no snapshots exist, every seqno up to `LastSequence()` is droppable.

#### 5.2 Oracle trace: `job_start` + `job_input` rows (~1160–1219)

Done **while mutex is still held** so the complete input set is captured atomically before any I/O races in.

```cpp
// compute union key range across all inputs (which=0: source level, which=1: target level)
for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
        // track min(f->smallest), max(f->largest) across all files
    }
}
```
Union key range stored in `job_smallest` / `job_largest` — written into the `job_start` row.

Then one `job_input` row per input file (all files from both levels), each carrying `file_number`, `file_size`, and per-file key range.

#### 5.3 Merge iterator + release mutex (~1222–1227)

```cpp
Iterator* input = versions_->MakeInputIterator(compact->compaction);
mutex_.Unlock();
input->SeekToFirst();
```
`MakeInputIterator` builds a **merging iterator** over all input SSTs from both levels, presenting them as one globally sorted key stream. Mutex dropped here — holding it across I/O would block every writer for the entire job duration.

#### 5.4 Main processing loop (~1233–1326)

Loop predicate: `input->Valid() && !shutting_down_`

**Imm flush preemption:**
```cpp
if (has_imm_.load(std::memory_order_relaxed)) {
    mutex_.Lock();
    CompactMemTable();            // flush imm_ → Level-0 SST
    background_work_finished_signal_.SignalAll();  // unblock stalled writers
    mutex_.Unlock();
    imm_micros += elapsed;
}
```
`imm_` flush unblocks stalled writers — higher priority than compaction. Job pauses, lets flush complete, then resumes.

**Grandparent overlap check:**
```cpp
if (compact->compaction->ShouldStopBefore(key) && compact->builder != nullptr)
    FinishCompactionOutputFile(compact, input);
```
Seals the current output file early when it's accumulating too much key overlap with grandparent-level (L+2) files. Limits the cost of the *next* compaction that will touch this output.

**Key drop logic — two rules:**

```cpp
// Rule A: duplicate key, older version
if (last_sequence_for_key <= compact->smallest_snapshot)
    drop = true;
```
Merged iterator yields keys in (user_key ASC, seqno DESC) order. First occurrence of a user key is the newest — write it. Subsequent occurrences have lower seqno; if below snapshot horizon, no reader can see them → drop.

```cpp
// Rule B: tombstone with no data below
else if (ikey.type == kTypeDeletion
      && ikey.sequence <= compact->smallest_snapshot
      && compact->compaction->IsBaseLevelForKey(ikey.user_key))
    drop = true;
```
A deletion tombstone can only be physically removed when all three hold simultaneously: (1) below snapshot horizon so no reader sees it, (2) `IsBaseLevelForKey` confirms no live data exists at deeper levels that the tombstone must suppress, and (3) older versions of the key at this level will be dropped by Rule A.

**Write surviving keys:**
```cpp
if (!drop) {
    if (compact->builder == nullptr)
        OpenCompactionOutputFile(compact);   // emits job_output_create trace row
    if (compact->builder->NumEntries() == 0)
        compact->current_output()->smallest.DecodeFrom(key);  // track first key
    compact->current_output()->largest.DecodeFrom(key);       // always update last key
    compact->builder->Add(key, input->value());

    if (compact->builder->FileSize() >= compact->compaction->MaxOutputFileSize())
        FinishCompactionOutputFile(compact, input);  // emits job_output_finish trace row
}
```
Opens an output SST on demand. Tracks the per-file key range. Seals and starts a new file when size limit hit — compactions produce multiple output files.

#### 5.5 Post-loop finalization (~1328–1338)

```cpp
if (status.ok() && shutting_down_)
    status = Status::IOError("Deleting DB during compaction");
if (status.ok() && compact->builder != nullptr)
    FinishCompactionOutputFile(compact, input);  // seal last open file
if (status.ok())
    status = input->status();  // propagate any iterator read error
delete input;
```
Three cases: DB shutdown mid-loop, unsealed last output file, iterator I/O error.

#### 5.6 Stats collection + version install (~1340–1361)

```cpp
stats.micros = NowMicros() - start_micros - imm_micros;  // net compaction time
// sum bytes_read across all input files
// sum bytes_written across all output files

mutex_.Lock();
stats_[level + 1].Add(stats);         // accumulate into per-level counters
status = InstallCompactionResults(compact);  // emits job_install + job_input_delete trace
```
Re-acquires mutex. `InstallCompactionResults` calls `LogAndApply` — atomically writes a MANIFEST record that removes all input files and adds all output files. After this returns, the new version is live and old input files are logically dead.

#### 5.7 Oracle `job_end` row (~1364–1383)

```cpp
r.event_type = "job_end";
r.bytes_read_logical  = stats.bytes_read;
r.bytes_written_logical = stats.bytes_written;
r.input_count  = total_inputs;
r.output_count = compact->outputs.size();
r.status = status.ok() ? "ok" : status.ToString();
trace_writer_->Write(r);
```
Closes the job_id sequence in the CSV. Carries I/O summary and success/failure.

#### Full trace event sequence per job

```
job_start            ← §5.2, mutex held, one row
job_input × N        ← §5.2, one row per input file
  [loop begins]
  job_output_create  ← §5.4, inside OpenCompactionOutputFile (repeats per output file)
  job_output_finish  ← §5.4, inside FinishCompactionOutputFile (repeats per output file)
  [loop ends]
job_install          ← §5.6, inside InstallCompactionResults
job_input_delete × N ← §5.6, inside InstallCompactionResults, one per input file
job_end              ← §5.7, mutex held
```

#### Key local variables

| Variable | Type | Role |
|---|---|---|
| `compact` | `CompactionState*` | Per-job mutable state: output files, builder, trace context |
| `start_micros` | `uint64_t` | Job wall-clock start for stats |
| `imm_micros` | `int64_t` | Time stolen by imm_ flushes, excluded from compaction stats |
| `input` | `Iterator*` | Merged sorted stream across all input SSTs |
| `ikey` | `ParsedInternalKey` | Decoded current key: `user_key`, `sequence`, `type` |
| `current_user_key` | `string` | Last seen user key — detects when key changes |
| `has_current_user_key` | `bool` | False at start and after corrupt key |
| `last_sequence_for_key` | `SequenceNumber` | Seqno of previous occurrence of same user key — drives Rule A drop |
| `drop` | `bool` | Whether current key-value is discarded (not written to output) |
| `stats` | `CompactionStats` | Bytes read/written, net micros — added to `stats_[level+1]` |

---

### 6. Output file lifecycle

#### `OpenCompactionOutputFile()` (`db/db_impl.cc:962`)
- Briefly re-acquires mutex to allocate a file number atomically.
- Releases mutex before creating the file — I/O outside the lock.
- Creates a `TableBuilder` to write SST format.

#### `FinishCompactionOutputFile()` (`db/db_impl.cc:1005`)
- Calls `builder->Finish()` to write index/footer blocks.
- Syncs and closes the file.
- Verifies the file is readable via `table_cache_->NewIterator()`.

---

### 7. Version commit — `InstallCompactionResults()` (`db/db_impl.cc:1073`)

- Calls `AddInputDeletions()` — marks all input files as deleted in the version edit.
- Calls `versions_->LogAndApply()` — atomically writes one MANIFEST record that removes input files and adds output files. After this returns, the new version is live.

---

### 8. Version selection internals — `db/version_set.cc`

Read after understanding the above:

| Function | What it does |
|---|---|
| `PickCompaction()` | Selects level and files to compact (size vs seek trigger) |
| `CompactRange()` | Manual compaction file selection |
| `LogAndApply()` | Writes VersionEdit to MANIFEST, installs new Version |
| `MakeInputIterator()` | Builds the merged iterator over all input files |
| `Version::OverlappingInputs()` | Expands input file set to avoid key-range gaps |

---

## Call Graph Summary

```
MaybeScheduleCompaction()
└── env_->Schedule(BGWork)
    └── BackgroundCall()
        └── BackgroundCompaction()
            ├── [imm_] CompactMemTable()
            │         └── WriteLevel0Table()
            │             └── versions_->LogAndApply()
            │
            ├── [trivial] versions_->LogAndApply()   ← zero I/O
            │
            └── [full] DoCompactionWork()
                        ├── versions_->MakeInputIterator()
                        ├── [per key] OpenCompactionOutputFile()
                        ├── [per key] FinishCompactionOutputFile()
                        └── InstallCompactionResults()
                            └── versions_->LogAndApply()
```

---

## Key Files

| File | What to read |
|---|---|
| `db/db_impl.cc` | Full compaction lifecycle (this guide) |
| `db/db_impl.h` | `DBImpl` fields: `mem_`, `imm_`, `versions_`, `trace_writer_` |
| `db/version_set.cc` | File selection, version management, MANIFEST writes |
| `db/version_set.h` | `Compaction` class, `VersionEdit`, `VersionSet` interface |
| `db/dbformat.h` | `InternalKey` encoding, level constants, sequence numbers |
| `db/skiplist.h` | Memtable backing structure |
| `table/merger.cc` | `MakeInputIterator` merge logic |
