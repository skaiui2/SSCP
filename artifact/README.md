# BudgetO1 Artifact & Reproduction Guide

## 1. Purpose and Scope

This artifact reproduces the figures, tables, and cross-stack implementations for the paper:

> **Beyond Magic Pacing Ratios: Analytical Boundaries and Workload-Based Scheduling** (ACM SIGCOMM CCR Short Technical Paper)

Traditional window-based transports hardwire static per-phase pacing multipliers (e.g., Linux TCP's `200% / 120%` or QUIC's illustrative `125%`). This research derives exact finite-window ACK-clocked dispersion boundaries ($2(H_{2W_0}-H_{W_0})\to 2\ln 2$ in Slow Start; $1 + \frac{W_0-1}{W_0(W_0+1)}\to 1.0$ in integer Reno CA) and implements **BudgetO1**, an $O(1)$ per-packet Fold-on-Send workload pacer with clock-separated jitter headroom, across three transport stacks plus a live Linux kernel network-namespace evaluation:

1. **SSCP** (a compact, self-contained C TCP-like reliable transport over UDP; baseline commit [`cca3fa6`](https://github.com/skaiui2/SSCP/commit/cca3fa6) on `main`),
2. **ns-3.46.1 TCP** (`patches/ns3-3.46.1-budget-o1.patch`, native `m_pacingTimer`),
3. **picoquic** (`patches/picoquic-9491698-budget-o1.patch`, base commit `9491698`, native pacing bucket),
4. **Live Linux 6.8 SSCP/UDP** network-namespace dual-tap captures (`scripts/linux_udp/`).

### Relationship to the `main` Branch

This branch (`research/budget-pacing-20261113`) tracks the exact SSCP core (`sscp.h`, `sscp.c`) from `main` (`cca3fa6`), adding only an 18-line static-gain comparison switch (`budget_pacing`, `static_ss_gain_q16`, `static_ca_gain_q16`, `pacing_rttvar_q16`) and the read-only experiment harnesses (`test/virtual/case_multiflow.c` and `artifact/`).

---

## 2. Quick Figure Regeneration (2 Seconds, No Compilation)

All summary datasets required to regenerate the four paper figures are included in `artifact/data/`:

```bash
python3 artifact/scripts/build_ccr_figures.py
```

**Expected output:** Generates `ccr_static.pdf` (Fig. 1), `ccr_dynamics.pdf` (Fig. 2), `ccr_results.pdf` (Fig. 3), and `ccr_coexistence.pdf` (Fig. 4) inside `artifact/figures/`.

---

## 3. Paper-to-Artifact Mapping

| Paper Item | Stack / Environment | Data File in `artifact/data/` | Reproduction Script / Patch |
| :--- | :--- | :--- | :--- |
| **Fig. 1b** (SS gain sweep, $q=\infty$) | SSCP virtual | `ss_gain_sweep.csv` | `scripts/run_sscp_virtual.py` |
| **Fig. 2a & 2b** (Limit cycle & CUBIC trace) | Analytical / SSCP virtual | `limit_cycle.csv`, `cubic_epoch_trace.csv` | `scripts/run_sscp_virtual.py` + `scripts/build_ccr_figures.py` |
| **Q1** (Fold-on-Send vs. Oracle $\mathcal{B}(L)$) | SSCP virtual | — | `scripts/run_sscp_virtual.py --quick` |
| **Table 1** (vs. static $(2\ln 2, 1)$) | SSCP virtual | — | `scripts/run_sscp_virtual.py --quick` |
| **Table 2 & Fig. 3a** (30-seed CUBIC & $q=20$) | SSCP virtual | `virtual_single_summary.csv` | `scripts/run_sscp_virtual.py --full` |
| **Table 3 & Q1/Q5** (TCP loss runs) | ns-3.46.1 TCP | `ns3_summary.csv` | `patches/ns3-3.46.1-budget-o1.patch` + `scripts/ns3/run_experiments.py` |
| **Fig. 3b & Q5** (20-seed QUIC CUBIC) | picoquic (`9491698`) | `picoquic_multiseed_runs.csv` | `patches/picoquic-9491698-budget-o1.patch` + `scripts/picoquic/` |
| **Fig. 4a & Q6** (Multi-flow CUBIC) | SSCP virtual | `virtual_multiflow_summary.csv` | `scripts/run_sscp_virtual.py --full` |
| **Fig. 4b & Q6** (Live wire bursts & CV) | Linux 6.8 netns UDP | `linux_dual_tap_summary.json` | `scripts/linux_udp/udp_bench.c` + `exp_linux_option_a.py` |

---

## 4. Full End-to-End Reproduction Commands

### A. SSCP Virtual Network Suite (Q1, Fig. 1b, Table 1, Table 2, Fig. 3a, Fig. 4a)

```bash
cmake -S test -B test/build -DCMAKE_BUILD_TYPE=Release
cmake --build test/build -j
ctest --test-dir test/build -L virtual --output-on-failure

# Fast verification (~5 s): Q1 Oracle, Fig. 1b, Table 1
python3 artifact/scripts/run_sscp_virtual.py --build-dir test/build --quick

# Full multi-seed suite (~2 min): adds Fig. 3a & Fig. 4a
python3 artifact/scripts/run_sscp_virtual.py --build-dir test/build --full
```

**Pass criteria:** All `10/10` virtual CTests pass; Q1 median relative error against the exact ACK-replay oracle is `< 0.01%` in Slow Start and `< 1.0%` in pure Reno CA; Table 1 reproduces `BudgetO1` beating static `(2 ln 2, 1)` by `~92-96 ms` at `10 Mbps, q=4,8` (`11` vs `12` and `7` vs `8` drops) and cutting P95 queueing delay by `5.36%` at `q=16,32`.

### B. ns-3.46.1 TCP Loss Runs (Table 3 & Q1/Q5)

```bash
# Inside a clean ns-3.46.1 checkout (or release tarball via `patch -p1 < ...`):
git apply /path/to/SSCP/artifact/patches/ns3-3.46.1-budget-o1.patch
./ns3 configure --build-profile=default --enable-examples
./ns3 build budget-experiment
python3 /path/to/SSCP/artifact/scripts/ns3/run_experiments.py \
    --ns3-dir /path/to/ns-3.46.1 \
    --data-root ./ns3_out
```

**Pass criteria:** All 16 cases (4 profiles × 4 policies: `budget`, `analytic`, `linux`, `flat`) exit `0` with `complete=True`; `ss_iw1_budget` / `ss_iw10_budget` converge to `1.386169` / `1.386200`; `reno_loss_budget` finishes in `2.328 s` vs. `2.883 s` (`linux`); `cubic_loss_budget` records `48` drops (`0` `CA_LOSS`) vs. `596` drops (`analytic`, `139/100`) and `609` drops (`1` `CA_LOSS`, `linux`).

### C. picoquic QUIC Evaluation (Fig. 3b & Q5)

```bash
# Checkout upstream picoquic at commit 9491698 (with picotls built alongside):
git apply /path/to/SSCP/artifact/patches/picoquic-9491698-budget-o1.patch
# Copy artifact/scripts/picoquic/* into exp/ beside picoquic/ and build:
bash exp/build_main_budget.sh
./exp/picoquic_main_budget_exp > exp/run.log
```

**Pass criteria:** All 63 deterministic and 280 multiseed runs return `ok=0`; `BudgetO1` achieves median CUBIC tail-gap CV `0.867` vs. `1.422` (`Linux_20_12`) and `3.652` (`Native_Def`) with zero baseline drift, and outperforms static `(2 ln 2, 1)` (`Analytic_2ln2_1`) by `40.6 ms` median FCT under NewReno.

### D. Linux 6.8 Network-Namespace SSCP/UDP Dual-Tap Captures (Fig. 4b & Q6)

Requires Linux (tested on kernel 6.8) with `iproute2`, `tc`, `ethtool`, `tcpdump`, and `sudo`:

```bash
cd artifact/scripts/linux_udp
gcc -O2 -DSSCP_TICK_SCALE=1000U \
    -I../../.. -I../../../lib/include -I../../../test/common \
    udp_bench.c ../../../test/common/transfer.c ../../../lib/src/*.c \
    -o udp_bench -lm
sudo python3 exp_linux_option_a.py
```

**Pass criteria:** On `100 Mbps, 50 ms RTT, q=350, 8 MiB, spin=1500 us` (`n=3`), `BudgetO1` reduces pre-bottleneck wire micro-bursts (`<50 us`) from `9.20%` (`Linux 2/1.2`) to `~2.2%` (`Analytic (2 ln 2, 1)`: `~2.4%`), and cuts wire tail `CV500` from `~1.00` down to `~0.19` (`Analytic (2 ln 2, 1)`: `~0.20`).
