# Compaction Oracle Evaluation And Modification Plan

## Bottom line

Yes. This feature is feasible in this LevelDB tree with a modest, localized
change set.

## Main code files related to this modification

- `db/db_impl.cc`
  Core compaction lifecycle implementation. This is the main file for
  `BackgroundCompaction()`, `DoCompactionWork()`,
  `OpenCompactionOutputFile()`, `FinishCompactionOutputFile()`, and
  `InstallCompactionResults()`.
- `db/db_impl.h`
  `DBImpl` declarations. This is where new DB-owned tracing state, helper
  methods, and the job ID counter would be declared.
- `db/version_set.cc`
  Compaction selection logic in `PickCompaction()`, `CompactRange()`, and
  `SetupOtherInputs()`. Relevant for deriving compaction reason and validating
  selected input membership.
- `db/version_set.h`
  Definitions for `VersionSet`, `Compaction`, and accessors over selected input
  files and levels. Relevant if the trace context needs new helper accessors.
- `db/version_edit.h`
  Version-edit structure used during install. Relevant because input deletions
  and output additions are recorded here before `LogAndApply()`.
- `db/filename.h`
  File naming helpers such as `TableFileName()` and log-path helpers. Relevant
  for naming the trace file and output SST files in the CSV.
- `include/leveldb/options.h`
  Public options surface. Relevant if the trace path or enable flag is exposed
  as a DB option.
- `util/posix_logger.h`
  Existing human-readable POSIX logger implementation. Relevant as a contrast:
  useful for understanding why the oracle should not reuse `info_log`.
- `util/windows_logger.h`
  Existing human-readable Windows logger implementation. Same relevance as the
  POSIX logger.
- `util/env_posix.cc`
  POSIX environment implementation. Relevant for timestamp sourcing and, if
  needed, adding an OS thread ID helper on POSIX.
- `db/db_test.cc`
  Existing DB-level tests. Best initial place for trace-validation tests unless
  a dedicated test file is added.

The current compaction path already exposes nearly all lifecycle boundaries that
the oracle needs:

- job selection happens in `VersionSet::PickCompaction()` and
  `VersionSet::CompactRange()`
- real work starts in `DBImpl::DoCompactionWork()`
- output file allocation happens in `DBImpl::OpenCompactionOutputFile()`
- output finalization happens in `DBImpl::FinishCompactionOutputFile()`
- install/commit happens in `DBImpl::InstallCompactionResults()`
- input-file obsolescence is known when `Compaction::AddInputDeletions()` is
  called
- job completion happens when `DBImpl::DoCompactionWork()` returns

That means we do not need to infer the oracle from side effects inside the
filesystem layer. We can instrument the compaction state machine directly.

## Evaluation of the proposed schema

The proposed two-layer model is correct for this codebase:

- `job_*` events are needed for lifecycle and byte-accounting validation
- `file_*` events are needed for true input/output/delete membership validation
- logging only `COMPACTION_START` and `COMPACTION_END` would be insufficient
  because it would not identify true inputs, true outputs, or delete causality

The proposed minimum event set also maps cleanly onto the code:

- `job_start`: emit at the start of `DoCompactionWork()`
- `job_input`: emit once per selected input file before releasing `mutex_`
- `job_output_create`: emit from `OpenCompactionOutputFile()`
- `job_output_finish`: emit from `FinishCompactionOutputFile()`
- `job_install`: emit immediately before and after `InstallCompactionResults()`
- `job_input_delete`: emit for each input file removed by the successful install
- `job_end`: emit at the end of `DoCompactionWork()` with success/failure status

## What is directly available today

The following fields are already available without deeper storage-engine
surgery:

- `trace_ts_us`: from `env_->NowMicros()`
- `job_id`: can be assigned from a new DB-local atomic counter
- `db_name`: from `dbname_`
- `is_manual`: known in `BackgroundCompaction()`
- `is_trivial_move`: known from `Compaction::IsTrivialMove()`
- `source_level`: `c->level()`
- `target_level` / `output_level`: `c->level() + 1`
- true input membership: `compact->compaction->input(which, i)`
- input file size: `FileMetaData::file_size`
- input file key range: `FileMetaData::smallest`, `FileMetaData::largest`
- output file number/name: chosen in `OpenCompactionOutputFile()`
- output file size and key range: captured in `CompactionState::Output`
- install point: `InstallCompactionResults()`
- job duration: `NowMicros()` at start and end
- bytes read logical: already computed as sum of input file sizes
- bytes written logical: already computed as sum of output file sizes

## Gaps and caveats

### 1. `cf_name`

Vanilla LevelDB has no column families. The field should either be:

- emitted as empty, or
- emitted as a fixed literal such as `default`

Empty is cleaner if the CSV is intended to stay engine-neutral.

### 2. `compaction_reason`

This is partially available, but not currently represented as a first-class enum.
We can derive:

- `manual`
- `size`
- `seek`

That derivation belongs near compaction selection in `BackgroundCompaction()`
and `VersionSet::PickCompaction()`.

### 3. `bg_thread_id`

The requirement says OS thread ID if available.

Current LevelDB logging only records `std::this_thread::get_id()` as a textual
thread identifier in the human-readable logger. That is not guaranteed to be the
OS TID. We will add a small platform helper:

- Linux: `syscall(SYS_gettid)`
- Windows: `GetCurrentThreadId()`

**Decision: use real OS TID.** This gives the strongest parity with ETW thread
attribution and is low-cost to implement.

### 4. `smallest_user_key`, `largest_user_key`, `seqno_smallest`, `seqno_largest`

Input-file metadata already stores internal-key bounds, so these can be emitted
for `job_input`.

For `job_start`, the compaction-wide range is also available from the chosen
inputs. If needed, compute it once from the union of inputs just before the work
 begins.

For `job_output_finish`, output smallest/largest internal keys are already
captured in `CompactionState::Output`. Sequence-number bounds can be derived
from those internal keys.

### 5. “logical” bytes

In this tree, the existing `bytes_read` and `bytes_written` compaction stats are
file-size sums:

- read: sum of selected input SST sizes
- written: sum of finished output SST sizes

That matches your validation need well enough for operational comparison with
the reconstruction pipeline, but it is not “logical bytes of records processed”
in a semantic sense. It is “bytes of participating SSTables”.

Recommendation:

- keep the column names `bytes_read_logical` and `bytes_written_logical` if you
  need schema compatibility downstream
- document in code and analysis that these are file-size-based compaction bytes

### 6. `job_input_delete`

This event should be emitted only after successful install, not when input files
are merely chosen for deletion in the edit.

Reason:

- `Compaction::AddInputDeletions()` mutates the pending `VersionEdit`
- actual obsolescence is only meaningful if `LogAndApply()` succeeds

So the right place is inside or immediately after `InstallCompactionResults()`
once `versions_->LogAndApply(...)` returns `OK`.

**Decision: `job_input_delete` means logical install-time obsolescence, not
physical unlink timing.** Physical unlink timing from `RemoveObsoleteFiles()` is
deferred to a later pass.

### 7. trivial move jobs

The current code handles non-manual trivial moves in `BackgroundCompaction()`
without entering `DoCompactionWork()`.

**Decision: include trivial moves as first-class jobs in the oracle.** They
require their own job path with:

- `job_start`
- `job_input`
- `job_install`
- `job_input_delete` (the source-level file is logically obsolete after install)
- `job_end`

There will be no `job_output_create` or `job_output_finish` because no new SST
is written.

This is the main reason the oracle must not be implemented only inside
`DoCompactionWork()`.

## Recommended implementation design

Use a dedicated CSV trace file, not `info_log`.

Reasons:

- `info_log` prepends timestamps and thread IDs in a human-oriented format
- `info_log` is not stable for CSV parsing
- the oracle should stay machine-readable and schema-stable

### New components

1. Extend `Options`

Add an opt-in field such as:

- `const char* compaction_trace_path = nullptr;`

Alternative:

- `bool enable_compaction_trace = false;`
- trace file defaults to `${dbname}/COMPACTION_TRACE.csv`

I prefer the explicit path because it avoids hidden filesystem behavior.

2. Add a small trace writer owned by `DBImpl`

Suggested internal helper:

- `class CompactionTraceWriter`

Responsibilities:

- open a writable file at DB open time
- write the CSV header exactly once
- serialize rows under a mutex
- escape CSV fields conservatively
- flush each row or batch depending on desired durability

This should be separate from `Logger`.

3. Add per-job trace state

Extend `DBImpl::CompactionState` with a trace payload, for example:

- `uint64_t job_id`
- `bool is_manual`
- `bool is_trivial_move`
- `std::string compaction_reason`
- `uint64_t start_ts_us`
- cached `bg_thread_id`
- cached input/output byte totals and file counts

For trivial moves, use a smaller stack-local job context if we do not construct
`CompactionState`.

4. Add a DB-level job counter

Add to `DBImpl`:

- `std::atomic<uint64_t> next_compaction_job_id_{1};`

This gives a stable join key for all rows.

## Exact hook plan

### A. Selection and reason derivation

Touch:

- `db/db_impl.cc`
- optionally `db/version_set.h`

Plan:

- in `BackgroundCompaction()`, determine `is_manual`
- derive `compaction_reason` as `manual`, `size`, or `seek`
- if `c->IsTrivialMove()`, trace that path explicitly
- otherwise allocate `CompactionState` and populate the trace context before
  calling `DoCompactionWork()`

### B. Emit `job_start` and `job_input`

Touch:

- `db/db_impl.cc`
- `DBImpl::DoCompactionWork()`

Plan:

- record `start_ts_us = env_->NowMicros()`
- emit `job_start` before releasing `mutex_`
- compute compaction-wide smallest/largest range from selected inputs
- emit one `job_input` row for every file in `inputs_[0]` and `inputs_[1]`

Important detail:

- emit inputs before any output activity begins, so the log captures the full
  selected membership even if the job later fails

### C. Emit `job_output_create`

Touch:

- `db/db_impl.cc`
- `DBImpl::OpenCompactionOutputFile()`

Plan:

- after file number allocation and filename construction, emit one row with
  `output_file_number`, `output_file_name`, and `output_level`

### D. Emit `job_output_finish`

Touch:

- `db/db_impl.cc`
- `DBImpl::FinishCompactionOutputFile()`

Plan:

- after `builder->Finish()` and after final file size is known, emit one row
- include output bounds from `compact->current_output()->smallest/largest`
- include `bytes_written_logical` as the finished file size for that output

Important detail:

- emit only for successfully finished outputs
- do not emit for abandoned output files on failed compactions unless you want a
  separate failure-only event type

### E. Emit `job_install`

Touch:

- `db/db_impl.cc`
- `DBImpl::InstallCompactionResults()`

Plan:

- emit a `job_install` row after `LogAndApply()` succeeds
- include `status=ok`
- include notes or aggregate counts/bytes if useful

On failure:

- either emit `job_install` with `status=<error>`
- or skip `job_install` and rely on `job_end`

I recommend emitting it on both success and failure because install failure is a
distinct stage boundary.

### F. Emit `job_input_delete`

Touch:

- `db/db_impl.cc`
- `DBImpl::InstallCompactionResults()`

Plan:

- after successful `LogAndApply()`, iterate the same selected input files and
  emit one delete row per input
- use the original file metadata and source level

Important nuance:

- this is “logically obsolete due to install”, not necessarily “physical unlink
  executed at this timestamp”
- that distinction should be stated in `notes`

If you also want physical unlink timing, you would need a second trace source in
`RemoveObsoleteFiles()`. That is optional and separate.

### G. Emit `job_end`

Touch:

- `db/db_impl.cc`
- `DBImpl::DoCompactionWork()`
- `BackgroundCompaction()` trivial-move path

Plan:

- emit at the end with:
  - final status
  - total `bytes_read_logical`
  - total `bytes_written_logical`
  - counts of inputs, outputs, and delete rows
- this event defines the authoritative job duration endpoint

## Schema recommendations

Keep the normalized single-file CSV, but make two small adjustments.

### 1. Add an explicit `event_index`

Suggested extra column:

- `event_index`

Reason:

- makes same-timestamp ordering deterministic within a job
- useful if multiple rows share the same `trace_ts_us`

### 2. Separate logical delete from physical unlink if needed

If downstream analysis cares about ETW delete timing skew, add:

- `job_input_delete` for logical deletion at install
- `file_unlink` for actual filesystem unlink in `RemoveObsoleteFiles()`

If you only need a ground-truth counterpart to reconstructed delete anchoring,
`job_input_delete` alone is enough for the first implementation.

## Validation against the reconstruction table

This oracle will support the intended comparisons well:

- start/end/duration: exact from `job_start` and `job_end`
- true job count: exact by counting successful or all jobs, depending on filter
- thread attribution: exact if OS TID helper is added
- input/output membership: exact from `job_input` and `job_output_finish`
- delete anchoring: exact logical install-driven delete set from
  `job_input_delete`
- input/output bytes: exact as SST-size totals
- structural interpretation: exact from `source_level` and `target_level`

The only comparison that needs careful wording is write amplification:

- `validated_wa = total_output_size / total_input_size` is directly supported
- `bytes_written_logical / bytes_read_logical` will be the same ratio if both
  totals are defined as SST-size sums
- if you later want record-level logical bytes instead, that is a different and
  more invasive instrumentation problem

## Proposed implementation sequence

1. Add an opt-in compaction trace writer and CSV header management.
2. Add a DB-local job ID counter and per-job trace context.
3. Instrument non-trivial compactions in `DoCompactionWork()`,
   `OpenCompactionOutputFile()`, `FinishCompactionOutputFile()`, and
   `InstallCompactionResults()`.
4. Instrument the trivial-move path in `BackgroundCompaction()`.
5. Add tests that run controlled compactions and assert emitted event rows.
6. Extend analysis code to join rows by `job_id` and compare against the
   reconstructed job table.

## Test plan

Add focused tests in `db/db_test.cc` or a new dedicated trace test:

- size-triggered compaction producing one output file
- compaction producing multiple output files
- manual compaction
- seek-triggered compaction if there is already a deterministic way to trigger
  it in tests
- trivial move
- failing compaction path, if fault injection makes this practical

Assertions should cover:

- event ordering per job
- complete input membership
- complete output membership
- byte totals
- source/target levels
- successful install rows
- end status on both success and failure

## Recommended scope for first pass

First pass should implement:

- dedicated CSV trace file
- `job_start`
- `job_input`
- `job_output_create`
- `job_output_finish`
- `job_install`
- `job_input_delete`
- `job_end`
- trivial-move coverage

Defer to a later pass:

- physical unlink timing from `RemoveObsoleteFiles()`
- richer failure-specific event types
- record-level logical byte accounting
- any attempt to reuse `info_log`

## Conclusion

This modification is a good fit for the current codebase.

The clean implementation is to add a dedicated compaction trace writer and emit
normalized CSV rows from the existing compaction lifecycle hooks. The three
material design decisions have been resolved:

- **`bg_thread_id`**: use real OS TID (`syscall(SYS_gettid)` on Linux,
  `GetCurrentThreadId()` on Windows)
- **`job_input_delete`**: logical install-time obsolescence; physical unlink
  timing is deferred
- **trivial moves**: included as first-class jobs in the oracle

These decisions give the strongest ground truth for validating the ETW-based
reconstruction pipeline without overcomplicating the first implementation.
