"""Run actual redis-benchmark against both servers, one server at a time."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import platform
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from client import Client  # noqa: E402
from integration import Server, free_port  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--redis-bin",
        type=Path,
        default=Path(os.getenv("REDIS_BIN", "../.cache/cinder-tools/redis-7.2.4/src")),
    )
    parser.add_argument("--requests", type=int, default=30000)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--out", type=Path, default=ROOT / "docs/results/benchmark")
    options = parser.parse_args()
    options.out.mkdir(parents=True, exist_ok=True)
    redis_bin = options.redis_bin.resolve()
    runs = []
    for trial in range(options.trials):
        # Alternate order to reduce a consistent warm-machine advantage.
        for name in ["cinder", "redis"] if trial % 2 == 0 else ["redis", "cinder"]:
            with tempfile.TemporaryDirectory(prefix="cinder-bench-") as temporary:
                path = Path(temporary)
                process, server = None, None
                log = (path / "redis.log").open("w")
                if name == "cinder":
                    server = Server(path / "state.snap", memory=64 * 1024 * 1024)
                    server.start()
                    port = server.port
                else:
                    port = free_port()
                    process = subprocess.Popen(
                        [
                            str(redis_bin / "redis-server"),
                            "--bind",
                            "127.0.0.1",
                            "--port",
                            str(port),
                            "--save",
                            "",
                            "--appendonly",
                            "no",
                            "--io-threads",
                            "1",
                        ],
                        stdout=log,
                        stderr=log,
                    )
                    for _ in range(200):
                        try:
                            with Client(port) as client:
                                assert client.call("PING") == "PONG"
                            break
                        except OSError:
                            time.sleep(0.025)
                    else:
                        raise RuntimeError("Redis readiness failed")
                try:
                    for clients, pipeline in [(1, 1), (16, 1), (50, 1), (16, 16)]:
                        base = options.out / f"{name}-t{trial}-c{clients}-p{pipeline}"
                        command = [
                            str(redis_bin / "redis-benchmark"),
                            "-h",
                            "127.0.0.1",
                            "-p",
                            str(port),
                            "-n",
                            str(options.requests),
                            "-c",
                            str(clients),
                            "-P",
                            str(pipeline),
                            "-d",
                            "128",
                            "-r",
                            "10000",
                            "-t",
                            "set,get",
                            "--csv",
                            "-e",
                        ]
                        completed = subprocess.run(
                            command, capture_output=True, text=True, timeout=90, check=True
                        )
                        base.with_suffix(".stdout").write_text(completed.stdout)
                        base.with_suffix(".stderr").write_text(completed.stderr)
                        base.with_suffix(".command.json").write_text(
                            json.dumps(command, indent=2) + "\n"
                        )
                        rows = list(csv.DictReader(io.StringIO(completed.stdout)))
                        if len(rows) != 2 or any(
                            "ERR" in completed.stdout or "Error" in completed.stderr for _ in [0]
                        ):
                            raise RuntimeError("benchmark failed or returned server errors")
                        for row in rows:
                            runs.append(
                                {
                                    "server": name,
                                    "trial": trial,
                                    "clients": clients,
                                    "pipeline": pipeline,
                                    "test": row["test"],
                                    "ops_sec": float(row["rps"]),
                                    "p99_ms": float(row["p99_latency_ms"]),
                                    "raw": base.name,
                                }
                            )
                        print(
                            name,
                            trial,
                            clients,
                            pipeline,
                            [(r["test"], r["rps"], r["p99_latency_ms"]) for r in rows],
                            flush=True,
                        )
                    if server:
                        with Client(port) as client:
                            (options.out / f"cinder-state-t{trial}.json").write_bytes(
                                client.call("INFO")
                            )
                finally:
                    if server:
                        server.close()
                    if process:
                        process.terminate()
                        process.wait(timeout=10)
                    log.close()
    summaries = []
    for name in ["cinder", "redis"]:
        for clients, pipeline in [(1, 1), (16, 1), (50, 1), (16, 16)]:
            for test in ["SET", "GET"]:
                group = [
                    r
                    for r in runs
                    if (r["server"], r["clients"], r["pipeline"], r["test"])
                    == (name, clients, pipeline, test)
                ]
                summaries.append(
                    {
                        "server": name,
                        "clients": clients,
                        "pipeline": pipeline,
                        "test": test,
                        "median_ops_sec": statistics.median(r["ops_sec"] for r in group),
                        "min_ops_sec": min(r["ops_sec"] for r in group),
                        "max_ops_sec": max(r["ops_sec"] for r in group),
                        "median_run_p99_ms": statistics.median(r["p99_ms"] for r in group),
                    }
                )
    metadata = {
        "platform": platform.platform(),
        "compiler": subprocess.check_output(["c++", "--version"], text=True),
        "cinder_cmake_cache": (ROOT / "build/CMakeCache.txt").read_text(),
        "python": sys.version,
        "redis_version": subprocess.check_output(
            [str(redis_bin / "redis-server"), "--version"], text=True
        ).strip(),
        "benchmark_sha256": hashlib.sha256(
            (redis_bin / "redis-benchmark").read_bytes()
        ).hexdigest(),
        "requests_per_test_per_trial": options.requests,
        "value_bytes": 128,
        "keyspace": 10000,
        "trials": options.trials,
        "persistence": "Cinder SAVE not invoked; Redis save empty and appendonly no",
        "limits": "loopback; no CPU pinning; shared laptop; client and server on same machine; Redis baseline pinned historical 7.2.4; benchmark random-key generator has no seed option",
    }
    if sys.platform == "darwin":
        metadata["hardware_raw"] = subprocess.check_output(
            ["sysctl", "machdep.cpu.brand_string", "hw.memsize", "hw.logicalcpu"], text=True
        )
    else:
        metadata["hardware_raw"] = (
            Path("/proc/cpuinfo").read_text() + Path("/proc/meminfo").read_text()
        )
    (options.out / "summary.json").write_text(
        json.dumps({"metadata": metadata, "summary": summaries, "runs": runs}, indent=2) + "\n"
    )


if __name__ == "__main__":
    main()
