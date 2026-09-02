#!/usr/bin/env bash
# Drives sqpoll_core_budget through the investigation matrix for the
# SQPOLL core-budget question and writes one CSV row per run.
#
# Usage: run_matrix.sh <path-to-sqpoll_core_budget-binary> [output.csv]
set -euo pipefail

BIN="${1:?usage: run_matrix.sh <binary> [output.csv]}"
OUT="${2:-sqpoll_core_budget_results.csv}"

echo "mode,threads,idle_ms,total_iops,avg_p50_us,avg_p99_us" > "$OUT"

echo "== baseline: independent rings, no pinning, sq_thread_idle=1000 (control) =="
for n in 1 2 4 8; do
  "$BIN" baseline "$n" | tee -a "$OUT"
done

echo "== attach: threads 1..N-1 share thread 0's kernel poller =="
for n in 1 2 4 8; do
  "$BIN" attach "$n" | tee -a "$OUT"
done

echo "== idle: 4 threads (where baseline starts collapsing), sweeping sq_thread_idle =="
for idle in 1 10 100 1000 10000; do
  "$BIN" idle 4 "$idle" | tee -a "$OUT"
done
echo "== idle: same sweep at 1 thread, as a control =="
for idle in 1 10 100 1000 10000; do
  "$BIN" idle 1 "$idle" | tee -a "$OUT"
done

echo "== pinned: app thread + poller each get a dedicated core =="
for n in 1 2; do
  "$BIN" pinned "$n" | tee -a "$OUT"
done

echo "done -- results in $OUT"
