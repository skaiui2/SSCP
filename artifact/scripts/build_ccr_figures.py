"""Build the CCR paper's four figures from artifact/data/ only.

Run from any directory:
    python3 artifact/scripts/build_ccr_figures.py
Outputs PDF figures into artifact/figures/.
"""

from pathlib import Path
import csv
import json
import math

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"
OUT = ROOT / "figures"
OUT.mkdir(parents=True, exist_ok=True)

plt.rcParams.update({
    "font.family": "serif", "font.size": 8,
    "axes.labelsize": 8, "axes.titlesize": 8,
    "legend.fontsize": 6.7, "xtick.labelsize": 7,
    "ytick.labelsize": 7, "lines.linewidth": 1.05,
    "pdf.fonttype": 42, "savefig.bbox": "tight",
})


def rows(name):
    with (DATA / name).open(encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def save(fig, name):
    fig.savefig(OUT / name, pad_inches=0.025)
    plt.close(fig)


# Figure 1: Linux 6.8 SS cohort span + SSCP unbounded gain sweep
gains = [1, 1.2, 1.39, 1.65, 2, 4, 8]
linux = {
    "Baseline": [96.82, 98.31, 98.72, 98.65, 80.64, 43.93, 28.29],
    "Immediate ACK": [99.80, 98.00, 98.69, 99.81, 81.63, 44.56, 22.43],
    "TSO/GSO on": [96.45, 94.65, 97.93, 97.64, 85.09, 46.12, 23.62],
    "Strict FQ": [97.30, 97.66, 99.76, 98.12, 81.45, 49.73, 21.65],
}
fig, ax = plt.subplots(1, 2, figsize=(6.6, 1.62), layout="constrained")
for (name, y), marker, style in zip(linux.items(), ["o", "s", "^", "x"], ["-", "--", "-.", ":"]):
    ax[0].plot(gains, y, marker=marker, markersize=3, linestyle=style, color="black", label=name)
ax[0].set_xscale("log", base=2)
ax[0].set_xticks([1, 1.39, 2, 4, 8], ["1", "1.39", "2", "4", "8"])
ax[0].set_ylim(15, 112)
ax[0].axhline(100, color="0.65", ls=":", lw=.8)
ax[0].axvline(2 * math.log(2), color="0.65", ls=":", lw=.8)
ax[0].set(xlabel="SS gain", ylabel="Cohort span (ms)", title="(a) Linux 6.8 SS: 100 ms base RTT")
ax[0].legend(loc="upper right", frameon=False, ncol=1, handlelength=1.5, borderaxespad=.2)
sweep = [r for r in rows("ss_gain_sweep.csv") if r["queue"] == "0" and r["policy"].startswith("Static")]
x = [float(r["g"]) for r in sweep]
ax[1].plot(x, [float(r["fct_ms"]) for r in sweep], "ko-", markersize=3, label="FCT")
right = ax[1].twinx()
right.plot(x, [float(r["burst_pct"]) for r in sweep], "k^--", markersize=3, label="Gaps <50 us")
ax[1].axvline(2 * math.log(2), color="0.5", ls=":", lw=.8)
ax[1].set(xlabel="SS gain", ylabel="FCT (ms)", title="(b) SSCP sweep: unbounded queue")
right.set_ylabel("Gaps <50 us (%)")
ax[1].legend(loc="upper center", frameon=False)
right.legend(loc="center right", frameon=False)
save(fig, "ccr_static.pdf")

# Figure 2: Limit-cycle collapse + real SSCP CUBIC trace
fig, ax = plt.subplots(1, 2, figsize=(6.6, 1.65), layout="constrained")
for mode, style, marker in [("Old_TailSlot", "--", "o"), ("CuttingIdentity", "-", "s")]:
    rr = [r for r in rows("limit_cycle.csv") if r["mode"] == mode]
    if not rr:
        continue
    ax[0].plot(
        [int(r["round"]) for r in rr],
        [abs(float(r["int1_ms"]) - float(r["int2_ms"])) for r in rr],
        color="black", ls=style, marker=marker, markersize=3,
        label=mode.replace("Old_TailSlot", "Naive tail slot").replace("CuttingIdentity", "Cutting identity"),
    )
ax[0].set(xlabel="Round", ylabel="Gap difference (ms)", title="(a) Fixed-window model: N=2")
ax[0].legend(frameon=False)

rr = rows("cubic_epoch_trace.csv")
ts = [float(r["t_sec"]) for r in rr]
ws = [float(r["cwnd_mss"]) for r in rr]
gs = [float(r["gain"]) for r in rr]
g_static = [2.0 * math.log(2.0) if w < 64.0 else 1.0 for w in ws]
l1, = ax[1].step(ts, gs, where="post", color="black", ls="-", lw=1.2, label="BudgetO1")
l2, = ax[1].step(ts, g_static, where="post", color="0.55", ls=":", lw=1.0, label="Static (2 ln 2, 1) ref.")
ax[1].set_ylim(0.95, 1.50)
ax[1].set_xlim(0.0, 8.6)
right = ax[1].twinx()
l3, = right.plot(ts, ws, color="black", ls="--", lw=0.95, label="cwnd")
right.set_ylim(0, 320)
ax[1].annotate("ssthresh fold\n(g=1.09)", xy=(0.82, 1.095), xytext=(2.55, 1.24),
               ha="center", fontsize=6.0, arrowprops={"arrowstyle": "->", "lw": 0.6, "color": "0.25"})
ax[1].annotate("Convex probe\n(g≈1.01)", xy=(7.0, 1.014), xytext=(7.25, 1.095),
               ha="center", fontsize=6.0, arrowprops={"arrowstyle": "->", "lw": 0.6, "color": "0.25"})
ax[1].set(xlabel="Time (s)", ylabel="Applied gain g", title="(b) SSCP CUBIC: SS to CA (ssthresh=64)")
right.set_ylabel("cwnd (MSS)")
ax[1].legend(handles=[l1, l2, l3], loc="upper center", bbox_to_anchor=(0.63, 1.01), frameon=False, ncol=1, handlelength=1.5)
save(fig, "ccr_dynamics.pdf")

# Figure 3: Cross-regime SSCP CUBIC + picoquic 20-seed CUBIC
policies = ["BudgetO1", "Analytic_2ln2_1", "Linux_2_12", "Static_10"]
labels = ["BudgetO1", "2 ln 2 / 1", "2 / 1.2", "1 / 1"]
styles = ["", "xxx", "///", "..."]
fig, ax = plt.subplots(1, 2, figsize=(6.6, 1.66), layout="constrained")
scenarios = [("Jit5_NoReord_q50", "Jitter"), ("Jit5_Reord_q50", "+ reordering"), ("BW_100M_q20_vsh", "Very shallow")]
source = rows("virtual_single_summary.csv")
width = .19
for k, (pol, label, hatch) in enumerate(zip(policies, labels, styles)):
    rr = [next(r for r in source if r["scen"] == scen and r["cc"] == "CUBIC" and r["pol"] == pol) for scen, _ in scenarios]
    ax[0].bar(
        np.arange(3) + (k - 1.5) * width,
        [float(r["fct_ms_mean"]) / 1000 for r in rr],
        width=width, color="0.25" if k == 0 else "white", edgecolor="black",
        hatch=hatch, linewidth=.6, label=label,
        yerr=[float(r["fct_ms_ci95"]) / 1000 for r in rr],
        error_kw={"elinewidth": .6, "capsize": 1},
    )
ax[0].set_ylim(0, 10.6)
ax[0].text(2 + 0.5 * width, 4.90, "80 drops", ha="center", va="bottom", fontsize=6.0)
ax[0].annotate("5 drops", xy=(2 - 0.5 * width, 1.08), xytext=(1.60, 2.55),
               ha="center", fontsize=5.8, arrowprops={"arrowstyle": "->", "lw": 0.55, "color": "0.25"})
ax[0].set_xticks(range(3), [label for _, label in scenarios])
ax[0].set(ylabel="Mean FCT (s)", title="(a) SSCP CUBIC: dynamic & shallow regimes")
ax[0].legend(frameon=False, ncol=2, loc="upper left", columnspacing=.6, handlelength=1)

pr = rows("picoquic_multiseed_runs.csv")
modes = ["BudgetO1", "Analytic_2ln2_1", "Native_Q2", "Linux_20_12", "Native_Def"]
m_styles = ["", "xxx", "///", "...", "+++"]
for k, (mode, hatch) in enumerate(zip(modes, m_styles)):
    vals = [float(r["cv500"]) for r in pr if r["scenario"] == "Multi_CUBIC_q12ms_jit" and r["mode"] == mode]
    q1, med, q3 = np.percentile(vals, [25, 50, 75])
    ax[1].bar(k, med, color="0.25" if k == 0 else "white", edgecolor="black", hatch=hatch, linewidth=.6)
    ax[1].errorbar(k, med, yerr=[[med - q1], [q3 - med]], color="black", fmt="none", capsize=2, lw=.6)
ax[1].set_ylim(0, 4.15)
ax[1].set_xticks(range(5), ["BudgetO1", "2 ln 2 / 1", "Native\nQ2", "2 / 1.2", "Native\nDef"])
ax[1].set(ylabel="Tail-gap CV (median / IQR)", title="(b) picoquic CUBIC: 20 seeds")
save(fig, "ccr_results.pdf")

# Figure 4: Multi-flow CUBIC Jain fairness + Linux 6.8 SSCP/UDP dual-tap
fig, ax = plt.subplots(1, 2, figsize=(3.3, 1.40), layout="constrained")
source = rows("virtual_multiflow_summary.csv")
topologies = ["N2_100M_jit5", "N4_50M_stag", "N8_100M_q80_ds"]
for k, (mix, label, hatch) in enumerate([
    ("Homo_CUBIC_Bud", "BudgetO1", ""),
    ("Homo_CUBIC_Lin", "2 / 1.2", "///"),
]):
    rr = [next(r for r in source if r["topo"] == topo and r["mix"] == mix) for topo in topologies]
    ax[0].bar(
        np.arange(3) + (k - .5) * .34,
        [float(r["jain_mean"]) for r in rr],
        width=.34, yerr=[float(r["jain_ci95"]) for r in rr],
        color="0.25" if k == 0 else "white", edgecolor="black", hatch=hatch,
        linewidth=.6, label=label, error_kw={"elinewidth": .6, "capsize": 1.2},
    )
ax[0].set_xticks(range(3), ["2/jit", "4/stag", "8/db"])
ax[0].set(ylabel="Jain index", ylim=(0.0, 1.24), title="(a) Multi-flow CUBIC")
ax[0].legend(frameon=False, ncol=2, loc="upper center", columnspacing=0.6, handlelength=1, fontsize=5.9)

with (DATA / "linux_dual_tap_summary.json").open(encoding="utf-8") as f:
    dt = json.load(f)
dt_keys = [
    ("BudgetO1_Reno_spin1500", "Budget\nO1", ""),
    ("Analytic_2ln2_1_Reno", "2 ln 2\n/ 1", "xxx"),
    ("Linux_2_12_Reno", "2 /\n1.2", "///"),
    ("Static_10_Reno", "1 /\n1", "..."),
]
for k, (key, lbl, hatch) in enumerate(dt_keys):
    m = dt[key]["metrics"]
    cv = m["vc_cv500"]["mean"]
    b50 = m["vc_b50_pct"]["mean"]
    ax[1].bar(k, cv, width=0.58, color="0.25" if k == 0 else "white", edgecolor="black", hatch=hatch, linewidth=.6)
    ax[1].text(k, cv + 0.04, f"{b50:.1f}%", ha="center", va="bottom", fontsize=5.6)
ax[1].set_xticks(range(4), [lbl for _, lbl, _ in dt_keys], fontsize=5.8)
ax[1].set(ylabel="Wire tail CV", ylim=(0, 1.32), title="(b) Linux SSCP/UDP")
save(fig, "ccr_coexistence.pdf")
print(f"Created four CCR figures in {OUT}")
