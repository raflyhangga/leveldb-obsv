# How does compaction thread got triggered?

Compaction background work is scheduled through env_->Schedule(&DBImpl::BGWork, this), not by creating a dedicated thread directly in DBImpl (db/db_impl.cc:752, db/db_impl.cc:753).

Scheduling conditions (must pass in MaybeScheduleCompaction)

- Not already scheduled: !background_compaction_scheduled_ (db/db_impl.cc:742)
- Not shutting down: !shutting_down_ (db/db_impl.cc:744)
- No prior bg error: bg_error_.ok() (db/db_impl.cc:746)
- There is work: at least one of:
    - imm_ != nullptr (immutable memtable to flush), or
    - manual_compaction_ != nullptr, or
    - versions_->NeedsCompaction() (db/db_impl.cc:748, db/db_impl.cc:749)

When MaybeScheduleCompaction() is invoked

- After DB open/recovery (db/db_impl.cc:1908)
- When write path rotates full memtable to imm_ (db/db_impl.cc:1766, db/db_impl.cc:1771)
- After read stats indicate seek pressure (UpdateStats) (db/db_impl.cc:1529, db/db_impl.cc:1530)
- After RecordReadSample() signals need (db/db_impl.cc:1552, db/db_impl.cc:1553)
- Manual compaction request path (CompactRange) (db/db_impl.cc:697, db/db_impl.cc:699)
- At end of each background call to chain more work if needed (db/db_impl.cc:776)

What runs once scheduled

- BackgroundCall() executes scheduled work if not shutting down and no bg error (db/db_impl.cc:761-db/db_impl.cc:769)
- BackgroundCompaction() does:
    - First priority: flush imm_ via CompactMemTable() and return early (db/db_impl.cc:787-db/db_impl.cc:793)
    - Else pick compaction:
        - manual: CompactRange(...)
        - automatic: PickCompaction() (size/seek driven) (db/db_impl.cc:809-db/db_impl.cc:823)

So practically: compaction is triggered by memtable rollover, manual requests, or version-level compaction pressure (size/seek), then gated by scheduler state/shutdown/error checks.

---

# Recommendation for eBPF Reconstructor (Thread + Job Classification)

## Key conclusion

There is no dedicated "compaction-only thread" in this fork. Work runs on the
single `Env::Schedule` background worker thread, which executes:

- immutable memtable flushes (`CompactMemTable()`),
- full compactions (`DoCompactionWork()`),
- trivial-move installs (inside `BackgroundCompaction()`),
- and can interleave these within one `BackgroundCompaction()` call.

Therefore, thread identity and job identity must be separated:

- **thread identity**: background worker thread (BG TID),
- **job type identity**: flush vs full compaction vs trivial move.

## Why previous LOCK-based detection fails

Using "thread that opens `LOCK` first" is incorrect. `LOCK` activity appears in:

- DB open/recovery,
- DB close/destructor,
- `DestroyDB`,

and is not specific to background compaction execution.

## Recommended detection pipeline

1. Identify main TID
- TID that performs early `DB::Open`/`Recover`/`LOCK` work.

2. Identify BG worker TID
- Non-main TID that repeatedly commits version edits via:
  `write(MANIFEST-*)` + `fsync/fdatasync(MANIFEST-*)` after open steady-state.

3. Build BG-thread "episodes" ending at each MANIFEST sync
- Treat each BG MANIFEST sync as a commit boundary for one install event.

4. Classify each episode
- **Full compaction**:
  - contains compaction output create (`open *.ldb O_WRONLY|O_CREAT` from
    `OpenCompactionOutputFile` path), and
  - typically has `.ldb` input reads before output create on same BG TID.
- **Memtable flush**:
  - contains `.ldb` output create from `WriteLevel0Table/BuildTable`,
  - no prior `.ldb` input SST reads for that episode.
- **Trivial move**:
  - MANIFEST commit with no `.ldb` output create in the episode.

5. Do not use `LOCK`, `MANIFEST` count, or `unlink` alone as compaction count
- They are useful corroboration signals, not primary job-type discriminators.

## Verified with workload B (db_bench `fillrandom,compact`)

Observed in run dated 2026-04-22:

- Main thread: `tid=1014842` (open/recover/LOCK).
- BG worker: `tid=1014844`.
- BG MANIFEST sync count: **11**.
- Full compaction outputs (`OpenCompactionOutputFile`): **3**
  - `000017.ldb`, `000021.ldb`, `000022.ldb`.
- Memtable flush outputs (`WriteLevel0Table`/`BuildTable`): **8**
  - `000005`, `000007`, `000009`, `000011`, `000013`, `000015`, `000018`,
    `000020`.

So:

- `11` MANIFEST syncs on BG thread = `8` memtable flush installs + `3` full
  compaction installs.
- Ground-truth full compaction count is the `3` episodes with compaction-output
  creation, not total BG MANIFEST sync count.
