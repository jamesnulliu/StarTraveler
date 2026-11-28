"""Build presentation figures from frozen results; never launches a renderer."""
from pathlib import Path
import csv
import hashlib
import json

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "renders/production-lag-fixed-exposure"
OUT = ROOT / "docs/final-materials"
OUT.mkdir(parents=True, exist_ok=True)
rows = list(csv.DictReader((DATA / "summary.csv").open(newline="", encoding="utf-8")))
shots = ["cockpit-anvil-turn", "mirror-glass-pass", "fast-star-turn"]
names = ["Anvil cockpit", "Mirror / glass", "Star turn"]
methods = ["asvgf-dlss-full", "rr-quality"]
labels = ["Full + DLSS K", "Hybrid + DLSS-RR"]
colors = ["#1869A6", "#C65A20"]
lookup = {(r["shot"], r["config"], r["reference"]): r for r in rows}
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 12,
                     "axes.spines.top": False, "axes.spines.right": False,
                     "svg.fonttype": "none"})
for metric, title, ylabel, stem, ymax in [
    ("settle_median", "Entry into each method's own terminal-error band",
     "Still-frame index (lower is earlier)", "settling-entry", 330),
    ("terminal_flip_median", "Observed terminal error after motion",
     "Mean LDR-FLIP (lower is better)", "terminal-error", .051),
]:
    fig, axes = plt.subplots(1, 2, figsize=(14, 5.6), sharey=True)
    for ax, ref in zip(axes, ["primary", "secondary"]):
        for m, (method, label, color) in enumerate(zip(methods, labels, colors)):
            values = np.array([float(lookup[s, method, ref][metric]) for s in shots])
            pos = np.arange(3) + (m - .5) * .32
            ax.bar(pos, values, .30, label=label, color=color)
            if metric == "settle_median":
                low = [float(lookup[s, method, ref]["settle_min"]) for s in shots]
                high = [float(lookup[s, method, ref]["settle_max"]) for s in shots]
                ax.errorbar(pos, values, yerr=[values-low, np.array(high)-values],
                            fmt="none", ecolor="#182A3A", capsize=4)
            for index, (x, v) in enumerate(zip(pos, values)):
                label_y = high[index] if metric == "settle_median" else v
                ax.text(x, label_y + ymax * .035, str(int(v)) if metric == "settle_median"
                        else f"{v:.5f}", ha="center", va="bottom", fontsize=10)
        ax.set_xticks(np.arange(3), names)
        ax.set_title(f"{ref.capitalize()} reference (8192 spp)")
        ax.set_ylim(0, ymax)
        ax.set_axisbelow(True)
        ax.grid(axis="y", alpha=.18)
    axes[0].set_ylabel(ylabel)
    handles, legend_labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, legend_labels, loc="lower center", ncol=2, bbox_to_anchor=(.5, .08))
    fig.suptitle(title, fontsize=19)
    detail = ("Medians of 3 seeds; whiskers = observed min-max, not confidence intervals. "
              "Each method has a different band." if metric == "settle_median" else
              "Medians of 3 per-run final-64-frame medians. References remain separate; "
              "no significance claim.")
    fig.text(.5, .025, detail, ha="center", fontsize=10)
    fig.subplots_adjust(bottom=.25, top=.83, wspace=.10, left=.08, right=.98)
    fig.canvas.draw()
    for ext in ["png", "svg"]:
        fig.savefig(OUT / f"{stem}.{ext}", dpi=180)
    plt.close(fig)

# Keep all twelve rows and original precision beside the figures.
(OUT / "chart-data.csv").write_bytes((DATA / "summary.csv").read_bytes())
(OUT / "figure-provenance.json").write_text(json.dumps({
    "source": "renders/production-lag-fixed-exposure/summary.csv",
    "source_sha256": hashlib.sha256((DATA / "summary.csv").read_bytes()).hexdigest(),
    "rows": len(rows), "reference_pooling": False, "thresholds_changed": False,
    "entry_error_bars": "three-seed observed min/max, not CI",
    "terminal_error_bars": "not shown; scalar group medians only",
    "capture_data_modified": False,
}, indent=2) + "\n", encoding="utf-8")
print(f"Built four figures and exact chart-data copy in {OUT}")
