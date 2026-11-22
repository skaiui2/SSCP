import csv
from statistics import median

CC_NAMES = {0: "PROB", 1: "AIMD", 2: "CUBIC"}


def truth(value):
    return str(value).lower() == "true"


def passed(row):
    return truth(row.get("success", False))


def fval(row, key, default=0.0):
    val = row.get(key)
    return float(val) if val not in (None, "") else default


def ival(row, key, default=0):
    val = row.get(key)
    return int(float(val)) if val not in (None, "") else default


def cc_label(row):
    name = row.get("cc_name")
    if name:
        return str(name).upper()
    return CC_NAMES.get(ival(row, "cc", 1), "AIMD")


def case_key(row):
    return (
        cc_label(row),
        row.get("scenario", "network"),
        ival(row, "mbps"),
        ival(row, "delay_ms"),
        ival(row, "queue_packets"),
        ival(row, "bytes"),
        ival(row, "loss_ppm"),
        ival(row, "seed"),
    )


def env_key(row):
    return (
        cc_label(row),
        row.get("scenario", "network"),
        ival(row, "mbps"),
        ival(row, "delay_ms"),
        ival(row, "queue_packets"),
        ival(row, "bytes"),
        ival(row, "loss_ppm"),
    )


def fmt_delta_f(cur, prev, fmt=".2f", suffix=""):
    diff = cur - prev
    return f" ({diff:+{fmt}}{suffix})"


def fmt_delta_i(cur, prev):
    diff = cur - prev
    return f" ({diff:+d})"


def write_report(root, rows, expected=None, baseline_rows=None, baseline_label="previous run"):
    keys = list(dict.fromkeys(k for row in rows for k in row))
    with (root / "sweep.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)

    count = sum(passed(row) for row in rows)
    total = expected or len(rows)
    lines = [f"# SSCP Network Sweep Report: {count}/{total} passed", ""]
    if expected is not None and len(rows) != expected:
        lines += [f"**FAIL**: expected {expected} reports, received {len(rows)}.", ""]

    base_cases = {}
    base_envs = {}
    if baseline_rows:
        for b in baseline_rows:
            base_cases[case_key(b)] = b
            base_envs.setdefault(env_key(b), []).append(b)

    failed = [row for row in rows if not passed(row)]
    if failed:
        lines += [
            f"## Failed Cases ({len(failed)})",
            "",
            "| CC | Scenario | MiB | Link / RTT / Queue | Loss % | Seed | Failure Reason | Time (s) | RTOs | Repro Command |",
            "|:---|:---|---:|:---|---:|---:|:---|---:|---:|:---|",
        ]
        for r in failed:
            mib = f"{ival(r, 'bytes') / 2**20:g}"
            env = f"{ival(r, 'mbps')}M / {ival(r, 'delay_ms') * 2}ms / q{ival(r, 'queue_packets')}"
            loss = f"{ival(r, 'loss_ppm') / 10000:g}%"
            reason = r.get("failure_reason") or "transfer failed"
            repro = "`" + r.get("repro_command", "unavailable") + "`"
            lines.append(
                f"| {cc_label(r)} | {r.get('scenario', 'network')} | {mib} | {env} | {loss} | {ival(r, 'seed')} "
                f"| {reason} | {fval(r, 'time_s'):.2f} | {ival(r, 'timeouts')} | {repro} |"
            )
        lines.append("")

    groups = {}
    for r in rows:
        groups.setdefault(env_key(r), []).append(r)

    title = f"## Environment Summary (vs {baseline_label})" if baseline_rows else "## Environment Summary"
    lines += [
        title,
        "",
        "| Scenario (Link / RTT / Queue) | Loss % | CC | Pass | Time (s) [med (Δ) / max] | Goodput Mbps (Δ) | Retrans % (Δ) | RTOs [med (Δ) / max] | SRTT ms [avg (Δ) / max] |",
        "|:---|---:|:---|:---:|---:|---:|---:|---:|---:|",
    ]
    by_scen = sorted(groups.items(), key=lambda kv: (kv[0][1:], kv[0][0]))
    for key, group in by_scen:
        cc_name, scen, mbps, delay, q, size, loss_ppm = key
        ok_group = [r for r in group if passed(r)]
        active = ok_group or group
        label = f"{scen} ({mbps}M / {2 * delay}ms / q{q})"
        loss_str = f"{loss_ppm / 10000:g}%"
        pass_str = f"{len(ok_group)}/{len(group)}"

        med_t = median(fval(r, "time_s") for r in active)
        max_t = max(fval(r, "time_s") for r in active)
        med_g = median(fval(r, "goodput_mbps") for r in active)
        med_ret = median(fval(r, "retrans_pct") for r in active)
        med_rto = int(median(ival(r, "timeouts") for r in active))
        max_rto = max(ival(r, "timeouts") for r in active)
        med_srtt = median(fval(r, "srtt_avg_ms") for r in active)
        max_srtt = max(fval(r, "srtt_max_ms") for r in active)

        dt, dg, dret, drto, dsrtt = "", "", "", "", ""
        if key in base_envs:
            b_active = [r for r in base_envs[key] if passed(r)] or base_envs[key]
            dt = fmt_delta_f(med_t, median(fval(r, "time_s") for r in b_active), ".2f", "s")
            dg = fmt_delta_f(med_g, median(fval(r, "goodput_mbps") for r in b_active), ".2f")
            dret = fmt_delta_f(med_ret, median(fval(r, "retrans_pct") for r in b_active), ".2f", "%")
            drto = fmt_delta_i(med_rto, int(median(ival(r, "timeouts") for r in b_active)))
            dsrtt = fmt_delta_f(med_srtt, median(fval(r, "srtt_avg_ms") for r in b_active), ".1f")

        lines.append(
            f"| {label} | {loss_str} | {cc_name} | {pass_str} "
            f"| {med_t:.2f}{dt} / {max_t:.2f} "
            f"| {med_g:.2f}{dg} "
            f"| {med_ret:.2f}%{dret} "
            f"| {med_rto}{drto} / {max_rto} "
            f"| {med_srtt:.1f}{dsrtt} / {max_srtt:.1f} |"
        )

    lines += [
        "",
        f"<details><summary>All individual runs ({len(rows)} cases)</summary>",
        "",
        "| Scenario | MiB | Link / RTT / Queue | Loss % | CC | Seed | Result | Time (s) (Δ) | Goodput Mbps (Δ) | Retrans % (Δ) | RTOs (Δ) | SRTT ms [avg / max] |",
        "|:---|---:|:---|---:|:---|---:|:---:|---:|---:|---:|---:|---:|",
    ]
    for r in sorted(rows, key=lambda x: (x.get("scenario", ""), ival(x, "loss_ppm"), cc_label(x), ival(x, "seed"))):
        ok = passed(r)
        mib = f"{ival(r, 'bytes') / 2**20:g}"
        mbps = ival(r, "mbps")
        env = f"{mbps}M / {ival(r, 'delay_ms') * 2}ms / q{ival(r, 'queue_packets')}"
        loss_str = f"{ival(r, 'loss_ppm') / 10000:g}%"
        cur_t = fval(r, "time_s")
        cur_g = fval(r, "goodput_mbps")
        cur_ret = fval(r, "retrans_pct")
        cur_rto = ival(r, "timeouts")

        b = base_cases.get(case_key(r))
        dt = fmt_delta_f(cur_t, fval(b, "time_s"), ".2f", "s") if b else ""
        dg = fmt_delta_f(cur_g, fval(b, "goodput_mbps"), ".2f") if b else ""
        dret = fmt_delta_f(cur_ret, fval(b, "retrans_pct"), ".2f", "%") if b else ""
        drto = fmt_delta_i(cur_rto, ival(b, "timeouts")) if b else ""

        lines.append(
            f"| {r.get('scenario', 'network')} | {mib} | {env} | {loss_str} | {cc_label(r)} | {ival(r, 'seed')} "
            f"| {'PASS' if ok else 'FAIL'} | {cur_t:.2f}{dt} | {cur_g:.2f}{dg} "
            f"| {cur_ret:.2f}%{dret} | {cur_rto}{drto} "
            f"| {fval(r, 'srtt_avg_ms'):.1f} / {fval(r, 'srtt_max_ms'):.1f} |"
        )
    lines += ["", "</details>"]
    (root / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return count
