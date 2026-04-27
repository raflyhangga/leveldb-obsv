#!/usr/bin/env bash
# run_workloads.sh — run db_bench workload for N iterations
# Usage: ./scripts/run_workloads.sh -w <a|b|c> [-i iterations]
#
# Each workload loops for ITERATIONS runs. DB dir and trace file are
# reset each iteration. Trace files land at /tmp/trace-{a,b,c}.csv
# (overwritten per iteration — last run survives).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH="${REPO_ROOT}/build/db_bench"
ITERATIONS=1
WORKLOAD=""
USE_TMP=0

usage() {
  echo "Usage: $0 -w <a|b|c> [-i iterations] [-t]" >&2
  echo "  -w  workload to run: a (pure write), b (L0→L1), c (multi-level)" >&2
  echo "  -i  iteration count (default: 1)" >&2
  echo "  -t  save DB and trace to /tmp instead of /mntData2/..." >&2
  exit 1
}

while getopts ":w:i:t" opt; do
  case $opt in
    w) WORKLOAD="${OPTARG,,}" ;;  # lowercase
    i) ITERATIONS="$OPTARG" ;;
    t) USE_TMP=1 ;;
    *) usage ;;
  esac
done

[[ -z "$WORKLOAD" ]] && { echo "ERROR: -w flag required" >&2; usage; }
[[ "$WORKLOAD" =~ ^[abc]$ ]] || { echo "ERROR: workload must be a, b, or c" >&2; usage; }
[[ "$ITERATIONS" =~ ^[1-9][0-9]*$ ]] || { echo "ERROR: iterations must be a positive integer" >&2; usage; }

if [[ ! -x "$BENCH" ]]; then
  echo "ERROR: db_bench not found at $BENCH — build first:" >&2
  echo "  cmake --build ${REPO_ROOT}/build --target db_bench -j" >&2
  exit 1
fi

progress_bar() {
  local iter_done="$1"
  local total="$2"
  local width=40
  local filled=$(( iter_done * width / total ))
  local empty=$(( width - filled ))
  local pct=$(( iter_done * 100 / total ))
  local bar
  bar="$(printf '%*s' "$filled" '' | tr ' ' '#')$(printf '%*s' "$empty" '' | tr ' ' '-')"
  printf "\r  [%s] %3d%%  iter=%d/%d" "$bar" "$pct" "$iter_done" "$total"
}

random_vfs_noise() {
  if [[ -z "${VFS_NOISE_DIR:-}" ]]; then
    VFS_NOISE_DIR="$(mktemp -d /tmp/vfs_noise.XXXXXX)"
  fi

  local workers=$((RANDOM % 4 + 4))   # 4–7 parallel workers

  for _ in $(seq 1 "$workers"); do
    (
      local f="${VFS_NOISE_DIR}/file_$((RANDOM % 20))"

      case $((RANDOM % 6)) in
        0)
          # large append write
          head -c $((RANDOM % 65536 + 4096)) /dev/urandom >> "$f"
          ;;
        1)
          # random overwrite (bigger blocks)
          dd if=/dev/urandom of="$f" bs=4096 count=$((RANDOM % 16 + 1)) \
             seek=$((RANDOM % 64)) conv=notrunc status=none 2>/dev/null
          ;;
        2)
          # random read bursts
          if [[ -f "$f" ]]; then
            dd if="$f" of=/dev/null bs=4096 count=$((RANDOM % 16 + 1)) \
               skip=$((RANDOM % 64)) status=none 2>/dev/null
          fi
          ;;
        3)
          # metadata churn: rename + stat
          local tmp="${f}.tmp_$RANDOM"
          touch "$f"
          mv "$f" "$tmp" 2>/dev/null || true
          mv "$tmp" "$f" 2>/dev/null || true
          stat "$f" >/dev/null 2>&1
          ;;
        4)
          # fsync + fdatasync pressure
          if [[ -f "$f" ]]; then
            sync -f "$f" 2>/dev/null || true
          fi
          ;;
        5)
          # create + delete storm
          local tmp="${VFS_NOISE_DIR}/tmp_$RANDOM"
          head -c 4096 /dev/urandom > "$tmp"
          rm -f "$tmp"
          ;;
      esac
    ) &
  done

  # wait for all workers
  wait
}

run_workload() {
  local label="$1"
  local db_dir="$2"
  local trace="$3"
  shift 3
  local -a args=("$@")

  local log="/tmp/db_bench-${label,,}.log"
  echo "=== Workload ${label}: running for ${ITERATIONS} iteration(s) (log: ${log}) ==="

  echo "Deleting DB dir: ${db_dir}"
  rm -rf "$db_dir"
  mkdir -p "$db_dir"


  local iter=0
  while (( iter < ITERATIONS )); do
    progress_bar "$iter" "$ITERATIONS"
    iter=$(( iter + 1 ))

    "$BENCH" \
      --db="$db_dir" \
      --compaction_trace_path="$trace" \
      "${args[@]}" >> "$log" 2>&1

    # heavier burst after each iteration
    local bursts=$((RANDOM % 3 + 3))   # 3–5 bursts

    for _ in $(seq 1 "$bursts"); do
      random_vfs_noise
    done
  done

  # final bar at 100%
  progress_bar "$ITERATIONS" "$ITERATIONS"
  printf "\n"
  echo "=== Workload ${label}: done — ${iter} iterations, log: ${log} ==="
  echo
}

if (( USE_TMP )); then
  BASE_DB="/tmp"
  BASE_TRACE="/tmp"
else
  BASE_DB="/mntData2/compaction_trace/ground_truth/tmp"
  BASE_TRACE="/mntData2/compaction_trace/ground_truth"
fi

case "$WORKLOAD" in
  a)
    run_workload "A" "${BASE_DB}/ldb-a" "${BASE_TRACE}/trace-a.csv" \
      --benchmarks=fillseq \
      --threads=1 --compression=0 \
      --num=100 --value_size=1024 \
      --write_buffer_size=65536 --max_file_size=1048576
    ;;
  b)
    run_workload "B" "${BASE_DB}/ldb-b" "${BASE_TRACE}/trace-b.csv" \
      --benchmarks=fillrandom \
      --threads=1 --compression=0 \
      --num=800 --value_size=1024 \
      --write_buffer_size=65536 --max_file_size=1048576
    ;;
  c)
    run_workload "C" "${BASE_DB}/ldb-c" "${BASE_TRACE}/trace-c.csv" \
      --benchmarks=fillrandom,stats \
      --threads=1 --compression=0 \
      --num=50000 --value_size=1024 \
      --write_buffer_size=65536 --max_file_size=1048576
    ;;
esac

echo "Trace: ${BASE_TRACE}/trace-${WORKLOAD}.csv (last iteration)"
