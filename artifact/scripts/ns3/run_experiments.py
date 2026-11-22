#!/usr/bin/env python3
"""Run the native ns-3.46.1 TCP pacing evaluation matrix."""

import argparse
import hashlib
import json
import pathlib
import subprocess
import time


PROFILES = {
    "ss_iw1": {
        "cc": "reno",
        "bottleneckRate": "100Gbps",
        "delayMs": 50,
        "queuePackets": 100000,
        "maxBytes": 16 * 1024 * 1024,
        "iw": 1,
        "stopTime": 10,
    },
    "ss_iw10": {
        "cc": "reno",
        "bottleneckRate": "100Gbps",
        "delayMs": 50,
        "queuePackets": 100000,
        "maxBytes": 16 * 1024 * 1024,
        "iw": 10,
        "stopTime": 10,
    },
    "reno_loss": {
        "cc": "reno",
        "bottleneckRate": "100Mbps",
        "delayMs": 50,
        "queuePackets": 20,
        "maxBytes": 16 * 1024 * 1024,
        "iw": 10,
        "stopTime": 60,
    },
    "cubic_loss": {
        "cc": "cubic",
        "bottleneckRate": "100Mbps",
        "delayMs": 50,
        "queuePackets": 100,
        "maxBytes": 128 * 1024 * 1024,
        "iw": 10,
        "stopTime": 60,
    },
}

CORE_SOURCES = (
    "src/internet/model/tcp-congestion-ops.cc",
    "src/internet/model/tcp-congestion-ops.h",
    "src/internet/model/tcp-socket-base.cc",
    "src/internet/model/tcp-socket-base.h",
    "src/internet/model/tcp-cubic.cc",
    "src/internet/model/tcp-cubic.h",
    "src/internet/model/tcp-linux-reno.cc",
    "src/internet/model/tcp-linux-reno.h",
    "src/internet/CMakeLists.txt",
)


def sha256_file(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the native ns-3.46.1 TCP budget-pacing experiment matrix."
    )
    parser.add_argument("--data-root", required=True, help="Output directory for run traces and summaries")
    parser.add_argument("--ns3-dir", default=None, help="Path to patched ns-3.46.1 directory")
    args = parser.parse_args()

    script_dir = pathlib.Path(__file__).resolve().parent
    ns3_dir = pathlib.Path(args.ns3_dir).resolve() if args.ns3_dir else (script_dir / "ns-3.46.1")
    binary = ns3_dir / "build/scratch/ns3.46.1-budget-experiment-default"
    internet_lib = ns3_dir / "build/lib/libns3.46.1-internet-default.so"
    driver = ns3_dir / "scratch/budget-experiment.cc"

    data_root = pathlib.Path(args.data_root).resolve()
    data_root.mkdir(parents=True, exist_ok=False)

    core_hashes = "\n".join(
        f"{sha256_file(ns3_dir / rel)}  {rel}" for rel in CORE_SOURCES
    ) + "\n"

    manifest = {
        "experiment": "native ns-3 TCP, deterministic no random loss",
        "profiles": PROFILES,
        "policies": ["budget", "analytic", "linux", "static1"],
        "binary_sha256": sha256_file(binary),
        "internet_library_sha256": sha256_file(internet_lib),
        "driver_sha256": sha256_file(driver),
        "core_source_sha256": core_hashes,
        "commands": [],
    }
    (data_root / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    failed = []
    for profile, config in PROFILES.items():
        for policy in manifest["policies"]:
            case_name = f"{profile}_{policy}"
            case_dir = data_root / case_name
            case_dir.mkdir()

            cmd = [
                str(binary),
                f"--outDir={case_dir}",
                f"--policy={policy}",
                *[f"--{key}={value}" for key, value in config.items()],
            ]
            manifest["commands"].append({"case": case_name, "argv": cmd})
            (data_root / "manifest.json").write_text(
                json.dumps(manifest, indent=2), encoding="utf-8"
            )

            print(f"RUN {case_name}", flush=True)
            t0 = time.monotonic()
            with (case_dir / "run.log").open("w", encoding="utf-8") as log_file:
                try:
                    rc = subprocess.run(
                        cmd,
                        cwd=ns3_dir,
                        stdout=log_file,
                        stderr=subprocess.STDOUT,
                        timeout=180,
                    ).returncode
                except subprocess.TimeoutExpired:
                    rc = 124
            elapsed = time.monotonic() - t0

            (case_dir / "execution.json").write_text(
                json.dumps(
                    {"exit_code": rc, "wall_seconds": elapsed, "argv": cmd},
                    indent=2,
                ),
                encoding="utf-8",
            )
            summary_path = case_dir / "summary.json"
            summary = (
                json.loads(summary_path.read_text(encoding="utf-8"))
                if summary_path.exists()
                else {}
            )
            print(
                f"DONE {case_name} exit={rc} fct={summary.get('fct_s')} "
                f"drops={summary.get('queue_drops')} wall={elapsed:.2f}s",
                flush=True,
            )
            if rc != 0:
                failed.append(case_name)

    (data_root / "batch-status.json").write_text(
        json.dumps({"failed": failed, "cases": len(manifest["commands"])}, indent=2),
        encoding="utf-8",
    )
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
