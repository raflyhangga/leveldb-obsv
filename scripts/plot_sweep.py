#!/usr/bin/env python3
"""
plot_sweep.py — visualize compaction job counts from sweep_num.sh output

Usage:
    python3 scripts/plot_sweep.py [summary_tsv] [output_png]

Defaults:
    summary_tsv  /tmp/sweep-out/summary.tsv
    output_png   /tmp/sweep-out/sweep_chart.png

Requires: matplotlib, numpy  (pip install matplotlib numpy)
"""

import sys
import csv
import pathlib
import numpy as np

summary_path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/sweep-out/summary.tsv")
out_path     = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "/tmp/sweep-out/sweep_chart.png")

# ── Load data ──────────────────────────────────────────────────────────────────
rows = []
with summary_path.open() as f:
    reader = csv.DictReader(f, delimiter="\t")
    for r in reader:
        rows.append(
            {
                "num": int(r["num"]),
                "compaction_jobs": int(r["compaction_jobs"]),
                "flush_imm_returns": int(r["flush_imm_returns"]),
                "total_background_returns": int(r["total_background_returns"]),
            }
        )

rows.sort(key=lambda r: r["num"])
nums = [r["num"] for r in rows]
compaction_jobs = [r["compaction_jobs"] for r in rows]

# ── Scale decisions ────────────────────────────────────────────────────────────
max_jobs = max(compaction_jobs) if compaction_jobs else 1

# Log x when nums span more than 20×
use_logx = len(nums) > 1 and nums[-1] / nums[0] > 20

# ── Bar widths (stacked, centered on nums) ─────────────────────────────────────
if use_logx and len(nums) > 1:
    log_nums = np.log10(nums)
    min_gap  = float(np.min(np.diff(log_nums)))
    tvw      = min_gap * 0.90
    bar_frac = 2 * (10**tvw - 1) / (1 + 10**tvw)
else:
    span     = (nums[-1] - nums[0]) if len(nums) > 1 else max(nums[0], 1)
    bar_frac = max(span / len(nums) * 0.90, 1) / max(nums)

w_halfs = [n * bar_frac for n in nums]

# ── Plot ───────────────────────────────────────────────────────────────────────
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

fig, ax = plt.subplots(figsize=(8, 4))

if use_logx:
    ax.set_xscale("log")

# Compaction jobs: background_compaction_return excluding notes="flush_imm"
ax.bar(
    nums,
    compaction_jobs,
    width=w_halfs,
    label='compaction jobs (event="background_compaction_return" && notes!="flush_imm")',
    color="#3b82f6",
)

# ── Y-axis scale & headroom ────────────────────────────────────────────────────
y_top = max_jobs * 1.30
ax.set_ylim(bottom=0, top=y_top)

# Tight x margins
ax.set_xlim(
    nums[0]  / (10 ** 0.18) if use_logx else nums[0]  - w_halfs[0],
    nums[-1] * (10 ** 0.18) if use_logx else nums[-1] + w_halfs[-1],
)

# ── Bar annotations ────────────────────────────────────────────────────────────
for r, x_pos, jobs in zip(rows, nums, compaction_jobs):
    if jobs > 0:
        ax.text(
            x_pos,
            jobs + max_jobs * 0.015,
            str(jobs),
            ha="center",
            va="bottom",
            fontsize=6.5,
            color="#111827",
            fontweight="bold",
        )

    # Secondary context labels for excluded and raw background-return counts.
    flush_imm = r["flush_imm_returns"]
    total_bg = r["total_background_returns"]
    if total_bg > 0:
        ax.text(
            x_pos,
            max(jobs * 0.5, 0.25),
            f"raw={total_bg}\nflush={flush_imm}",
            ha="center",
            va="center",
            fontsize=5.5,
            color="#1f2937",
        )

# ── Threshold lines ────────────────────────────────────────────────────────────
for x, color, label in [
    (100,   "#22c55e", "A"),
    (450,   "#ef4444", "B"),
    (50000, "#a855f7", "C"),
]:
    ax.axvline(x, color=color, linestyle="--", linewidth=1.1,
               label=f"claimed {label} threshold (num={x:,})")

# ── Axes formatting ────────────────────────────────────────────────────────────
if use_logx:
    ax.set_xticks(nums)
    ax.xaxis.set_major_formatter(
        ticker.FuncFormatter(lambda v, _: f"{int(v):,}")
    )
ax.tick_params(axis="x", labelsize=7.5, rotation=40)
ax.tick_params(axis="y", labelsize=8)
ax.yaxis.set_major_locator(ticker.MaxNLocator(integer=True))

ax.set_xlabel("--num  (KV pairs written)", fontsize=9)
ax.set_ylabel("compaction jobs", fontsize=9)
ax.set_title(
    "Compaction jobs vs KV Pairs Written\n"
    '(event="background_compaction_return" excluding notes="flush_imm")',
    fontsize=8.5,
)
ax.legend(fontsize=7, loc="upper left", framealpha=0.85)
ax.grid(axis="y", alpha=0.25)

fig.tight_layout(pad=0.8)
fig.savefig(out_path, dpi=150)
print(f"Chart saved to: {out_path}")
