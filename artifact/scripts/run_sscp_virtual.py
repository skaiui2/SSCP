"""Reproduce the SSCP virtual-network evaluations reported in the CCR paper:
  - Q1: Fold-on-Send vs. exact packet-by-packet ACK-replay oracle B(L)
  - Fig. 1b: Unbounded Slow-Start gain sweep
  - Table 1: Analytical (2*ln2, 1) shallow-queue head-to-head
  - Table 2: 30-seed jittered CUBIC scheduler ablation
  - Fig. 3a: Cross-regime single-flow CUBIC (30-seed jitter, reordering, q=20)
  - Fig. 4a: Multi-flow CUBIC fairness (2/jit 15 seeds, 4/stag, 8/db)

Usage:
    cmake -S test -B test/build -DCMAKE_BUILD_TYPE=Release
    cmake --build test/build -j
    python3 artifact/scripts/run_sscp_virtual.py --build-dir test/build --quick
    python3 artifact/scripts/run_sscp_virtual.py --build-dir test/build --full
"""

import argparse
import csv
import json
import math
import statistics
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

MSS = 1436
CWND_MAX = 1 << 20


def find_exe(build_dir: Path, name: str) -> Path:
    for cand in (build_dir / name, build_dir / f"{name}.exe",
                 build_dir / "Release" / f"{name}.exe"):
        if cand.exists():
            return cand
    raise FileNotFoundError(f"Cannot find {name} in {build_dir}")


def oracle_full_window_aimd(w_k: int, ssth: int) -> float:
    w, cnt, rem, work = w_k, 0, w_k, 0.0
    while rem > 0:
        seg = min(MSS, rem)
        rem -= seg
        if w < ssth:
            nw = min(CWND_MAX, w + seg)
        else:
            num = MSS * seg + cnt
            inc = num // w
            cnt = num - inc * w
            nw = min(CWND_MAX, w + inc)
        work += (seg + (nw - w)) / float(nw)
        w = nw
    return work


def run_q1_oracle(v_exe: Path, work_dir: Path) -> None:
    print("\n=== Q1: Fold-on-Send vs. Exact ACK-Replay Horizon Oracle B(L) ===")
    cases = [
        ("SS_unbounded_4M", ["--bytes", "4194304", "--mbps", "100", "--delay-ms", "25",
                             "--queue-packets", "0", "--cc", "1", "--budget-pacing", "1", "--rttvar-k", "0"]),
        ("Reno_SS_CA_q16",  ["--bytes", "524288", "--mbps", "10", "--delay-ms", "50",
                             "--queue-packets", "16", "--ssthresh-bytes", str(96 * MSS),
                             "--cc", "1", "--budget-pacing", "1", "--rttvar-k", "0"]),
        ("Reno_CA_2M",      ["--bytes", "2097152", "--mbps", "100", "--delay-ms", "25",
                             "--queue-packets", "0", "--cc", "1", "--start-ca", "1",
                             "--budget-pacing", "1", "--rttvar-k", "0"]),
    ]
    for name, args in cases:
        d = work_dir / f"q1_{name}"
        d.mkdir(parents=True, exist_ok=True)
        ev = d / "events.csv"
        subprocess.run([str(v_exe)] + args + ["--events-csv", str(ev)],
                       cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        total = int(args[args.index("--bytes") + 1])
        errs, sent = [], 0
        prev_gq, prev_w, prev_ss = None, None, None
        with ev.open("r", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                if r["direction"] == "0" and r["event"] == "send":
                    sent += int(r["payload_bytes"])
                elif r["direction"] == "1" and r["event"] == "arrival":
                    gq, w, ss = int(r["gain_q16"]), int(r["cwnd"]), int(r["ssthresh"])
                    if prev_gq is not None and gq != prev_gq and prev_w is not None:
                        if prev_w >= 2 * MSS and (total - sent) > prev_w:
                            g_or = max(1.0, oracle_full_window_aimd(prev_w, prev_ss))
                            errs.append(abs(gq / 65536.0 - g_or) / g_or * 100.0)
                    prev_gq, prev_w, prev_ss = gq, w, ss
        ev.unlink(missing_ok=True)
        print(f"  {name:<18}: n={len(errs):2d} refreshes | median_err={statistics.median(errs):.4f}% | max_err={max(errs):.4f}%")


def run_table1(v_exe: Path, work_dir: Path) -> None:
    print("\n=== Table 1: Analytical (2*ln2, 1) Shallow-Queue Head-to-Head (512 KiB, 100 ms RTT, ssth=96 MSS) ===")
    print(f"  {'Rate/q':<10} | {'Policy':<16} | {'FCT (ms)':>10} | {'P95 QDelay':>10} | {'Drops':>5} | {'RTO':>3}")
    for rate in (10, 20):
        for q in (4, 8, 16, 32):
            for pname, extra in [
                ("Static (2ln2,1)", ["--budget-pacing", "0", "--ss-g", "90852", "--ca-g", "65536", "--rttvar-k", "0"]),
                ("BudgetO1",        ["--budget-pacing", "1", "--rttvar-k", "0"]),
            ]:
                d = work_dir / f"t1_r{rate}_q{q}_{pname[:6].strip()}"
                d.mkdir(parents=True, exist_ok=True)
                subprocess.run([
                    str(v_exe), "--bytes", "524288", "--mbps", str(rate), "--delay-ms", "50",
                    "--queue-packets", str(q), "--ssthresh-bytes", str(96 * MSS), "--cc", "1",
                ] + extra, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                s = json.loads((d / "summary.json").read_text(encoding="utf-8"))
                print(f"  {rate:>2}M/q={q:<4} | {pname:<16} | {s['data_fct_ms']:>10.3f} | {s['qdelay_p95_ms']:>10.3f} | {s['queue_dropped']:>5} | {s['timeouts']:>3}")


def run_fig1b(v_exe: Path, work_dir: Path) -> None:
    print("\n=== Figure 1b: SSCP Unbounded Slow-Start Gain Sweep (4 MiB, 100 Mbps, 50 ms RTT, q=0) ===")
    for g in (1.0, 1.3862943611, 2.0, 2.77, -1.0):
        if g < 0:
            label, extra = "BudgetO1", ["--budget-pacing", "1", "--rttvar-k", "0"]
        else:
            label = f"Static_{g:.4f}"
            extra = ["--budget-pacing", "0", "--ss-g", str(int(round(g * 65536))), "--ca-g", "65536", "--rttvar-k", "0"]
        d = work_dir / f"f1b_{label}"
        d.mkdir(parents=True, exist_ok=True)
        ev = d / "events.csv"
        subprocess.run([
            str(v_exe), "--bytes", "4194304", "--mbps", "100", "--delay-ms", "25",
            "--queue-packets", "0", "--cc", "1", "--events-csv", str(ev),
        ] + extra, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        s = json.loads((d / "summary.json").read_text(encoding="utf-8"))
        send_t = [float(r["t_ms"]) * 1000.0 for r in csv.DictReader(ev.open("r", encoding="utf-8"))
                  if r["direction"] == "0" and r["event"] == "send"]
        ev.unlink(missing_ok=True)
        gaps = [send_t[i] - send_t[i - 1] for i in range(1, len(send_t))]
        b50 = 100.0 * sum(1 for dt in gaps if dt < 50.0) / len(gaps) if gaps else 0.0
        print(f"  {label:<15}: FCT={s['simulated_ms']:>8.3f} ms | Gaps<50us={b50:6.3f}%")


def run_full_cubic(v_exe: Path, m_exe: Path, work_dir: Path, workers: int) -> None:
    print("\n=== Table 2, Figure 3a & Figure 4a: Full Multi-Seed CUBIC Evaluation ===")
    policies = [
        ("BudgetO1",        ["--budget-pacing", "1"]),
        ("Analytic_2ln2_1", ["--budget-pacing", "0", "--ss-g", "90852", "--ca-g", "65536"]),
        ("Linux_2_12",      ["--budget-pacing", "0", "--ss-g", "131072", "--ca-g", "78643"]),
        ("Static_10",       ["--budget-pacing", "0", "--ss-g", "65536", "--ca-g", "65536"]),
    ]

    def run_single(task):
        scen, cfg, seed, pname, pflags = task
        d = work_dir / f"s_{scen}_s{seed}_{pname}"
        d.mkdir(parents=True, exist_ok=True)
        ev = d / "events.csv"
        cmd = [
            str(v_exe), "--bytes", str(cfg["bytes"]), "--mbps", str(cfg["mbps"]),
            "--delay-ms", str(cfg["delay"]), "--queue-packets", str(cfg["q"]),
            "--jitter-ms", str(cfg["jitter"]), "--reorder", str(cfg["reorder"]),
            "--seed", str(seed), "--cc", "2", "--timeout-ms", "150000", "--events-csv", str(ev),
        ] + cfg.get("extra", []) + pflags
        subprocess.run(cmd, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        s = json.loads((d / "summary.json").read_text(encoding="utf-8"))
        send_t = [float(r["t_ms"]) for r in csv.DictReader(ev.open("r", encoding="utf-8"))
                  if r["direction"] == "0" and r["event"] == "send"]
        ev.unlink(missing_ok=True)
        gaps = [send_t[i] - send_t[i - 1] for i in range(1, len(send_t))]
        tail = gaps[-min(500, len(gaps)):]
        cv500 = statistics.pstdev(tail) / statistics.mean(tail) if len(tail) > 10 else 0.0
        return scen, pname, s["simulated_ms"], s["queue_dropped"], s["timeouts"], cv500

    scens = [
        ("Jit5_NoReord_q50", range(1, 31), dict(bytes=2097152, mbps=100, delay=25, q=50, jitter=5, reorder=0)),
        ("Jit5_Reord_q50",   range(1, 31), dict(bytes=2097152, mbps=100, delay=25, q=50, jitter=5, reorder=1)),
        ("BW_100M_q20_vsh",  [1],          dict(bytes=2097152, mbps=100, delay=25, q=20, jitter=0, reorder=0,
                                                extra=["--ssthresh-bytes", str(400 * MSS)])),
    ]
    ablation_policies = [
        ("BudgetO1_k0",  ["--budget-pacing", "1", "--rttvar-k", "0"]),
        ("BudgetO1_k10", ["--budget-pacing", "1", "--rttvar-k", "65536"]),
        ("BudgetO1_k20", ["--budget-pacing", "1", "--rttvar-k", "131072"]),
    ]
    tasks = [(sc, cfg, s, pn, pf) for sc, seeds, cfg in scens for s in seeds for pn, pf in policies]
    jit_cfg = scens[0][2]
    tasks += [("Jit5_NoReord_q50", jit_cfg, s, pn, pf)
              for s in range(1, 31) for pn, pf in ablation_policies]
    with ThreadPoolExecutor(max_workers=workers) as ex:
        res = list(ex.map(run_single, tasks))
    for sc, _, _ in scens:
        for pn, _ in policies:
            grp = [r for r in res if r[0] == sc and r[1] == pn]
            fcts = [r[2] for r in grp]
            ci = 1.96 * statistics.stdev(fcts) / math.sqrt(len(fcts)) if len(fcts) > 1 else 0.0
            print(f"  {sc:<17} | {pn:<15} | FCT={statistics.mean(fcts):7.1f}+-{ci:5.1f} ms | Drops={statistics.mean(r[3] for r in grp):5.1f} | CV500={statistics.mean(r[5] for r in grp):.4f}")

    print("\n=== Table 2: Scheduler Ablation (30-seed Jittered CUBIC, q=50) ===")
    tab2_order = [
        ("BudgetO1",        "BudgetO1 (k=0.5)"),
        ("BudgetO1_k0",     "BudgetO1 (k=0)"),
        ("BudgetO1_k10",    "BudgetO1 (k=1.0)"),
        ("BudgetO1_k20",    "BudgetO1 (k=2.0)"),
        ("Analytic_2ln2_1", "Static (2ln2,1) (k=0.5)"),
    ]
    for pn, label in tab2_order:
        grp = [r for r in res if r[0] == "Jit5_NoReord_q50" and r[1] == pn]
        fcts = [r[2] / 1000.0 for r in grp]
        ci = 1.96 * statistics.stdev(fcts) / math.sqrt(len(fcts))
        print(f"  {label:<24} | MeanFCT={statistics.mean(fcts):.3f} s | CI95={ci:.3f} s | TailCV500={statistics.mean(r[5] for r in grp):.3f}")

    # Multi-flow Figure 4a
    mf_cases = [
        ("N2_100M_jit5",   range(1, 16), 2, 2097152, 100, 25, 50, 5, 0, 0, 0),
        ("N4_50M_stag",    [1],          4, 4194304,  50, 25, 50, 0, 64, 1000, 0),
        ("N8_100M_q80_ds", [1],          8, 1048576, 100, 25, 80, 0, 54, 0, 1),
    ]
    for topo, seeds, n, bsz, mbps, delay, q, jit, ssth, stag, desync in mf_cases:
        for mix, bud, ssg, cag in [("Homo_CUBIC_Bud", 1, 0, 0), ("Homo_CUBIC_Lin", 0, 131072, 78643)]:
            jains, drops, fcts = [], [], []
            for seed in seeds:
                d = work_dir / f"mf_{topo}_{mix}_s{seed}"
                d.mkdir(parents=True, exist_ok=True)
                cmd = [str(m_exe), "--nflows", str(n), "--bytes", str(bsz), "--mbps", str(mbps),
                       "--delay-ms", str(delay), "--queue-packets", str(q), "--jitter-ms", str(jit),
                       "--seed", str(seed), "--timeout-ms", "180000"]
                for i in range(n):
                    st = (stag + i) if (stag > 0 and i >= n // 2) else (i if (desync or stag > 0) else 0)
                    cmd += ["--flow", f"{i}:2:{bud}:{ssg}:{cag}:{ssth}:{st}"]
                subprocess.run(cmd, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                s = json.loads((d / "summary_multi.json").read_text(encoding="utf-8"))
                jains.append(s["jain"])
                drops.append(s["queue_dropped"])
                fcts.append(statistics.mean(s["fcts_ms"]))
            print(f"  {topo:<17} | {mix:<14} | Jain={statistics.mean(jains):.4f} | MeanFCT={statistics.mean(fcts):7.1f} ms | Drops={statistics.mean(drops):.1f}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", type=Path, default=Path("test/build"))
    ap.add_argument("--quick", action="store_true", help="Run Q1 Oracle, Fig. 1b, and Table 1 (~5 s)")
    ap.add_argument("--full", action="store_true", help="Also run full multi-seed Fig. 3a and Fig. 4a")
    ap.add_argument("--workers", type=int, default=4)
    args = ap.parse_args()

    v_exe = find_exe(args.build_dir, "sscp_virtual")
    with tempfile.TemporaryDirectory(prefix="budget_o1_virtual_") as tmp:
        work = Path(tmp)
        run_q1_oracle(v_exe, work)
        run_fig1b(v_exe, work)
        run_table1(v_exe, work)
        if args.full and not args.quick:
            m_exe = find_exe(args.build_dir, "sscp_multiflow")
            run_full_cubic(v_exe, m_exe, work, args.workers)


if __name__ == "__main__":
    main()
