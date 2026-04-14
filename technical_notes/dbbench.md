# db_bench Flags Reference

All flags are passed as `--flag=value`. Boolean flags accept `0` or `1`.

---

## Workload Selection

### `--benchmarks=<list>`
**Default:** `fillseq,fillsync,fillrandom,overwrite,readrandom,readrandom,readseq,readreverse,compact,readrandom,readseq,readreverse,fill100K,crc32c,snappycomp,snappyuncomp,zstdcomp,zstduncomp`

Comma-separated list of benchmark names to run in order. Each name maps to a specific operation.

**Write benchmarks** (these create a fresh DB unless noted):

| Name | Description |
|------|-------------|
| `fillseq` | Write `N` key-value pairs in sequential key order, async |
| `fillrandom` | Write `N` key-value pairs in random key order, async |
| `overwrite` | Overwrite `N` keys in random order (reuses existing DB) |
| `fillsync` | Write `N/1000` key-value pairs in random order with `sync=true` (fsync per write) |
| `fill100K` | Write `N/1000` entries with 100 KB values in random order |
| `fillbatch` | Write `N` entries in sequential order using batches of 1000 |

**Read benchmarks** (reuse existing DB):

| Name | Description |
|------|-------------|
| `readseq` | Read `N` entries by iterating forward from first key |
| `readreverse` | Read `N` entries by iterating backward from last key |
| `readrandom` | Read `N` entries by random key lookup; reports hit rate |
| `readmissing` | Read `N` keys that are guaranteed to be missing (key truncated by 1 byte) |
| `readhot` | Read `N` times from a random key within the top 1% of the key range |
| `readrandomsmall` | Like `readrandom` but only `reads/1000` ops |
| `seekrandom` | Perform `N` random seeks using an iterator; reports hit rate |
| `seekordered` | Perform `N` seeks that advance forward (each step is +0..99 keys), creating cache-friendly access patterns |
| `readwhilewriting` | One background thread writes continuously; other threads do `readrandom`. Uses `--threads` read threads plus 1 writer. |

**Delete benchmarks**:

| Name | Description |
|------|-------------|
| `deleteseq` | Delete `N` keys in sequential order |
| `deleterandom` | Delete `N` keys in random order |

**Meta / utility**:

| Name | Description |
|------|-------------|
| `open` | Repeatedly open the DB (`N/10000` times); measures open cost |
| `compact` | Call `CompactRange(nullptr, nullptr)` — compacts the entire DB |
| `stats` | Print `leveldb.stats` property to stdout |
| `sstables` | Print `leveldb.sstables` property to stdout |
| `heapprofile` | Dump a heap profile to `<db>/heap-NNNN` (platform-dependent) |
| `crc32c` | Compute CRC32C on 4 KB chunks until 500 MB processed; measures raw checksum throughput |
| `snappycomp` | Compress 1 GB of synthetic data with Snappy; measures throughput |
| `snappyuncomp` | Compress then decompress 1 GB with Snappy; measures decompression throughput |
| `zstdcomp` | Compress 1 GB with Zstd at `--zstd_compression_level` |
| `zstduncomp` | Compress then decompress 1 GB with Zstd; measures decompression throughput |

---

## Scale Parameters

### `--num=<int>`
**Default:** `1000000`

Number of key-value pairs to write (or the key-space size for reads/deletes). Several benchmarks scale off this:
- `fillsync` and `fill100K` use `num/1000`
- `open` uses `num/10000`
- `readrandom` / `readmissing` / etc. draw random keys from `[0, num)`

### `--reads=<int>`
**Default:** `-1` (uses `--num`)

Number of read operations to perform. Negative means use `--num`. Applies to all read benchmarks.

### `--threads=<int>`
**Default:** `1`

Number of concurrent threads for benchmarks that support parallelism. `readwhilewriting` spawns one extra writer thread on top of this.

### `--value_size=<int>`
**Default:** `100` (bytes)

Size of each value in bytes. `fill100K` overrides this to `100000` regardless.

---

## Storage Engine Tuning

### `--write_buffer_size=<int>`
**Default:** `4194304` (4 MiB, from `Options::write_buffer_size`)

Memtable size in bytes. When the memtable fills, it is flushed to an L0 SST. Smaller values trigger flushes and compactions more frequently. Enforced minimum in `db_impl.cc`: 64 KiB.

### `--max_file_size=<int>`
**Default:** `2097152` (2 MiB, from `Options::max_file_size`)

Target size for SST files in bytes. Smaller files increase the number of files per level and can affect compaction granularity. Enforced minimum in `db_impl.cc`: 1 MiB.

### `--block_size=<int>`
**Default:** `4096` (4 KiB, from `Options::block_size`)

Approximate size of uncompressed user data per SST block before compression. Larger blocks improve sequential read throughput but increase read amplification for point lookups.

### `--cache_size=<int>`
**Default:** `-1` (LevelDB's internal default)

Size of the LRU block cache in bytes. Negative means no explicit cache is created (LevelDB uses an 8 MiB default internally). Set to `0` to disable caching entirely. Useful for benchmarking cold-read performance.

### `--open_files=<int>`
**Default:** `1000` (from `Options::max_open_files`)

Maximum number of SST files to keep open simultaneously. Increase for large databases with many files; decrease to simulate constrained file-descriptor environments.

### `--bloom_bits=<int>`
**Default:** `-1` (disabled)

Bits per key for the Bloom filter policy. Negative disables the filter. `10` is a common value (≈1% false positive rate). Filters reduce disk reads for negative lookups.

---

## Compression

### `--compression=<0|1>`
**Default:** `1` (enabled)

Whether to use compression. `1` = Snappy; `0` = no compression. Applies to SST writes.

### `--compression_ratio=<float>`
**Default:** `0.5`

Controls the compressibility of synthetic value data. `0.5` means generated values compress to about 50% of their original size. `1.0` = incompressible; `0.0` = maximally compressible. Only affects generated benchmark data, not how LevelDB compresses it.

### `--zstd_compression_level=<int>`
**Default:** `1`

Zstd compression level for `zstdcomp` / `zstduncomp` benchmarks. Higher values trade CPU for better compression ratios. **Note:** this flag is defined but not parsed from the command line in `main()` — the value is always 1.

---

## Database State

### `--db=<path>`
**Default:** `<tmp_dir>/dbbench`

Path to the database directory. The directory is created if it doesn't exist. For compaction tracing experiments, use a dedicated path per run (e.g., `/tmp/ldb-b`).

### `--use_existing_db=<0|1>`
**Default:** `0` (destroy and recreate)

If `1`, skip destroying the existing database at startup and when a `fresh_db` benchmark runs. Benchmarks that require a fresh DB (e.g. `fillseq`, `fillrandom`) will be skipped with a message. Useful for read benchmarks on a previously populated DB.

### `--reuse_logs=<0|1>`
**Default:** `0`

If `1`, sets `Options::reuse_logs = true`, which reuses existing WAL log and MANIFEST files when reopening a database instead of rotating them.

---

## Output and Diagnostics

### `--histogram=<0|1>`
**Default:** `0`

If `1`, prints a microsecond-latency histogram after each benchmark. Operations taking >20 ms also print a warning to stderr in real time.

### `--comparisons=<0|1>`
**Default:** `0`

If `1`, wraps the default comparator with a counting comparator and prints total key comparison counts after each benchmark. Useful for understanding seek/lookup amplification.

---

## Key Shape

### `--key_prefix=<int>`
**Default:** `0`

Prepends `N` bytes of `'a'` before the 16-digit zero-padded integer key. Total key size = `16 + key_prefix`. Use this to simulate longer keys or test prefix-compression effects.

---

## Compaction Oracle (fork-specific)

### `--compaction_trace_path=<path>`
**Default:** `""` (disabled)

If non-empty, enables the `CompactionTraceWriter` and writes a CSV compaction oracle trace to this file. This is a custom addition to this fork — not present in upstream LevelDB. Maps to `Options::compaction_trace_path`.

---

## Quick-Reference Summary

```
--benchmarks=fillrandom      # which workloads to run
--num=1000000                # key-value pair count / key space
--reads=-1                   # read ops (-1 = use --num)
--threads=1                  # concurrent threads
--value_size=100             # bytes per value
--write_buffer_size=4194304  # memtable size (bytes)
--max_file_size=2097152      # SST file size target (bytes)
--block_size=4096            # SST block size (bytes)
--cache_size=-1              # block cache size (-1 = default)
--open_files=1000            # max open SST file descriptors
--bloom_bits=-1              # bloom filter bits per key (-1 = off)
--compression=1              # 1=snappy, 0=none
--compression_ratio=0.5      # synthetic data compressibility
--db=/tmp/ldb-bench          # database directory
--use_existing_db=0          # 0 = destroy on start
--reuse_logs=0               # reuse WAL/MANIFEST on reopen
--histogram=0                # latency histogram output
--comparisons=0              # count key comparisons
--key_prefix=0               # bytes of 'a' prefix on each key
--compaction_trace_path=     # oracle CSV output (fork-only)
```
