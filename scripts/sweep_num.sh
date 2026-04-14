#!/usr/bin/env bash
# sweep_num.sh — vary --num across the compaction threshold and record job counts
#
# Usage:  ./scripts/sweep_num.sh [output_dir]
# Output: one CSV file per run in output_dir, plus a summary TSV at output_dir/summary.tsv
#
# Reads:  ./build/db_bench (must be already built)
# Writes: /tmp/sweep-runs/<num>/ directories for DB data
#         <output_dir>/<num>.csv for per-run traces
#         <output_dir>/summary.tsv

set -euo pipefail

BENCH=./build/db_bench
OUT_DIR="${1:-/tmp/sweep-out}"
DB_BASE="/tmp/sweep-runs"
SUMMARY="$OUT_DIR/summary.tsv"

# --- Parameter ranges to test ---
# Workload-A territory (expect 0 compactions)
A_NUMS=(100 200)
# Workload-B territory (find the threshold)
B_NUMS=(450 500)
# Workload-C territory (expect many compactions)
C_NUMS=(1000 1500 2000 10000 50000)

ALL_NUMS=("${A_NUMS[@]}" "${B_NUMS[@]}" "${C_NUMS[@]}")

mkdir -p "$OUT_DIR" "$DB_BASE"
rm -f "$SUMMARY"

# Header
printf "num\tjob_starts\tjob_completions\ttrivial_moves\tnormal_jobs\n" >> "$SUMMARY"

echo "=== num sweep: write_buffer_size=65536 value_size=1024 fillrandom ==="
echo ""

for num in "${ALL_NUMS[@]}"; do
    db_path="$DB_BASE/$num"
    trace="$OUT_DIR/${num}.csv"

    rm -rf "$db_path"
    rm -f  "$trace"

    "$BENCH" \
        --db="$db_path" \
        --benchmarks=fillrandom \
        --threads=1 \
        --compression=0 \
        --num="$num" \
        --value_size=1024 \
        --write_buffer_size=65536 \
        --max_file_size=1048576 \
        --compaction_trace_path="$trace" \
        2>/dev/null

    if [[ ! -f "$trace" ]]; then
        echo "num=$num  [no trace file — 0 compactions]"
        printf "%d\t0\t0\t0\t0\n" "$num" >> "$SUMMARY"
        continue
    fi

    # Count initiated jobs (job_start) and completed jobs (job_end status="ok")
    job_starts=$(awk -F',' 'NR>1 && $4=="job_start"' "$trace" | wc -l)
    job_completions=$(awk -F',' 'NR>1 && $4=="job_end" && $21=="\"ok\""' "$trace" | wc -l)
    trivial=$(awk -F',' 'NR>1 && $4=="job_end" && $21=="\"ok\"" && $8=="1"' "$trace" | wc -l)
    normal=$(( job_completions - trivial ))

    printf "num=%-5d  job_starts=%-3d  job_completions=%-3d  (trivial=%-2d  normal=%-2d)\n" \
        "$num" "$job_starts" "$job_completions" "$trivial" "$normal"

    printf "%d\t%d\t%d\t%d\t%d\n" "$num" "$job_starts" "$job_completions" "$trivial" "$normal" >> "$SUMMARY"
done

echo ""
echo "Summary written to: $SUMMARY"
echo "Per-run traces in:  $OUT_DIR/"
