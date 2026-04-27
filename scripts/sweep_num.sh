#!/usr/bin/env bash
# sweep_num.sh — vary --num across the compaction threshold and record compaction job counts
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
B_NUMS=(450 500 600 700 800 900)
# Workload-C territory (expect many compactions)
C_NUMS=(1000 1500 2000 10000 50000)

ALL_NUMS=("${A_NUMS[@]}" "${B_NUMS[@]}" "${C_NUMS[@]}")

mkdir -p "$OUT_DIR" "$DB_BASE"
rm -f "$SUMMARY"

# Header
printf "num\tcompaction_jobs\tflush_imm_returns\ttotal_background_returns\n" >> "$SUMMARY"

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
        printf "%d\t0\t0\t0\n" "$num" >> "$SUMMARY"
        continue
    fi

    # New trace CSV columns:
    # trace_ts_us,event_index,event,db_name,thread_id,file_number,file_name,manifest_file_number,status,notes
    # Count compaction jobs as background_compaction_return where:
    #   - notes != "flush_imm"
    #   - status does not start with "IO error"
    counts=$(awk -F',' '
        NR > 1 {
            event = $3
            status = $9
            notes = $10
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", event)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", status)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", notes)
            gsub(/^"|"$/, "", status)
            gsub(/^"|"$/, "", notes)
            if (event == "background_compaction_return") {
                total++
                if (notes == "flush_imm") {
                    flush_imm++
                } else if (status ~ /^IO error/) {
                    io_error++
                } else {
                    jobs++
                }
            }
        }
        END {
            printf "%d %d %d\n", jobs + 0, flush_imm + 0, total + 0
        }
    ' "$trace")
    read -r compaction_jobs flush_imm_returns total_background_returns <<< "$counts"

    printf "num=%-5d  compaction_jobs=%-3d  (flush_imm_returns=%-2d  total_background_returns=%-3d)\n" \
        "$num" "$compaction_jobs" "$flush_imm_returns" "$total_background_returns"

    printf "%d\t%d\t%d\t%d\n" "$num" "$compaction_jobs" "$flush_imm_returns" "$total_background_returns" >> "$SUMMARY"
done

echo ""
echo "Summary written to: $SUMMARY"
echo "Per-run traces in:  $OUT_DIR/"
