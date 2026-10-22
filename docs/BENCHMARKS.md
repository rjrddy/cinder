# Same-machine Redis comparison

Values below are generated from docs/results/benchmark/summary.json. Throughput is median ops/s; range is min–max across trials. p99 is the median of per-run p99s, not a pooled percentile.

| Server | Clients | Pipeline | Command | Median ops/s | Range | Median run p99 ms |
|---|---:|---:|---|---:|---|---:|
| cinder | 1 | 1 | SET | 48,000.00 | 39,318.48–48,076.92 | 0.039 |
| cinder | 1 | 1 | GET | 48,076.92 | 46,875.00–48,465.27 | 0.039 |
| cinder | 16 | 1 | SET | 107,526.88 | 105,263.16–108,695.66 | 0.135 |
| cinder | 16 | 1 | GET | 106,761.57 | 105,263.16–107,142.86 | 0.151 |
| cinder | 50 | 1 | SET | 107,142.86 | 106,007.07–108,695.66 | 0.343 |
| cinder | 50 | 1 | GET | 106,382.98 | 91,463.41–107,913.67 | 0.359 |
| cinder | 16 | 16 | SET | 1,363,636.38 | 1,304,347.88–1,428,571.50 | 0.351 |
| cinder | 16 | 16 | GET | 1,428,571.50 | 1,363,636.38–1,428,571.50 | 0.183 |
| redis | 1 | 1 | SET | 44,977.51 | 42,613.64–45,731.71 | 0.047 |
| redis | 1 | 1 | GET | 46,948.36 | 43,668.12–47,468.35 | 0.039 |
| redis | 16 | 1 | SET | 109,090.91 | 87,976.54–112,781.95 | 0.167 |
| redis | 16 | 1 | GET | 111,940.30 | 110,294.11–113,636.37 | 0.183 |
| redis | 50 | 1 | SET | 114,068.44 | 112,359.55–114,942.52 | 0.447 |
| redis | 50 | 1 | GET | 113,207.55 | 108,303.24–115,384.62 | 0.399 |
| redis | 16 | 16 | SET | 1,034,482.81 | 967,741.94–1,034,482.81 | 0.359 |
| redis | 16 | 16 | GET | 1,200,000.00 | 1,200,000.00–1,250,000.00 | 0.287 |

## Interpretation

Do not average pipelines together or infer production capacity from loopback. Read both commands, latency tails and trial ranges. Cinder has connection-bound native workers, a periodic sweeper and node-based maps; Redis has a mature event loop. Pipeline batching amortizes socket overhead but changes latency semantics. The random-key generator in this historical redis-benchmark has no seed option; synthetic workload parameters are fixed, exact key sequences are not claimed deterministic. Application tests and fuzz/model workloads do have fixed seeds. No universal speedup or allocator-efficiency claim.

## Reproduce

Download https://download.redis.io/releases/redis-7.2.4.tar.gz and build with `make -j4 MALLOC=libc BUILD_TLS=no`. Redis is only an external comparison tool and is not bundled into Cinder or needed at runtime. Preserve its own source/license when redistributing it. Run `make setup`, then `make bench REDIS_BIN=/absolute/path/to/redis-7.2.4/src`. The script starts only owned temporary servers on unused loopback ports, alternates server order by trial, disables persistence for Redis and never invokes SAVE during Cinder timing. It stops both before returning. Every invocation, stdout CSV and stderr is saved. Metadata includes executable hash, OS/hardware/compiler and CMake cache. No Redis source modification beyond upstream build configuration.

Protocol reference: https://redis.io/docs/latest/reference/protocol-spec/
Benchmark methodology reference: https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/benchmarks/
