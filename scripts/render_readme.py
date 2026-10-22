"""Generate measured README and comparison table exclusively from captured results."""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    data = json.loads((ROOT / "docs/results/benchmark/summary.json").read_text())
    core = json.loads((ROOT / "docs/results/core.log").read_text())
    histories = json.loads((ROOT / "docs/results/build/histories.json").read_text())

    def row(name: str, test: str) -> dict:
        return next(
            r
            for r in data["summary"]
            if r["server"] == name
            and r["test"] == test
            and r["clients"] == 16
            and r["pipeline"] == 1
        )

    cset, cget, rget = row("cinder", "SET"), row("cinder", "GET"), row("redis", "GET")
    text = f"""# Cinder

## Problem

An in-memory key-value store must keep atomic operations, expiry and snapshots correct when clients contend for the same keys.

## Results

- Cinder SET: **{cset["median_ops_sec"]:,.0f} ops/s**, median run p99 **{cset["median_run_p99_ms"]:.3f} ms**; GET: **{cget["median_ops_sec"]:,.0f} ops/s**, **{cget["median_run_p99_ms"]:.3f} ms**.
- Same-machine Redis GET: **{rget["median_ops_sec"]:,.0f} ops/s**, median run p99 **{rget["median_run_p99_ms"]:.3f} ms**. These rows use {data["metadata"]["trials"]} trials, {data["metadata"]["requests_per_test_per_trial"]:,} requests/test, 16 clients, no pipelining, {data["metadata"]["value_bytes"]}-byte values.
- **{core["checks"]:,} core checks**, including {core["model_steps"]:,} reference-model steps and {core["parser_fuzz_cases"]:,} parser fuzz cases; **{histories["passed"]} × {histories["operations_each"]}-operation** real concurrent histories accepted, stale-read negative control rejected.

Source: [raw benchmark CSVs and summary](docs/results/benchmark/), [core output](docs/results/core.log), [socket histories](docs/results/build/histories.json). Apple M2 Pro, 16 GiB RAM, macOS arm64; exact specs/compiler/config in the benchmark summary. Redis is a pinned historical 7.2.4 baseline, both built with libc/system allocators. Three trials are finite laptop measurements; neither CPU affinity nor a dedicated host. p99 values are medians of individual-run p99s, not a pooled percentile. Client and server share the machine. [Full comparison and caveats](docs/BENCHMARKS.md).

[ASan](docs/results/asan-final.log), [TSan](docs/results/tsan-final.log), [socket tests](docs/results/test.log), [dependency scan](docs/results/dependency-scan.log). macOS ASan uses leak detection disabled because LeakSanitizer is unavailable; Linux CI enables it. [CI runs](https://github.com/rjrddy/cinder/actions). [Quality report](docs/QUALITY.md).

Caught failures: malformed RESP and quota/invalid writes were rejected without changing the old value ([raw cases](docs/results/build/caught-failures.json)); a byte-flipped snapshot prevented startup ([actual fatal log](docs/results/build/corruption-rejected.log)). Reproduce with `make test`. These are successful negative tests; retained tooling/harness failures are explained in [results notes](docs/results/NOTES.md).

## Architecture

```mermaid
flowchart LR
    cli[CLI / redis-cli / redis-benchmark] --> resp[Bounded RESP connection pool]
    viewer[Live memory-map viewer] --> http[Same-origin HTTP listener]
    http --> engine[Command engine]
    resp --> engine
    engine --> shards[Sharded maps / reader-writer locks]
    sweep[Expiry sweep] --> shards
    shards --> capture[Ordered all-shard snapshot capture]
    capture --> disk[fsync / atomic rename / checksum]
```

![Real live viewer](docs/results/browser/desktop.png)

## Run it

Prerequisites: C++20 compiler, CMake, Make, Python and Node. The demo installs pinned developer tools and seeds synthetic keys. Native server and viewer run locally; Ctrl-C stops them.

```sh
make demo
# Open http://127.0.0.1:8084
python3 scripts/client.py GET synthetic:00
```

Checks: `make lint test`; sanitizer gates: `make asan` and `make tsan`. Benchmark: `make bench REDIS_BIN=/absolute/path/to/redis/src` after building the documented pinned baseline. [Benchmark reproduction](docs/BENCHMARKS.md), [ninety-second demo steps](docs/DEMO.md), [actual browser proof](docs/results/browser/verification.json).

## Decisions and limits

Per-shard shared locks and owned read copies prioritize inspectable lifetime/thread safety. Bucket collisions use full-key equality. INCR and NX are atomic under one exclusive lock. Payload accounting excludes allocator/node/stack overhead and is not an RSS limit. Quotas are per shard, so a hot shard can fill while others have spare capacity. TTL uses Unix time and is sensitive to clock adjustments. SAVE captures one state, then writes off-lock; changes after the last successful SAVE are lost on crash. Checksum detects accidental corruption, not malicious edits. Viewer totals are observational across shards. [Design and guarantees](docs/DESIGN.md), [ADRs](docs/adr/), [protocol subset](docs/PROTOCOL.md), [migration provenance](docs/migration/cinder.md).

This is a bounded local storage lab: no lock-free claim, replication, WAL, transactions, tenant isolation, RESP3 or general Redis compatibility. Native workers hold connections and can be occupied by slow clients. No hosted performance or availability has been measured. The prepared [Render free-plan config](render.yaml) is **not deployed** and has ephemeral storage; [deployment limits](docs/DEPLOY.md). No account was created and no employer code/data was used. [Interview walkthrough](INTERVIEW.md), [follow-up drill](DRILL.md).

## Next

Measure an event-loop frontend, incremental expiry and allocator-aware memory use before changing the concurrency design; then add a WAL with crash-point tests and a monotonic in-process TTL clock. Integration with Tidal waits until both projects independently pass.
"""
    (ROOT / "README.md").write_text(text)
    comparison = "# Same-machine Redis comparison\n\nValues below are generated from docs/results/benchmark/summary.json. Throughput is median ops/s; range is min–max across trials. p99 is the median of per-run p99s, not a pooled percentile.\n\n| Server | Clients | Pipeline | Command | Median ops/s | Range | Median run p99 ms |\n|---|---:|---:|---|---:|---|---:|\n"
    for r in data["summary"]:
        comparison += f"| {r['server']} | {r['clients']} | {r['pipeline']} | {r['test']} | {r['median_ops_sec']:,.2f} | {r['min_ops_sec']:,.2f}–{r['max_ops_sec']:,.2f} | {r['median_run_p99_ms']:.3f} |\n"
    comparison += """
## Interpretation

Do not average pipelines together or infer production capacity from loopback. Read both commands, latency tails and trial ranges. Cinder has connection-bound native workers, a periodic sweeper and node-based maps; Redis has a mature event loop. Pipeline batching amortizes socket overhead but changes latency semantics. The random-key generator in this historical redis-benchmark has no seed option; synthetic workload parameters are fixed, exact key sequences are not claimed deterministic. Application tests and fuzz/model workloads do have fixed seeds. No universal speedup or allocator-efficiency claim.

## Reproduce

Download https://download.redis.io/releases/redis-7.2.4.tar.gz and build with `make -j4 MALLOC=libc BUILD_TLS=no`. Redis is only an external comparison tool and is not bundled into Cinder or needed at runtime. Preserve its own source/license when redistributing it. Run `make setup`, then `make bench REDIS_BIN=/absolute/path/to/redis-7.2.4/src`. The script starts only owned temporary servers on unused loopback ports, alternates server order by trial, disables persistence for Redis and never invokes SAVE during Cinder timing. It stops both before returning. Every invocation, stdout CSV and stderr is saved. Metadata includes executable hash, OS/hardware/compiler and CMake cache. No Redis source modification beyond upstream build configuration.

Protocol reference: https://redis.io/docs/latest/reference/protocol-spec/
Benchmark methodology reference: https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/benchmarks/
"""
    (ROOT / "docs/BENCHMARKS.md").write_text(comparison)


if __name__ == "__main__":
    main()
