#!/usr/bin/env bash
# run_workloads.sh — run a db_bench workload continuously for 2 minutes
# Usage: ./scripts/run_workloads.sh -w <a|b|c> [-d duration_seconds]
#
# Each workload loops for DURATION seconds. DB dir and trace file are
# reset each iteration. Trace files land at /tmp/trace-{a,b,c}.csv
# (overwritten per iteration — last run survives).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH="${REPO_ROOT}/build/db_bench"
DURATION=120
WORKLOAD=""

usage() {
  echo "Usage: $0 -w <a|b|c> [-d duration_seconds]" >&2
  echo "  -w  workload to run: a (pure write), b (L0→L1), c (multi-level)" >&2
  echo "  -d  duration in seconds (default: 120)" >&2
  exit 1
}

while getopts ":w:d:" opt; do
  case $opt in
    w) WORKLOAD="${OPTARG,,}" ;;  # lowercase
    d) DURATION="$OPTARG" ;;
    *) usage ;;
  esac
done

[[ -z "$WORKLOAD" ]] && { echo "ERROR: -w flag required" >&2; usage; }
[[ "$WORKLOAD" =~ ^[abc]$ ]] || { echo "ERROR: workload must be a, b, or c" >&2; usage; }

if [[ ! -x "$BENCH" ]]; then
  echo "ERROR: db_bench not found at $BENCH — build first:" >&2
  echo "  cmake --build ${REPO_ROOT}/build --target db_bench -j" >&2
  exit 1
fi

progress_bar() {
  local elapsed="$1"
  local total="$2"
  local iter="$3"
  local width=40
  local filled=$(( elapsed * width / total ))
  local empty=$(( width - filled ))
  local pct=$(( elapsed * 100 / total ))
  local bar
  bar="$(printf '%*s' "$filled" '' | tr ' ' '#')$(printf '%*s' "$empty" '' | tr ' ' '-')"
  printf "\r  [%s] %3d%%  %ds/%ds  iter=%d" "$bar" "$pct" "$elapsed" "$total" "$iter"
}

run_workload() {
  local label="$1"
  local db_dir="$2"
  local trace="$3"
  shift 3
  local -a args=("$@")

  local log="/tmp/db_bench-${label,,}.log"
  echo "=== Workload ${label}: running for ${DURATION}s (log: ${log}) ==="

  local start=$SECONDS
  local iter=0

  echo "Deleting DB dir: ${db_dir}"
  rm -rf "$db_dir"
  mkdir -p "$db_dir"


  while (( SECONDS - start < DURATION )); do
    iter=$(( iter + 1 ))
    local elapsed=$(( SECONDS - start ))
    progress_bar "$elapsed" "$DURATION" "$iter"

    "$BENCH" \
      --db="$db_dir" \
      --compaction_trace_path="$trace" \
      "${args[@]}" >> "$log" 2>&1
  done

  # final bar at 100%
  progress_bar "$DURATION" "$DURATION" "$iter"
  printf "\n"
  echo "=== Workload ${label}: done — ${iter} iterations, log: ${log} ==="
  echo
}

case "$WORKLOAD" in
  a)
    run_workload "A" "/mntData2/compaction_trace/ground_truth/tmp/ldb-a" "/mntData2/compaction_trace/ground_truth/trace-a.csv" \
      --benchmarks=fillseq \
      --threads=1 --compression=0 \
      --num=100 --value_size=1024 \
      --write_buffer_size=65536 --max_file_size=1048576
    ;;
  b)
    run_workload "B" "/mntData2/compaction_trace/ground_truth/tmp/ldb-b" "/mntData2/compaction_trace/ground_truth/trace-b.csv" \
      --benchmarks=fillrandom,compact \
      --threads=1 --compression=0 \
      --num=450 --value_size=1024 \
      --write_buffer_size=65536 --max_file_size=1048576
    ;;
  c)
    run_workload "C" "/mntData2/compaction_trace/ground_truth/tmp/ldb-c" "/mntData2/compaction_trace/ground_truth/trace-c.csv" \
      --benchmarks=fillrandom,stats \
      --threads=1 --compression=0 \
      --num=50000 --value_size=1024 \
      --write_buffer_size=65536 --max_file_size=1048576
    ;;
esac

echo "Trace: /mntData2/compaction_trace/ground_truth/trace-${WORKLOAD}.csv (last iteration)"
