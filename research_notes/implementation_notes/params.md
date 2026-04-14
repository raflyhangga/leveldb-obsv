`--threads = 1 `
Eliminates concurrent foreground writers. LevelDB always uses exactly one background compaction thread regardless of this setting; the risk with threads > 1 is non-deterministic write ordering — multiple writers race to fill the memtable, so the precise sequence of flush triggers varies by OS scheduling, making traces hard to reason about and reproduce.

This setting is only relevant for the controlled validation workloads (A/B/C). The oracle itself is safe under concurrent writes in real-life workloads: compaction events always originate from the single background thread, and the trace writer operates under LevelDB's existing mutex so no events are lost or racily written regardless of foreground writer count. The reason to keep --threads=1 for validation is so the expected trace output is predictable enough to verify correctness against — once correctness is confirmed, the oracle can be trusted against concurrent production traffic.

`--compresion=0 `
No Snappy/Zstd means actual byte sizes are deterministic — value bytes written == bytes flushed, making the math predictable 

`--write_buffer_size = 65536 (64 KB)`
This is the minimum allowed by db_impl.cc's runtime clamp. Using the minimum maximises the number of flushes per unit of data written, which directly controls how fast L0 SST files accumulate

`--max_file_size = 1048576 (1 MB)`
the minimum allowed by the clamp. Smaller files = more files per level = faster level-size thresholds are crossed = compaction is reachable with smaller --num values

# Workload A
`--benchmarks=fillseq  --num=100  --value_size=1024`

--benchmarks=fillseq
Sequential writes — keys are monotonically increasing so each flush's key range is disjoint. Disjoint ranges allow flushed L0 files to be immediately pushed to L1 without triggering a compaction job (trivial move path), keeping L0 file count low

--num 100
100 × 1 KiB = ~100 KiB of values. With a 64 KiB write buffer this produces only 1–2 flushes — well under the 4-file L0 trigger 

--value_size=1024
1 KiB values make the byte count predictable. num × value_size directly estimates data volume

# Workload B
--benchmarks=fillrandom
Random writes produce overlapping key ranges across flushed SSTs. Overlapping ranges prevent trivial moves, so L0 files pile up instead of being pushed down immediately — this is what makes the 4-file trigger actually fire

--num=450
by experiment

# Workload C
--benchmarks=fillrandom,stats 
fillrandom again for overlapping ranges (same reasoning as B). stats appends a stats dump at the end showing the final per-level file/size counts — used to confirm L2 was actually reached   

--num=50000 
50000 × 1 KiB = ~50 MiB of raw values. This is large enough to keep L0→L1 compaction firing repeatedly and push L1 past its ~10 MiB size threshold, which triggers L1→L2 compaction
