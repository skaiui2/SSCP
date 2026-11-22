import csv
import json
import math
import os
import re
import statistics
import subprocess
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
BENCH = os.environ.get("UDP_BENCH", str(SCRIPT_DIR / "udp_bench"))
OUT_DIR = Path(os.environ.get("OUT_DIR", str(SCRIPT_DIR / "results_linux_udp")))
TMP_DIR = Path(os.environ.get("TMP_DIR", "/tmp/budget_o1_linux_udp"))


def run(cmd, check=True, timeout=30):
    return subprocess.run(cmd, shell=True, check=check, capture_output=True, text=True, timeout=timeout)


def clean():
    run("pkill -9 -x udp_bench 2>/dev/null || true", check=False)
    run("pkill -9 -x tcpdump 2>/dev/null || true", check=False)
    for ns in ("sscp-c", "sscp-rd", "sscp-rq", "sscp-s"):
        run(f"ip netns del {ns} 2>/dev/null || true", check=False)
    run(f"rm -rf {TMP_DIR} && mkdir -p {TMP_DIR}", check=False)


def build_netns():
    clean()
    for ns in ("sscp-c", "sscp-rd", "sscp-rq", "sscp-s"):
        run(f"ip netns add {ns}")
    run("ip link add veth-c type veth peer name veth-rd0")
    run("ip link add veth-rd1 type veth peer name veth-rq0")
    run("ip link add veth-rq1 type veth peer name veth-s")
    for dev, ns in [
        ("veth-c", "sscp-c"),
        ("veth-rd0", "sscp-rd"),
        ("veth-rd1", "sscp-rd"),
        ("veth-rq0", "sscp-rq"),
        ("veth-rq1", "sscp-rq"),
        ("veth-s", "sscp-s"),
    ]:
        run(f"ip link set {dev} netns {ns}")
    run("ip -n sscp-c addr add 10.0.1.1/24 dev veth-c")
    run("ip -n sscp-rd addr add 10.0.1.254/24 dev veth-rd0")
    run("ip -n sscp-rd addr add 10.0.2.1/24 dev veth-rd1")
    run("ip -n sscp-rq addr add 10.0.2.254/24 dev veth-rq0")
    run("ip -n sscp-rq addr add 10.0.3.254/24 dev veth-rq1")
    run("ip -n sscp-s addr add 10.0.3.1/24 dev veth-s")
    for ns, devs in [
        ("sscp-c", ["lo", "veth-c"]),
        ("sscp-rd", ["lo", "veth-rd0", "veth-rd1"]),
        ("sscp-rq", ["lo", "veth-rq0", "veth-rq1"]),
        ("sscp-s", ["lo", "veth-s"]),
    ]:
        for d in devs:
            run(f"ip -n {ns} link set {d} up")
            if d != "lo":
                run(f"ip netns exec {ns} ethtool -K {d} gro off gso off tso off 2>/dev/null || true", check=False)
    run("ip -n sscp-c route add default via 10.0.1.254 dev veth-c")
    run("ip -n sscp-rd route add 10.0.3.0/24 via 10.0.2.254 dev veth-rd1")
    run("ip -n sscp-rq route add 10.0.1.0/24 via 10.0.2.1 dev veth-rq0")
    run("ip -n sscp-s route add default via 10.0.3.254 dev veth-s")
    for ns in ("sscp-rd", "sscp-rq"):
        run(f"ip netns exec {ns} sysctl -q -w net.ipv4.ip_forward=1")
        run(f"ip netns exec {ns} sysctl -q -w net.ipv4.conf.all.rp_filter=0")
        run(f"ip netns exec {ns} sysctl -q -w net.ipv4.conf.default.rp_filter=0")
    run("ip netns exec sscp-c tc qdisc replace dev veth-c root fq")


def configure_link(rate="100mbit", delay="25ms", netem_extra="", q_pkts=350, qdisc="pfifo"):
    netem_cmd = f"limit 100000 delay {delay}" + (f" {netem_extra}" if netem_extra else "")
    run(f"ip netns exec sscp-rd tc qdisc replace dev veth-rd1 root netem {netem_cmd}")
    run(f"ip netns exec sscp-rd tc qdisc replace dev veth-rd0 root netem limit 100000 delay {delay}")
    for dev in ("veth-rq1", "veth-rq0"):
        run(f"ip netns exec sscp-rq tc qdisc del dev {dev} root 2>/dev/null || true", check=False)
        run(f"ip netns exec sscp-rq tc qdisc replace dev {dev} root handle 1: htb default 10 r2q 100")
        run(f"ip netns exec sscp-rq tc class replace dev {dev} parent 1: classid 1:10 htb rate {rate} ceil {rate} burst 64k cburst 64k")
        if qdisc == "codel" and dev == "veth-rq1":
            run(f"ip netns exec sscp-rq tc qdisc replace dev {dev} parent 1:10 handle 10: codel limit {q_pkts} target 5ms interval 100ms noecn")
        else:
            run(f"ip netns exec sscp-rq tc qdisc replace dev {dev} parent 1:10 handle 10: pfifo limit {q_pkts}")


def parse_rq1_stats():
    txt = run("ip netns exec sscp-rq tc -s qdisc show dev veth-rq1").stdout
    dropped, overlimits, sent_pkts = 0, 0, 0
    lines = txt.splitlines()
    for i, ln in enumerate(lines):
        if ("qdisc pfifo 10:" not in ln) and ("qdisc codel 10:" not in ln):
            continue
        block = "\n".join(lines[i:min(i + 4, len(lines))])
        m_s = re.search(r"Sent \d+ bytes (\d+) pkt \(dropped (\d+), overlimits (\d+)", block)
        if m_s:
            sent_pkts += int(m_s.group(1))
            dropped += int(m_s.group(2))
            overlimits += int(m_s.group(3))
    return {"sent_pkts": sent_pkts, "dropped": dropped, "overlimits": overlimits}


def parse_qdisc_stats(ns, dev, handle_prefix=""):
    txt = run(f"ip netns exec {ns} tc -s qdisc show dev {dev}").stdout
    dropped, overlimits, sent_pkts = 0, 0, 0
    lines = txt.splitlines()
    for i, ln in enumerate(lines):
        if handle_prefix and handle_prefix not in ln:
            continue
        block = "\n".join(lines[i:min(i + 4, len(lines))])
        m_s = re.search(r"Sent \d+ bytes (\d+) pkt \(dropped (\d+), overlimits (\d+)", block)
        if m_s:
            sent_pkts += int(m_s.group(1))
            dropped += int(m_s.group(2))
            overlimits += int(m_s.group(3))
        if handle_prefix:
            break
    return {"sent_pkts": sent_pkts, "dropped": dropped, "overlimits": overlimits}


def parse_pcap_detailed(pcap_path, inspect_hdr=False):
    txt = run(f"tcpdump -r {pcap_path} -tt -nn -q 2>/dev/null", check=False).stdout
    ts = []
    for ln in txt.splitlines():
        parts = ln.split()
        if parts:
            try:
                ts.append(float(parts[0]))
            except ValueError:
                pass
    hdr_sample = None
    if inspect_hdr and len(ts) > 10:
        vtxt = run(f"tcpdump -r {pcap_path} -tt -nn -v -c 3 2>/dev/null", check=False).stdout
        m = re.search(r"length (\d+)\)", vtxt)
        m_u = re.search(r"UDP, length (\d+)", vtxt)
        hdr_sample = {
            "ip_len_bytes": int(m.group(1)) if m else None,
            "udp_payload_bytes": int(m_u.group(1)) if m_u else None,
        }
    if len(ts) < 20:
        return {"pkts": len(ts), "b50_pct": None, "b100_pct": None, "b115_pct": None,
                "min_us": None, "p05_us": None, "p50_us": None, "p95_us": None, "p99_us": None,
                "cv500": None, "ecdf_q": None, "hdr_sample": hdr_sample}
    gaps_us = [(ts[i + 1] - ts[i]) * 1e6 for i in range(len(ts) - 1)]
    gs = sorted(gaps_us)
    n = len(gaps_us)
    b50 = 100.0 * sum(1 for g in gaps_us if g < 50.0) / n
    b100 = 100.0 * sum(1 for g in gaps_us if g < 100.0) / n
    b115 = 100.0 * sum(1 for g in gaps_us if g < 115.0) / n
    tail = gaps_us[-500:] if n >= 500 else gaps_us
    m = sum(tail) / len(tail)
    cv = math.sqrt(sum((g - m) ** 2 for g in tail) / len(tail)) / m if m > 0 else 0.0
    ecdf_q = [round(gs[min(n - 1, int(p * (n - 1) / 100.0))], 2) for p in range(0, 101, 2)]
    return {
        "pkts": len(ts),
        "b50_pct": round(b50, 3),
        "b100_pct": round(b100, 3),
        "b115_pct": round(b115, 3),
        "min_us": round(gs[0], 2),
        "p05_us": round(gs[int(0.05 * (n - 1))], 2),
        "p50_us": round(gs[int(0.50 * (n - 1))], 2),
        "p95_us": round(gs[int(0.95 * (n - 1))], 2),
        "p99_us": round(gs[int(0.99 * (n - 1))], 2),
        "cv500": round(cv, 4),
        "ecdf_q": ecdf_q,
        "hdr_sample": hdr_sample,
    }


def run_dual_tap_one(policy, rep, bytes_sz, port, pargs, capture_s=True, save_ecdf=False, inspect_hdr=False):
    pc = TMP_DIR / "cap_c.pcap"
    ps = TMP_DIR / "cap_s.pcap"
    pc.unlink(missing_ok=True)
    ps.unlink(missing_ok=True)
    rq_before = parse_rq1_stats()

    td_c = subprocess.Popen(
        f"ip netns exec sscp-c tcpdump -i veth-c -nn -s 96 -w {pc} 'udp dst port {port} and greater 200'",
        shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    td_s = None
    if capture_s:
        td_s = subprocess.Popen(
            f"ip netns exec sscp-s tcpdump -i veth-s -nn -s 96 -w {ps} 'udp dst port {port} and greater 200'",
            shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
    srv = subprocess.Popen(
        f"ip netns exec sscp-s {BENCH} --role B --port {port} --bytes {bytes_sz} {pargs}",
        shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(0.25)
    ok = True
    j = {}
    try:
        a = run(f"ip netns exec sscp-c {BENCH} --role A --peer 10.0.3.1 --port {port} --bytes {bytes_sz} {pargs}", timeout=20)
        srv.wait(timeout=5)
        lines = [l for l in a.stdout.splitlines() if l.startswith("{")]
        j = json.loads(lines[-1]) if lines else {}
        ok = bool(j.get("success"))
    except Exception:
        run("pkill -9 -x udp_bench 2>/dev/null || true", check=False)
        try:
            srv.wait(timeout=3)
        except Exception:
            pass
        ok = False

    time.sleep(0.10)
    run("pkill -INT -x tcpdump 2>/dev/null || true", check=False)
    td_c.wait(timeout=4)
    if td_s is not None:
        td_s.wait(timeout=4)

    wc = parse_pcap_detailed(pc, inspect_hdr=False)
    ws = parse_pcap_detailed(ps, inspect_hdr=inspect_hdr) if capture_s else {}
    pc.unlink(missing_ok=True)
    ps.unlink(missing_ok=True)
    rq_after = parse_rq1_stats()
    rd_drop = parse_qdisc_stats("sscp-rd", "veth-rd1", "qdisc netem")["dropped"]

    row = {
        "policy": policy, "rep": rep, "ok": int(ok),
        "fct_ms": j.get("fct_ms"), "goodput_mbps": j.get("goodput_mbps"),
        "retrans": j.get("retrans"), "timeouts": j.get("timeouts"),
        "app_b50_pct": j.get("burst50_pct"), "app_cv500": j.get("cv500"),
        "rtt_p50_ms": j.get("rtt_p50_ms"), "rtt_p95_ms": j.get("rtt_p95_ms"),
        "vc_pkts": wc.get("pkts"), "vc_b50_pct": wc.get("b50_pct"), "vc_b100_pct": wc.get("b100_pct"),
        "vc_min_us": wc.get("min_us"), "vc_p05_us": wc.get("p05_us"), "vc_p50_us": wc.get("p50_us"),
        "vc_p95_us": wc.get("p95_us"), "vc_cv500": wc.get("cv500"),
        "vs_pkts": ws.get("pkts"), "vs_b50_pct": ws.get("b50_pct"), "vs_b100_pct": ws.get("b100_pct"),
        "vs_b115_pct": ws.get("b115_pct"), "vs_min_us": ws.get("min_us"), "vs_p05_us": ws.get("p05_us"),
        "vs_p50_us": ws.get("p50_us"), "vs_p95_us": ws.get("p95_us"), "vs_cv500": ws.get("cv500"),
        "rq1_drop": rq_after["dropped"] - rq_before["dropped"],
        "rq1_overlimits": rq_after["overlimits"] - rq_before["overlimits"],
        "rd1_drop": rd_drop,
    }
    ecdf_obj = None
    if save_ecdf:
        ecdf_obj = {
            "policy": policy, "rep": rep,
            "veth_c_q": wc.get("ecdf_q"),
            "veth_s_q": ws.get("ecdf_q"),
            "hdr_sample": ws.get("hdr_sample"),
        }
    return row, ecdf_obj


def parse_rusage(path):
    txt = Path(path).read_text(encoding="utf-8", errors="replace")
    m = re.search(r"RUSAGE\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)%\s+(\d+)\s+(\d+)", txt)
    if not m:
        return None
    return {
        "wall_s": float(m.group(1)),
        "user_s": float(m.group(2)),
        "sys_s": float(m.group(3)),
        "cpu_pct_str": float(m.group(4)),
        "vol_csw": int(m.group(5)),
        "invol_csw": int(m.group(6)),
    }


def jain(vals):
    s = sum(vals)
    q = sum(v * v for v in vals)
    return (s * s) / (len(vals) * q) if q > 0 else 0.0


def run_cpu_one(policy, nflows, rep, pargs, base_port=6200, seconds=6, warmup=1):
    # Note: ZERO tcpdump observer during CPU/scalability runs so 8 flows on 8 cores have no observer contention.
    srvs = []
    for i in range(nflows):
        port = base_port + i
        p = subprocess.Popen(
            f"ip netns exec sscp-s {BENCH} --role B --port {port} --seconds {seconds} --warmup 0 {pargs}",
            shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        srvs.append(p)
    time.sleep(0.30)

    rq_before = parse_rq1_stats()
    clis = []
    out_files = []
    time_files = []
    for i in range(nflows):
        port = base_port + i
        of = TMP_DIR / f"cli_{i}.out"
        tf = TMP_DIR / f"cli_{i}.time"
        of.unlink(missing_ok=True)
        tf.unlink(missing_ok=True)
        cmd = (
            f"ip netns exec sscp-c /usr/bin/time -f 'RUSAGE %e %U %S %P %w %c' -o {tf} "
            f"{BENCH} --role A --peer 10.0.3.1 --port {port} --seconds {seconds} --warmup {warmup} {pargs} > {of} 2>/dev/null"
        )
        clis.append(subprocess.Popen(cmd, shell=True))
        out_files.append(of)
        time_files.append(tf)

    ok = True
    try:
        for p in clis:
            rc = p.wait(timeout=seconds + 20)
            if rc != 0:
                ok = False
        for p in srvs:
            p.wait(timeout=10)
    except Exception:
        run("pkill -9 -x udp_bench 2>/dev/null || true", check=False)
        ok = False

    rq_after = parse_rq1_stats()
    flow_stats = []
    for of, tf in zip(out_files, time_files):
        ru = parse_rusage(tf) if tf.exists() else None
        js = {}
        if of.exists():
            lines = [l for l in of.read_text(encoding="utf-8", errors="replace").splitlines() if l.startswith("{")]
            if lines:
                js = json.loads(lines[-1])
        if not ru or not js.get("success"):
            ok = False
        flow_stats.append((js, ru or {}))
        of.unlink(missing_ok=True)
        tf.unlink(missing_ok=True)

    mbps_list = [js.get("goodput_mbps", 0.0) for js, _ in flow_stats]
    user_sum = sum(ru.get("user_s", 0.0) for _, ru in flow_stats)
    sys_sum = sum(ru.get("sys_s", 0.0) for _, ru in flow_stats)
    wall_mean = statistics.mean([max(0.01, ru.get("wall_s", seconds)) for _, ru in flow_stats])
    cpu_tot_pct = 100.0 * (user_sum + sys_sum) / wall_mean
    vol_sum = sum(ru.get("vol_csw", 0) for _, ru in flow_stats)
    invol_sum = sum(ru.get("invol_csw", 0) for _, ru in flow_stats)

    return {
        "policy": policy, "nflows": nflows, "rep": rep, "ok": int(ok),
        "total_mbps": round(sum(mbps_list), 3),
        "per_flow_mbps": round(statistics.mean(mbps_list), 3) if mbps_list else 0.0,
        "jain": round(jain(mbps_list), 5) if ok else 0.0,
        "wall_mean_s": round(wall_mean, 3),
        "user_s_sum": round(user_sum, 4),
        "sys_s_sum": round(sys_sum, 4),
        "cpu_total_pct": round(cpu_tot_pct, 2),
        "cpu_per_flow_pct": round(cpu_tot_pct / nflows, 2),
        "user_per_flow_pct": round(100.0 * user_sum / wall_mean / nflows, 2),
        "sys_per_flow_pct": round(100.0 * sys_sum / wall_mean / nflows, 2),
        "vol_csw_per_flow_s": round(vol_sum / (wall_mean * nflows), 2),
        "invol_csw_per_flow_s": round(invol_sum / (wall_mean * nflows), 2),
        "app_b50_mean": round(statistics.mean([js.get("burst50_pct", 0.0) for js, _ in flow_stats]), 3),
        "rtt_p95_mean": round(statistics.mean([js.get("rtt_p95_ms", 0.0) for js, _ in flow_stats]), 3),
        "sum_retrans": sum(js.get("retrans", 0) for js, _ in flow_stats),
        "rq1_drop": rq_after["dropped"] - rq_before["dropped"],
    }


def summarize_rows(rows, group_keys, metric_keys):
    grouped = {}
    for r in rows:
        grouped.setdefault(tuple(r[k] for k in group_keys), []).append(r)
    out = {}
    for gk, grp in grouped.items():
        ok_grp = [x for x in grp if x["ok"]]
        metrics = {}
        for mk in metric_keys:
            vals = [x[mk] for x in ok_grp if x.get(mk) is not None]
            if not vals:
                metrics[mk] = {"mean": None, "std": None, "ci95": None}
            elif len(vals) == 1:
                metrics[mk] = {"mean": round(vals[0], 4), "std": 0.0, "ci95": 0.0}
            else:
                m = statistics.mean(vals)
                sd = statistics.stdev(vals)
                # Student-t critical ~ approx 95% for small n via exact table up to 30
                tcrit = {2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776, 10: 2.262, 15: 2.145, 30: 2.045}.get(len(vals), 1.96)
                ci = tcrit * sd / math.sqrt(len(vals))
                metrics[mk] = {"mean": round(m, 4), "std": round(sd, 4), "ci95": round(ci, 4)}
        key_str = "::".join(str(x) for x in gk)
        out[key_str] = {"n_total": len(grp), "n_ok": len(ok_grp), "metrics": metrics}
    return out


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    port_idx = 0
    try:
        build_netns()

        # --- EXP A3: Dual-tap veth-c vs veth-s ---
        print("=== EXP A3: Dual-Tap (100M_25ms_q350, veth-c vs veth-s) ===", flush=True)
        dual_pols = [
            ("BudgetO1_Reno_spin0",    "--cc 1 --budget 1 --rttvar-k 32768 --spin-us 0"),
            ("BudgetO1_Reno_spin250",  "--cc 1 --budget 1 --rttvar-k 32768 --spin-us 250"),
            ("BudgetO1_Reno_spin500",  "--cc 1 --budget 1 --rttvar-k 32768 --spin-us 500"),
            ("BudgetO1_Reno_spin1500", "--cc 1 --budget 1 --rttvar-k 32768 --spin-us 1500"),
            ("BudgetO1_CUBIC_spin500", "--cc 2 --budget 1 --rttvar-k 32768 --spin-us 500"),
            ("Linux_2_12_Reno",        "--cc 1 --budget 0 --ss-g 131072 --ca-g 78643 --spin-us 1500"),
            ("Analytic_2ln2_1_Reno",   "--cc 1 --budget 0 --ss-g 90852 --ca-g 65536 --spin-us 1500"),
            ("Static_10_Reno",         "--cc 1 --budget 0 --ss-g 65536 --ca-g 65536 --spin-us 1500"),
        ]
        dual_rows = []
        ecdf_data = []
        for rep in range(1, 4):
            for pname, pargs in dual_pols:
                configure_link("100mbit", "25ms", "", 350, "pfifo")
                port = 6100 + (port_idx % 400)
                port_idx += 1
                save_ecdf = (rep == 1 and pname in {"BudgetO1_Reno_spin0", "BudgetO1_Reno_spin500", "Linux_2_12_Reno", "Analytic_2ln2_1_Reno"})
                inspect_hdr = (rep == 1 and pname == "BudgetO1_Reno_spin500")
                row, ecdf_obj = run_dual_tap_one(pname, rep, 8388608, port, pargs, capture_s=True, save_ecdf=save_ecdf, inspect_hdr=inspect_hdr)
                dual_rows.append(row)
                if ecdf_obj:
                    ecdf_data.append(ecdf_obj)
                print(f"  [r{rep}] {pname:<24s} ok={row['ok']} fct={row['fct_ms']} rqDrop={row['rq1_drop']} "
                      f"| veth-c(<50={row['vc_b50_pct']}%,p50={row['vc_p50_us']},cv={row['vc_cv500']}) "
                      f"| veth-s(min={row['vs_min_us']},p05={row['vs_p05_us']},p50={row['vs_p50_us']},<100={row['vs_b100_pct']}%,cv={row['vs_cv500']})", flush=True)

        with (OUT_DIR / "exp_a3_dual_tap_runs.csv").open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(dual_rows[0].keys()))
            w.writeheader()
            w.writerows(dual_rows)
        dual_sum = summarize_rows(dual_rows, ["policy"], [
            "fct_ms", "goodput_mbps", "retrans", "rtt_p95_ms", "app_b50_pct",
            "vc_b50_pct", "vc_b100_pct", "vc_min_us", "vc_p05_us", "vc_p50_us", "vc_p95_us", "vc_cv500",
            "vs_b50_pct", "vs_b100_pct", "vs_b115_pct", "vs_min_us", "vs_p05_us", "vs_p50_us", "vs_p95_us", "vs_cv500",
            "rq1_drop", "rq1_overlimits"
        ])
        (OUT_DIR / "exp_a3_dual_tap_summary.json").write_text(json.dumps(dual_sum, indent=2))
        (OUT_DIR / "exp_a3_dual_tap_ecdf.json").write_text(json.dumps(ecdf_data, indent=2))

        # --- EXP A2: Spin CPU & 1/8-Flow Scalability ---
        print("\n=== EXP A2: spin_us CPU & 1/8-Flow Scalability (100M_25ms_q350) ===", flush=True)
        cpu_pols = [
            ("BudgetO1_Reno_spin0",     "--cc 1 --budget 1 --rttvar-k 32768 --spin-us 0"),
            ("BudgetO1_Reno_spin250",   "--cc 1 --budget 1 --rttvar-k 32768 --spin-us 250"),
            ("BudgetO1_Reno_spin500",   "--cc 1 --budget 1 --rttvar-k 32768 --spin-us 500"),
            ("BudgetO1_Reno_spin1500",  "--cc 1 --budget 1 --rttvar-k 32768 --spin-us 1500"),
            ("Linux_2_12_Reno_spin500", "--cc 1 --budget 0 --ss-g 131072 --ca-g 78643 --spin-us 500"),
            ("Linux_2_12_Reno_spin1500","--cc 1 --budget 0 --ss-g 131072 --ca-g 78643 --spin-us 1500"),
        ]
        cpu_rows = []
        for nflows in [1, 8]:
            for rep in range(1, 4):
                for pname, pargs in cpu_pols:
                    configure_link("100mbit", "25ms", "", 350, "pfifo")
                    row = run_cpu_one(pname, nflows, rep, pargs, base_port=6300 + (port_idx % 200), seconds=6, warmup=1)
                    port_idx += nflows
                    cpu_rows.append(row)
                    print(f"  [N={nflows} r{rep}] {pname:<25s} ok={row['ok']} mbps={row['total_mbps']:6.2f} jain={row['jain']:.4f} "
                          f"cpuTot={row['cpu_total_pct']:6.1f}% (usr={row['user_per_flow_pct']:5.1f}%,sys={row['sys_per_flow_pct']:4.1f}%/flow) "
                          f"vcsw/s={row['vol_csw_per_flow_s']:6.0f} drop={row['rq1_drop']}", flush=True)

        with (OUT_DIR / "exp_a2_cpu_runs.csv").open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(cpu_rows[0].keys()))
            w.writeheader()
            w.writerows(cpu_rows)
        cpu_sum = summarize_rows(cpu_rows, ["nflows", "policy"], [
            "total_mbps", "per_flow_mbps", "jain", "cpu_total_pct", "cpu_per_flow_pct",
            "user_per_flow_pct", "sys_per_flow_pct", "vol_csw_per_flow_s",
            "invol_csw_per_flow_s", "app_b50_mean", "rtt_p95_mean", "sum_retrans", "rq1_drop"
        ])
        (OUT_DIR / "exp_a2_cpu_summary.json").write_text(json.dumps(cpu_sum, indent=2))

        # --- EXP A4: Analytic_2ln2_1 across L2 regimes & 10-rep loss005 ---
        print("\n=== EXP A4: Analytic_2ln2_1 L2 & 10-rep 100M_25ms_loss005 ===", flush=True)
        a4_regimes = [
            ("50M_25ms_q600",     "50mbit",  "25ms", "",           600,  "pfifo", 4194304, 3, True,
             [("Analytic_2ln2_1_Reno", "--cc 1 --budget 0 --ss-g 90852 --ca-g 65536 --spin-us 1500")]),
            ("100M_25ms_jit5_q600","100mbit","25ms", "5ms 95% rate 150mbit", 600, "pfifo", 8388608, 3, True,
             [("Analytic_2ln2_1_Reno", "--cc 1 --budget 0 --ss-g 90852 --ca-g 65536 --spin-us 1500")]),
            ("100M_25ms_codel",   "100mbit", "25ms", "",           1000, "codel", 8388608, 3, True,
             [("Analytic_2ln2_1_Reno", "--cc 1 --budget 0 --ss-g 90852 --ca-g 65536 --spin-us 1500")]),
            ("100M_25ms_loss005", "100mbit", "25ms", "loss 0.05%", 600,  "pfifo", 4194304, 10, False,
             [("BudgetO1_Reno_k05",  "--cc 1 --budget 1 --rttvar-k 32768 --spin-us 500"),
              ("BudgetO1_CUBIC_k05", "--cc 2 --budget 1 --rttvar-k 32768 --spin-us 500"),
              ("Linux_2_12_Reno",    "--cc 1 --budget 0 --ss-g 131072 --ca-g 78643 --spin-us 1500"),
              ("Analytic_2ln2_1_Reno", "--cc 1 --budget 0 --ss-g 90852 --ca-g 65536 --spin-us 1500"),
              ("Static_10_Reno",     "--cc 1 --budget 0 --ss-g 65536 --ca-g 65536 --spin-us 1500")]),
        ]
        a4_rows = []
        for rname, rate, delay, nextra, qp, qdisc, bsz, reps, cap, pols in a4_regimes:
            print(f"  -- Regime {rname} --", flush=True)
            for rep in range(1, reps + 1):
                for pname, pargs in pols:
                    configure_link(rate, delay, nextra, qp, qdisc)
                    port = 6600 + (port_idx % 300)
                    port_idx += 1
                    row, _ = run_dual_tap_one(pname, rep, bsz, port, pargs, capture_s=cap, save_ecdf=False, inspect_hdr=False)
                    row["regime"] = rname
                    a4_rows.append(row)
                    print(f"    [r{rep}] {pname:<20s} ok={row['ok']} fct={row['fct_ms']} gp={row['goodput_mbps']} "
                          f"retr={row['retrans']} vc_b50={row['vc_b50_pct']} vs_cv={row['vs_cv500']} rqDrop={row['rq1_drop']}", flush=True)

        with (OUT_DIR / "exp_a4_linux_runs.csv").open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(a4_rows[0].keys()))
            w.writeheader()
            w.writerows(a4_rows)
        a4_sum = summarize_rows(a4_rows, ["regime", "policy"], [
            "fct_ms", "goodput_mbps", "retrans", "rtt_p95_ms", "app_b50_pct",
            "vc_b50_pct", "vc_cv500", "vs_b50_pct", "vs_b100_pct", "vs_p50_us", "vs_cv500", "rq1_drop"
        ])
        (OUT_DIR / "exp_a4_linux_summary.json").write_text(json.dumps(a4_sum, indent=2))
        print("\nALL OPTION-A LINUX EXPERIMENTS DONE.", flush=True)
    finally:
        clean()


if __name__ == "__main__":
    main()
