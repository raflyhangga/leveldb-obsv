# Compaction Job Reconstructor v2 — Implementation Plan

Signal source: `compaction_signals.md`

---

## Phase 1 — Trace Ingestion

1. Define event schema: timestamp, thread ID, syscall name, file path, open flags (if applicable).
2. Parse raw VFS trace into a single ordered event stream sorted by timestamp.
3. Extract file number from each `.ldb` and `MANIFEST-NNNNNN` filename at parse time.

---

## Phase 2 — Open-Phase Boundary Detection

Goal: suppress false-positive `.ldb` creates from `RecoverLogFile()` during `DB::Open()`.

1. Identify main thread TID: the thread that first opens the `LOCK` file.
2. Track all `.ldb` O_WRONLY|O_CREAT events on the main thread — these are recovery-time SST creates; exclude them from job detection.
3. Mark steady-state start: the timestamp of the first `write(MANIFEST-*)` on a thread that is **not** the main thread. All events before this boundary are open-phase; discard for job reconstruction.

---

## Phase 3 — BG Thread Identification

1. After the steady-state boundary, any thread that performs `open(*.ldb, O_WRONLY|O_CREAT)` is a BG compaction/flush thread.
2. Record the BG thread TID on first observation; confirm it stays consistent (LevelDB uses one BG thread).
3. Reject any `.ldb` creates on non-BG, non-main threads as anomalies.

---

## Phase 4 — Per-Thread State Machine

Maintain per BG thread:

- `pending_reads`: ordered list of `.ldb` file numbers opened O_RDONLY since last job start.
- `output_files`: list of `.ldb` file numbers opened O_WRONLY|O_CREAT in current job.
- `manifest_synced`: boolean, set when `fsync(MANIFEST-*)` seen.
- `state`: one of `IDLE | ACTIVE | COMMITTED`.

Transitions:

1. `IDLE → ACTIVE`: on `open(*.ldb, O_WRONLY|O_CREAT)` — record job start timestamp, add file to `output_files`, carry forward `pending_reads` as job inputs.
2. `ACTIVE`: on subsequent `open(*.ldb, O_WRONLY|O_CREAT)` — append to `output_files` (multi-output compaction).
3. `ACTIVE → COMMITTED`: on `fsync(MANIFEST-*)` — record commit timestamp, set `manifest_synced`.
4. `COMMITTED → IDLE`: after collecting post-commit unlinks (see Phase 6), emit job record and reset state.

Trivial-move path (no `.ldb` write):

- Detect as `fsync(MANIFEST-*)` on BG thread with empty `output_files` and empty `pending_reads`.
- Classify immediately as trivial move; emit and reset.

---

## Phase 5 — Job Type Classification

Apply after job reaches COMMITTED:

1. `output_files` empty, `pending_reads` empty → **trivial move**.
2. `pending_reads` empty, `output_files` non-empty → **L0 flush** (memtable input, no SST reads).
3. `pending_reads` non-empty, `output_files` non-empty → **L1+ compaction**.

---

## Phase 6 — Post-Commit Unlink Collection

1. After MANIFEST fsync, enter unlink-collection window on the BG thread.
2. Collect all `unlink(*.ldb)` events until either:
   - Next `open(*.ldb, O_WRONLY|O_CREAT)` is seen (new job starts), or
   - A configurable time window expires (fallback guard).
3. These unlinked file numbers are the deleted input SSTables for the committed job.

---

## Phase 7 — File Number Cross-Check (Signal 5 validation)

For every completed job:

1. Assert `min(output file numbers) > max(input file numbers from pending_reads)`.
2. If assertion fails, flag the job as anomalous and do not emit a normal job record.
3. Log the raw events that caused the violation for manual inspection.

---

## Phase 8 — Job Record Emission

Emit one record per reconstructed job containing:

- Synthetic monotonic job ID.
- Job type: `flush | compact | trivial_move`.
- Job start timestamp (first `.ldb` O_WRONLY open).
- Job commit timestamp (MANIFEST fsync).
- Input file numbers (from `pending_reads`).
- Output file numbers (from `output_files`).
- Deleted file numbers (from post-commit unlinks).
- Anomaly flag if Signal 5 cross-check failed.

---

## Phase 9 — No-Compaction Validation Mode

For workloads expected to produce zero compactions:

1. After steady-state boundary, scan for any `open(*.ldb, O_WRONLY|O_CREAT)` on BG thread.
2. If found, report as unexpected compaction activity.
3. If none found, report clean — no compaction or flush occurred.

---

## Implementation Order

1. Trace parser and event schema (Phase 1).
2. Open-phase boundary and BG thread detection (Phases 2–3).
3. Per-thread state machine with IDLE/ACTIVE/COMMITTED transitions (Phase 4).
4. Job type classifier (Phase 5).
5. Post-commit unlink collector (Phase 6).
6. File number cross-check (Phase 7).
7. Job record emitter (Phase 8).
8. No-compaction validation mode (Phase 9).
9. Integration test against oracle CSV output from `CompactionTraceWriter`.
