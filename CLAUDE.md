# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Repo Is

This is a fork of Google's LevelDB (v1.23.0) being instrumented with a **compaction oracle** — a dedicated CSV trace writer that logs compaction lifecycle events for validating an ETW-based compaction-job reconstruction pipeline. See `research_notes/modification_plan.md` for the full design and `research_notes/controlled_db_bench_workloads.md` for benchmark workloads.

## Build

```bash
# Full build (Debug)
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug .. && cmake --build .

# Release build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Build only db_bench
cmake --build build --target db_bench -j

# Build only tests
cmake --build build --target leveldb_tests -j
```

Submodules must be initialized: `git submodule update --init --recursive`

## Running Tests

```bash
# Run all tests
cd build && ctest --output-on-failure

# Run a specific test binary
./build/db_test
./build/version_set_test
./build/db_format_test

# Run a specific test case within a binary (GTest filter)
./build/db_test --gtest_filter=DBTest.CompactionsGenerateMultipleFiles
```

Primary test file for DB-level behavior: `db/db_test.cc`

## Code Style

Conforms to [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html). Format with:
```bash
clang-format -i --style=file <file>
```

No C++ exceptions (`-fno-exceptions`) and no RTTI (`-fno-rtti`).

## Architecture Overview

### Storage Engine Layers

- **Public API**: `include/leveldb/db.h`, `options.h`, `iterator.h`, `write_batch.h`, `slice.h`, `status.h`, `env.h`
- **DB implementation**: `db/db_impl.{h,cc}` — the core `DBImpl` class; owns the compaction background thread, mutex, memtable, and all lifecycle methods
- **Version management**: `db/version_set.{h,cc}` — `VersionSet` tracks level/file state; `PickCompaction()` and `CompactRange()` select jobs; `LogAndApply()` commits version edits
- **SSTable layer**: `table/` — building and reading sorted string tables
- **Write path**: WAL log (`db/log_writer.cc`) → memtable (`db/memtable.{h,cc}`, backed by `db/skiplist.h`) → flush to Level-0 SST
- **OS abstraction**: `include/leveldb/env.h` with POSIX implementation in `util/env_posix.cc`

### Compaction Lifecycle (key hook sites)

The compaction flow in `db/db_impl.cc` drives all oracle instrumentation:

1. `BackgroundCompaction()` — selects job, determines `is_manual`/`is_trivial_move`, derives `compaction_reason`
2. `DoCompactionWork()` — main work loop; emit `job_start` and `job_input` here
3. `OpenCompactionOutputFile()` — emit `job_output_create`
4. `FinishCompactionOutputFile()` — emit `job_output_finish`
5. `InstallCompactionResults()` → `versions_->LogAndApply()` — emit `job_install` and `job_input_delete` on success
6. `DoCompactionWork()` return — emit `job_end`

Trivial moves bypass `DoCompactionWork()` entirely and need their own oracle path in `BackgroundCompaction()`.

### Planned New Components (from `modification_plan.md`)

- `class CompactionTraceWriter` — owned by `DBImpl`, writes CSV header once at open, serializes rows under mutex
- `Options::compaction_trace_path` — opt-in field to enable tracing
- `std::atomic<uint64_t> next_compaction_job_id_` in `DBImpl` — stable join key for all rows
- Per-job trace context added to `DBImpl::CompactionState`

### Key Constants

- Level-0 compaction trigger: 4 files (`db/dbformat.h`)
- Level-1 size limit: ~10 MiB; grows 10× per level (`db/version_set.cc`)
- `write_buffer_size` runtime minimum: 64 KiB; `max_file_size` minimum: 1 MiB (enforced in `db/db_impl.cc`)

## Benchmark Workloads

See `research_notes/controlled_db_bench_workloads.md` for three controlled `db_bench` workloads (pure-write/no-compaction, single L0→L1 compaction, multi-level compaction) used to validate the oracle output.

```bash
# Example: trigger one L0→L1 compaction
./build/db_bench \
  --db=/tmp/ldb-b --benchmarks=fillrandom --threads=1 \
  --compression=0 --num=320 --value_size=1024 \
  --write_buffer_size=65536 --max_file_size=1048576
```
