# Quality checkpoint

| Gate | Evidence / state |
|---|---|
| Storage core and explanation | src/store.cpp; docs/DESIGN.md; explicit lock/durability/clock limits |
| One-command fresh-clone run | PASS: docs/results/fresh-clone/verification.json; setup/lint/test and real seeded make demo |
| Live CLI and viewer | PASS: real native and HTTP tests plus docs/results/browser/verification.json; mobile/desktop, real expiry, safe rendering |
| Unit + integration + model/chaos | docs/results/core.log and test.log; concurrent histories, SIGKILL recovery, corruption gate |
| Benchmarks | Raw redis-benchmark CSVs and generated same-machine Redis comparison |
| Demonstrated caught failure | Quota/invalid writes retained; byte-flipped snapshot rejects startup |
| ADRs | docs/adr/ |
| Interview preparation | INTERVIEW.md and DRILL.md |
| Sanitizers | ASan + TSan core and actual network runs; macOS leak-option limitation retained |
| Dependency scan | docs/results/dependency-scan.log; native core has no third-party libraries |
| CI and container | PASS: all matrix jobs in https://github.com/rjrddy/cinder/actions/runs/39874091257; raw Linux ASan/TSan and actual image/restart HTTP smoke under docs/results/ci/ |
| Claims | README generated from raw result files; no adoption, hosted capacity or universal speedup claim |

Not deployed. The Render Free plan config is prepared only and provides no persistent disk. Local snapshot guarantees do not transfer to an ephemeral hosting filesystem. Hosted RESP compatibility, hostile traffic resistance, WAL, replication, multi-key atomicity and general Redis compatibility are out of scope. These limits are not silently replaced by claims about local tests.
