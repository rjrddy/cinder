# Same-machine Redis comparison

Values below are generated from docs/results/benchmark/summary.json. Throughput is median ops/s; range is min–max across trials. p99 is the median of per-run p99s, not a pooled percentile.

| Server | Clients | Pipeline | Command | Median ops/s | Range | Median run p99 ms |
|---|---:|---:|---|---:|---|---:|
| cinder | 1 | 1 | SET | 45,385.78 | 42,372.88–46,728.97 | 0.055 |
| cinder | 1 | 1 | GET | 45,045.04 | 43,988.27–50,000.00 | 0.047 |
| cinder | 16 | 1 | SET | 105,633.80 | 104,895.10–107,526.88 | 0.143 |
| cinder | 16 | 1 | GET | 105,263.16 | 100,334.45–107,142.86 | 0.143 |
| cinder | 50 | 1 | SET | 106,007.07 | 90,634.44–108,303.24 | 0.335 |
| cinder | 50 | 1 | GET | 106,007.07 | 103,092.78–107,526.88 | 0.367 |
| cinder | 16 | 16 | SET | 1,363,636.38 | 1,304,347.88–1,500,000.00 | 0.359 |
| cinder | 16 | 16 | GET | 1,428,571.50 | 1,363,636.38–1,500,000.00 | 0.199 |
| redis | 1 | 1 | SET | 42,735.04 | 42,432.81–45,045.04 | 0.055 |
| redis | 1 | 1 | GET | 45,941.81 | 36,496.35–46,296.30 | 0.047 |
| redis | 16 | 1 | SET | 110,701.11 | 67,720.09–111,940.30 | 0.175 |
| redis | 16 | 1 | GET | 110,294.11 | 105,633.80–111,111.11 | 0.183 |
| redis | 50 | 1 | SET | 108,695.66 | 106,382.98–114,068.44 | 0.471 |
| redis | 50 | 1 | GET | 102,739.73 | 102,739.73–112,359.55 | 0.807 |
| redis | 16 | 16 | SET | 967,741.94 | 937,499.94–1,034,482.81 | 0.359 |
| redis | 16 | 16 | GET | 1,250,000.00 | 1,111,111.12–1,250,000.00 | 0.287 |

## Interpretation

Do not average pipelines together or infer production capacity from loopback. Read both commands, latency tails and trial ranges. Cinder has connection-bound native workers, a periodic sweeper and node-based maps; Redis has a mature event loop. Pipeline batching amortizes socket overhead but changes latency semantics. The random-key generator in this historical redis-benchmark has no seed option; synthetic workload parameters are fixed, exact key sequences are not claimed deterministic. Application tests and fuzz/model workloads do have fixed seeds. No universal speedup or allocator-efficiency claim.

## Reproduce

Download https://download.redis.io/releases/redis-7.2.4.tar.gz and build with `make -j4 MALLOC=libc BUILD_TLS=no`. Redis is only an external comparison tool and is not bundled into Cinder or needed at runtime. Preserve its own source/license when redistributing it. Run `make setup`, then `make bench REDIS_BIN=/absolute/path/to/redis-7.2.4/src`. The script starts only owned temporary servers on unused loopback ports, alternates server order by trial, disables persistence for Redis and never invokes SAVE during Cinder timing. It stops both before returning. Every invocation, stdout CSV and stderr is saved. Metadata includes executable hash, OS/hardware/compiler and CMake cache. No Redis source modification beyond upstream build configuration.

Protocol reference: https://redis.io/docs/latest/reference/protocol-spec/
Benchmark methodology reference: https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/benchmarks/
