# Controlled `db_bench` Workloads

## Goal

Use `db_bench` to generate controlled workloads for validating the compaction-job pipeline:

1. A pure-write case that only causes memtable flushes and should produce no compaction windows.
2. A deterministic forced-compaction case that should produce one clear automatic Level-0 to Level-1 compaction job.
3. A larger multi-level case that drives compaction beyond Level-1 and produces multiple overlapping job windows.

## Relevant Repo Constraints

- `db_bench` exposes:
  - `--write_buffer_size`
  - `--max_file_size`
- In this repo, LevelDB starts Level-0 compaction at 4 files.
- This repo clamps:
  - `write_buffer_size >= 64 KiB`
  - `max_file_size >= 1 MiB`

That means the practical "small" settings for controlled runs are:

```text
write_buffer_size = 65536
max_file_size = 1048576
```

## Related Code Files

- `benchmarks/db_bench.cc`
  - Defines the `db_bench` workloads such as `fillseq`, `fillrandom`, and `compact`.
  - Parses benchmark CLI flags including `--write_buffer_size`, `--max_file_size`, `--threads`, `--num`, `--value_size`, and `--compression`.
  - Applies those flags into `leveldb::Options` before opening the database.
- `db/dbformat.h`
  - Defines the Level-0 compaction trigger constants.
  - In this repo, automatic Level-0 compaction starts at 4 files.
- `db/version_set.cc`
  - Defines the per-level size thresholds used for automatic compaction.
  - In this repo, Level-1 starts compacting around 10 MiB total size, and each higher level grows by 10x.
  - Output SSTs still target `max_file_size`, so with the minimum settings below, compaction output files stay near 1 MiB.
- `db/db_impl.cc`
  - Clamps runtime option values.
  - Enforces the practical lower bounds used in these experiments:
    - `write_buffer_size >= 64 KiB`
    - `max_file_size >= 1 MiB`
- `include/leveldb/options.h`
  - Defines default option values for LevelDB, including default write buffer and file sizing.
- `CMakeLists.txt`
  - Builds the `db_bench` target from `benchmarks/db_bench.cc`.

## Build

If needed:

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target db_bench -j
```

## Workload A: Pure Write, No Compaction Yet

### Intent

Create a small write-only workload that causes only flushes and stays below the automatic Level-0 compaction trigger.

### Expected Result

- No compaction windows
- Pipeline output should be empty
- Serves as a sanity check for false positives

### Command

```bash
rm -rf /tmp/ldb-a
./build/db_bench \
  --db=/tmp/ldb-a \
  --benchmarks=fillseq \
  --threads=1 \
  --compression=0 \
  --num=100 \
  --value_size=1024 \
  --write_buffer_size=65536 \
  --max_file_size=1048576
```

### Why This Should Stay Quiet

- `100 * 1024` bytes is about 100 KiB of value data.
- With a 64 KiB write buffer, this should generate only a small number of flushes.
- It should stay below the 4-file Level-0 compaction trigger.
- Therefore the pipeline should detect no compaction job.

## Workload B: Forced Automatic Compaction

### Intent

Trigger one clear automatic compaction by filling Level-0 until it crosses the compaction trigger.

### Expected Result

- One clear compaction job
- Clean high-level pattern:

```text
read (input SST)
write (output SST)
delete (input SST)
```

### Command

```bash
rm -rf /tmp/ldb-b
./build/db_bench \
  --db=/tmp/ldb-b \
  --benchmarks=fillrandom \
  --threads=1 \
  --compression=0 \
  --num=320 \
  --value_size=1024 \
  --write_buffer_size=65536 \
  --max_file_size=1048576
```

### Why `fillrandom`

`fillrandom` is preferred over `fillseq` here because overlapping key ranges are more likely to keep flushed SSTs in Level-0, which makes the automatic Level-0 to Level-1 compaction path deterministic.

### Tuning Notes

- If compaction does not trigger, increase `--num` to `384` or `448`.
- If more than one compaction job appears, reduce `--num` slightly.
- Keep `--threads=1` to minimize background noise and preserve a clean trace.
- Keep `--compression=0` to reduce variability.

## Workload C: Multi-level Compaction

### Intent

Create enough sustained overlap to trigger repeated `L0 -> L1` work and then push accumulated `L1` data over its size limit so `L1 -> L2` compaction also appears.

### Expected Result

- Multiple compaction jobs instead of one isolated window
- Overlapping or back-to-back job windows in the trace as background compaction keeps catching up
- More realistic job complexity than workload B because the pipeline should see both:
  - repeated `L0 -> L1` style jobs
  - deeper `L1 -> L2` jobs with larger fan-in and fan-out

### Command

```bash
rm -rf /tmp/ldb-c
./build/db_bench \
  --db=/tmp/ldb-c \
  --benchmarks=fillrandom,stats \
  --threads=1 \
  --compression=0 \
  --num=50000 \
  --value_size=1024 \
  --write_buffer_size=65536 \
  --max_file_size=1048576
```

### Why This Reaches Deeper Levels

- `fillrandom` keeps key ranges overlapping, so flush output tends to stay in `L0` and repeatedly triggers `L0 -> L1` compaction.
- `L1` compaction is size-based, not file-count based.
- In this repo, `L1` starts compacting at about `10 MiB`.
- With `max_file_size=1048576`, that means roughly ten 1 MiB files are enough to start `L1 -> L2`.
- A `50000`-entry run at `value_size=1024` is large enough to keep `L0 -> L1` busy long enough for `L1` to spill into `L2`.

### Validation Snapshot

The command above was validated locally with `fillrandom,stats` and produced this final level shape:

```text
Level  Files Size(MB) Time(sec) Read(MB) Write(MB)
--------------------------------------------------
  0        5        0        21        0        50
  1       11       10        49      979       971
  2       27       26         6      129       123
```

That is the target pattern for the downstream pipeline: live data remains in both `L1` and `L2`, and the compaction accounting shows substantial traffic at more than one level.

### Tuning Notes

- If `L2` does not appear, raise `--num` to `60000` or `75000`.
- If the trace becomes too noisy for analysis, keep the same flags and reduce only `--num` until `L2` is still present but the run is shorter.
- Keep `--threads=1` and `--compression=0` so the added complexity comes from compaction depth, not foreground concurrency or compression variance.

## Trace Collection

If the compaction-job pipeline consumes syscall or file-operation traces, wrap the same benchmark commands with the tracer. Example with `strace`:

```bash
strace -ff -ttt -o /tmp/trace-a ./build/db_bench ...
strace -ff -ttt -o /tmp/trace-b ./build/db_bench ...
strace -ff -ttt -o /tmp/trace-c ./build/db_bench ...
```

Replace `...` with the full workload command arguments above.

## Validation Summary

### A. Pure write

- Small dataset
- Flushes only
- Expected: no compaction windows
- Pipeline should output nothing

### B. Forced compaction

- Small write buffer
- Small effective setup
- Fill Level-0 until automatic compaction is triggered
- Expected: one compaction job with the pattern:

```text
read input SST
write output SST
delete input SST
```

### C. Multi-level compaction

- Small write buffer
- Large enough random-write workload to saturate repeated `L0 -> L1` compaction
- Push accumulated `L1` data past its size threshold so `L1 -> L2` also appears
- Expected: multiple overlapping or adjacent compaction jobs rather than one clean window
- More realistic for pipeline evaluation because the trace should contain mixed-depth compaction behavior
