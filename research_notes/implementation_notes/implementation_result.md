# Compaction Oracle Implementation Result

## Status

Complete. All existing tests pass (3/3 test suites, 0 failures).

## Files Changed

### New files

| File | Purpose |
|------|---------|
| `db/compaction_trace_writer.h` | `CompactionTraceWriter` class declaration and `Row` struct |
| `db/compaction_trace_writer.cc` | Thread-safe CSV writer implementation |

### Modified files

| File | Change |
|------|--------|
| `include/leveldb/options.h` | Added `const char* compaction_trace_path = nullptr` |
| `db/version_set.h` | Added `compaction_score()` and `file_to_compact()` const accessors to `Version` |
| `db/db_impl.h` | Added `CompactionTraceWriter* trace_writer_` and `uint64_t next_compaction_job_id_` to `DBImpl`; added trace context fields to `CompactionState` |
| `db/db_impl.cc` | Initialized trace writer in constructor; instrumented all 7 hook sites; added `TraceUserKey`, `TraceSeqno`, `TraceSetKeyRange` helpers |
| `benchmarks/db_bench.cc` | Added `--compaction_trace_path=<path>` flag |
| `CMakeLists.txt` | Added `compaction_trace_writer.cc` and `.h` to `leveldb` target sources |

## CSV Schema

26 columns, one row per event, single flat file. All rows from all jobs are written to the same file.

```
trace_ts_us, event_index, job_id, event_type, db_name, cf_name,
is_manual, is_trivial_move, compaction_reason,
source_level, target_level, bg_thread_id,
file_number, file_name, file_size,
smallest_user_key, largest_user_key, seqno_smallest, seqno_largest,
output_level, status,
bytes_read_logical, bytes_written_logical,
input_count, output_count, notes
```

- `event_index`: global monotonic counter; makes same-timestamp ordering deterministic
- `job_id`: per-DB counter starting at 1; stable join key for all rows of a job
- `smallest_user_key` / `largest_user_key`: raw user key bytes, hex-encoded
- `cf_name`: always `default` (no column families in vanilla LevelDB)
- `bg_thread_id`: OS thread ID via `syscall(SYS_gettid)` on Linux, `GetCurrentThreadId()` on Windows
- `bytes_read_logical` / `bytes_written_logical`: SST file size sums (not record-level logical bytes)
- Empty cells mean the field is not applicable for that event type

## Event Types and Hook Sites

| Event | Emitted from | Condition |
|-------|-------------|-----------|
| `job_start` | `DoCompactionWork()` start; `BackgroundCompaction()` trivial-move path | Always |
| `job_input` | `DoCompactionWork()` start (before `mutex_.Unlock()`); trivial-move path | Once per selected input file |
| `job_output_create` | `OpenCompactionOutputFile()` | On successful file creation |
| `job_output_finish` | `FinishCompactionOutputFile()` | On success, non-empty output only |
| `job_install` | `InstallCompactionResults()` after `LogAndApply()` | Always (success and failure) |
| `job_input_delete` | `InstallCompactionResults()` after successful `LogAndApply()` | Once per input file; logical install-time obsolescence only |
| `job_end` | `DoCompactionWork()` return; trivial-move path | Always |

Trivial moves (handled entirely in `BackgroundCompaction()` without entering `DoCompactionWork()`) emit `job_start`, `job_input`, `job_install`, `job_input_delete`, and `job_end`. There is no `job_output_create` or `job_output_finish` for trivial moves.

## Design Decisions

### `bg_thread_id`
Real OS TID: `syscall(SYS_gettid)` on Linux, `GetCurrentThreadId()` on Windows, `0` on other platforms.

### `job_input_delete`
Represents logical install-time obsolescence (after successful `LogAndApply()`), not physical unlink timing. Physical unlink from `RemoveObsoleteFiles()` is deferred and not tracked in this first pass. The `notes` column says `"logical install-time obsolescence"` on every `job_input_delete` row.

### Trivial moves
Included as first-class jobs. They are a meaningful compaction event (file moves from level N to N+1 with a MANIFEST commit) and their omission would leave gaps in the oracle when validating against ETW traces.

### Compaction reason derivation
Derived in `BackgroundCompaction()` before calling `PickCompaction()`, by inspecting `versions_->current()->compaction_score()` and `versions_->current()->file_to_compact()`. These accessors were added to `Version` as const methods to avoid modifying `VersionSet::PickCompaction()`. Possible values: `"manual"`, `"size"`, `"seek"`.

### `bytes_read_logical` / `bytes_written_logical`
SST file size sums (same as what LevelDB already tracks in `CompactionStats`). These are "bytes of participating SSTables", not record-level logical bytes.

### Trace writer ownership
`CompactionTraceWriter` is owned by `DBImpl` (initialized in constructor, deleted in destructor). It is thread-safe internally (mutex-guarded). The `trace_writer_` pointer is checked at every emission site: when `nullptr`, all tracing is a no-op.

## Verified Output

Sample run (`fillrandom,compact`, 320 keys, 1 KiB each, 64 KiB buffer, 1 MiB max file):

```
Job 1: reason=size  L0->L1  5 inputs → 1 output  bytes_read=273248  bytes_written=180154
Job 2: reason=manual L1->L2  2 inputs → 1 output  bytes_read=244501  bytes_written=210673
```

Full event sequence per job:
```
job_start
job_input × N      (one per selected input file)
job_output_create  (one per output file opened)
job_output_finish  (one per output file finished)
job_install
job_input_delete × N
job_end
```

Column count: 26. All rows validated with Python `csv.reader` — no column count mismatches.

## Deferred (not in this pass)

- Physical unlink timing from `RemoveObsoleteFiles()` (`file_unlink` event)
- Richer failure-specific event types
- Record-level logical byte accounting
- Formal test cases in `db/db_test.cc` (see test plan in `modification_plan.md`)
