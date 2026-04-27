# Compaction Detection Signals from VFS Traces

Source of truth: `db/db_impl.cc`, `db/version_set.cc`, `db/builder.cc`.

---

## Why `unlink` alone is unreliable

`RemoveObsoleteFiles()` is called from three sites:

| Call site | Thread | When |
|---|---|---|
| `DB::Open()` line 1862 | Main thread | After recovery, cleans stale files from prior runs |
| `CompactMemTable()` line 622 | BG thread | After L0 flush commits |
| `BackgroundCompaction()` line 920 | BG thread | After full compaction commits |

In a no-compaction workload, `DB::Open()` still calls `RemoveObsoleteFiles()` and unlinks
old WAL logs, old MANIFEST files, and dead SSTables left by prior process runs.
That is the source of false-positive unlinking.

---

## Signal 1 — `.ldb` open-for-write

**Syscall:** `open(NNNNNN.ldb, O_WRONLY|O_CREAT)`

**Source:** `OpenCompactionOutputFile()` → `env_->NewWritableFile(fname, &compact->outfile)`
(`db/db_impl.cc:984`), and `BuildTable()` → `env->NewWritableFile(fname, &file)`
(`db/builder.cc:26`).

**Why reliable:** While the DB is in steady-state (past the open phase), nothing creates
`.ldb` files for writing except compaction output and L0 flush. Normal writes go exclusively
to `.log` WAL files.

**Caveat:** During `DB::Open()`, `RecoverLogFile()` may call `WriteLevel0Table()` →
`BuildTable()` on the main thread. Filter by excluding SST creates that happen on the main
thread early in the process lifetime, before the first `MaybeScheduleCompaction()` returns.

**False-positive rate in no-compaction steady-state:** zero.

---

## Signal 2 — MANIFEST write after initial open

**Syscall sequence:** `write(MANIFEST-NNNNNN)` → `fsync(MANIFEST-NNNNNN)`

**Source:** `VersionSet::LogAndApply()` lines 824–826 (`db/version_set.cc`).
Called by every compaction/flush install path:
- `InstallCompactionResults()` → `versions_->LogAndApply()` (`db/db_impl.cc:1093`)
- `CompactMemTable()` → `versions_->LogAndApply()` (`db/db_impl.cc:614`)
- Trivial-move path → `versions_->LogAndApply()` (`db/db_impl.cc:846`)

**Why reliable:** After the initial open, MANIFEST is only written when a version edit is
committed, which only happens on compaction/flush install. The initial open's `LogAndApply`
runs on the main thread; all subsequent ones run on the BG thread.

**Distinguishing from open-time:** MANIFEST writes during open happen on the main thread.
Steady-state MANIFEST writes happen on the BG compaction thread.

---

## Signal 3 — `.ldb` read-before-write on the same thread

**Syscall pattern:**
```
open(NNNNNN.ldb, O_RDONLY)   × N    ← input SSTables via TableCache
...
open(MMMMMM.ldb, O_WRONLY|O_CREAT)  ← output SSTable
```
Both on the same BG thread, with M > all N (monotonic file number).

**Source of reads:** `DoCompactionWork()` → `versions_->MakeInputIterator()` → `TableCache`
→ `NewRandomAccessFile()`. Source of write: `OpenCompactionOutputFile()` (same function chain).

**Distinguishes L1+ compaction from L0 flush:**
- **L0 flush** (`WriteLevel0Table` → `BuildTable`): input is a memtable iterator — no `.ldb`
  reads. BG thread creates a new `.ldb` with no prior `.ldb` reads on that thread.
- **L1+ compaction** (`DoCompactionWork`): BG thread reads N existing `.ldb` files, then
  creates one or more new `.ldb` files.

---

## Signal 4 — Ordered temporal cluster on the BG thread

Full compaction produces this invariant sequence on a single BG thread:

```
open(NNNNNN.ldb, O_WRONLY|O_CREAT)    OpenCompactionOutputFile
write(NNNNNN.ldb)  × N                 TableBuilder::Add blocks
fsync(NNNNNN.ldb)                       FinishCompactionOutputFile → outfile->Sync()
close(NNNNNN.ldb)                       FinishCompactionOutputFile → outfile->Close()
  [repeat open/write/fsync/close for each output file]
write(MANIFEST-MMMMMM)                  LogAndApply → descriptor_log_->AddRecord()
fsync(MANIFEST-MMMMMM)                  LogAndApply → descriptor_file_->Sync()
unlink(old_NNNNNN.ldb)  × M            RemoveObsoleteFiles
```

An `unlink` without a preceding `.ldb` create + MANIFEST sync on the same thread is **not**
a compaction — discard it.

The MANIFEST sync is the atomicity boundary: it marks the point where the version edit is
durable and the compaction is considered committed.

---

## Signal 5 — File number monotonicity

LevelDB uses a single monotonic counter (`VersionSet::next_file_number_`, incremented by
`NewFileNumber()`). Every file — WAL, SST, MANIFEST — gets a unique ascending number.

Compaction output SST numbers are always strictly greater than all input SST numbers selected
for that job. If you record file numbers at open-for-write time, you can verify:

```
output_file_number > max(input_file_numbers read on same thread)
```

This provides a cross-check: if a thread creates file N and has been reading files with
numbers > N, that is not a normal compaction output.

---

## Recommended reconstruction strategy

| Priority | Signal | Use |
|---|---|---|
| 1 | `open(*.ldb, O_WRONLY\|O_CREAT)` on BG thread | Job start / output file created |
| 2 | `fsync(MANIFEST-*)` on BG thread | Job commit boundary |
| 3 | `.ldb` reads before `.ldb` write on same thread | Distinguish L1+ compaction vs L0 flush |
| 4 | Temporal cluster (create → write → fsync → close → MANIFEST sync → unlinks) | Full job reconstruction |
| 5 | output file# > all input file#s on same thread | Cross-check: reject misattributed jobs |
| — | `unlink` alone | Discard as primary signal; use only as post-commit corroboration |

**Primary detector for no-compaction validation:** `open(*.ldb, O_WRONLY|O_CREAT)` in
steady-state. If this event appears, a compaction or flush is happening regardless of
unlink activity.
