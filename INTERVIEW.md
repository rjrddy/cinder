# Cinder interview walkthrough

## Two-minute pitch

Cinder is a native in-memory string store built to make contention and durability tradeoffs visible. I implemented the storage and RESP subset in C++, then exercised it with actual redis-cli-compatible requests. Each shard owns its map and reader/writer lock; GET returns a copy, NX and INCR remain inside one exclusive critical section. A viewer shows the real shard contents, payload accounting, hash buckets and command results rather than synthetic telemetry.

The interesting work is verification. A separate reference model checks expiry and accounting; concurrent network clients record invocation/return histories, and a bounded search checks whether legal sequential orders explain the outcomes. A stale-read negative control must fail. ASan and TSan run both the core and actual server, and a corrupted snapshot must prevent startup. The exact measured comparison with Redis is in README.md and docs/BENCHMARKS.md, generated from raw redis-benchmark CSVs. It includes unfavorable cases; I do not claim to outperform Redis generally.

Snapshots capture all shards under ordered locks, release them before disk I/O, and acknowledge only after file sync, atomic rename and directory sync. That gives an inspectable durability boundary but no write-ahead log: later acknowledged writes are lost after a crash. The project is intentionally small, single-process and local. It does not provide replication, complete Redis semantics or a production security perimeter.

## Ten-minute architecture walkthrough

Start at src/server.cpp. Separate native and HTTP listeners feed bounded worker pools. The native connection reads bytes into a bounded buffer, calls the incremental parser repeatedly, batches replies and retains an incomplete suffix. Authentication is connection-local. The HTTP listener serves a fixed asset allowlist and authenticated state/command routes; the latter accepts a single RESP frame and calls the same engine.

Move to src/protocol.cpp. It accepts only arrays of bulk strings, validates lengths and terminators, distinguishes incomplete from malformed input, and implements an explicit command subset. SET options are validated before mutation. Unsupported commands are errors rather than pretend successes; CONFIG GET and CLIENT metadata are explicitly documented discovery shims. Counters are atomic and do not log payloads.

In src/store.cpp, FNV routes the binary key, then the map performs full-key comparison. A routing collision is not a correctness collision. Shared locks protect read/copy; exclusive locks protect updates and expiry. Show INCR: parse the whole integer, reject overflow, check quota, update the value and byte accounting while holding one lock. Explain why moving the read outside that lock would lose increments.

Show expiry separately. Reads treat an expired entry as absent immediately; background sweep releases its resident payload later. NX can replace expired data. The injected clock gives deterministic boundary tests. Unix time is persisted so downtime consumes TTL; clock adjustment remains a known limitation. Resident counters can include expired entries awaiting sweep and are not an atomic global view.

Walk through SAVE. Take the save mutex, acquire shard locks in increasing order, capture live records at one instant, then unlock. Serialize a version header, lengths, absolute deadlines and checksum. Write a temporary file, fsync, rename and sync the directory. Startup validates into a staging store and swaps only after validation. An invalid file fails startup instead of silently starting empty. Distinguish successful SAVE from commands performed afterward.

Finish with tests and measurement. tests/core.cpp owns explicit checks, model-based workloads, parser fuzzing, collision cases and concurrent increment/snapshot cases. tests/integration.py uses actual sockets and subprocess kills, authentication/origin gates and bounded history search. Read an actual history and negative control in docs/results/build/histories.json. Compare raw benchmark CSVs against the generated table; describe trial range, pipeline settings, shared-laptop limits and historical baseline. Show the viewer’s real response to a rejected increment. No feature requires a hosted account.

## Five tradeoffs

- **Locks versus lock-free reads:** std::shared_mutex makes ownership and reclamation straightforward. Copying values costs bandwidth, and writers on one shard serialize. I would measure before adding epochs or hazard pointers.
- **Node-based standard map versus packed storage:** standard collision behavior and ordinary lifetime rules simplify inspection. Buckets/nodes cost memory and locality. Payload accounting intentionally does not label those bytes as total memory.
- **Fixed per-shard quotas versus global admission:** admission is local and avoids a contended global accounting lock. Skew can cause early OOM. A global reservation scheme would require rollback-safe atomic bookkeeping.
- **Snapshot pause versus concurrent copy-on-write:** a consistent capture has a clear lock-order argument. Large captures pause writers and temporarily duplicate payloads. A WAL or versioned snapshot system would need crash-point tests.
- **Connection workers versus event loop:** the portable implementation is easy to debug, but slow/idle clients occupy workers. The benchmark includes that frontend cost. An event-loop revision needs the same tests and separate comparison runs.

## Five failure modes

- **Lost update under contention:** INCR holds one shard exclusively across read/validate/write. The concurrent counter test and history checker look for inconsistent outcomes; TSan looks for data races, which is a different property.
- **Expired value resurrected after restart:** absolute expiry is stored, and startup omits already expired entries. Tests allow a deadline to pass while the process is down. Clock jumps are not solved.
- **Truncated/corrupt snapshot:** length checks and checksum reject it before committing staged state. A real byte flip is retained in the caught-failure evidence. Checksums are not authentication.
- **Invalid or over-quota write:** validation precedes mutation. Tests assert the previous value remains. Allocation failure can still cause a command error; process-wide resource exhaustion is outside the payload quota guarantee.
- **Malformed or slow client:** count/length/frame caps and socket timeouts bound ordinary request handling. Queues are bounded; overload can close a connection. There is no robust hostile-traffic fairness or TLS in the native listener.

## Fifteen likely questions

1. **Where is GET linearized?** At the locked lookup/expiry decision. The return owns a copy made before the lock releases, so later writes cannot invalidate it.
2. **Why not return string_view?** A concurrent erase or rehash could destroy the referenced value after unlocking. Avoiding the copy requires an explicit ownership/reclamation mechanism.
3. **Can two different keys overwrite each other after a hash collision?** No. The hash chooses a shard/bucket; map equality compares the complete binary key. Collisions affect contention and lookup work, not identity.
4. **Is INCR equivalent to GET followed by SET?** Only if both occur under the same exclusive lock. Public commands sent separately are not an atomic transaction.
5. **What does the memory counter mean?** Resident key/value payload bytes. It excludes nodes, bucket arrays, thread stacks, request/response buffers and snapshot staging. Empty bytes and entry limits are treated separately.
6. **What happens when a shard fills?** The write is rejected without replacing the old value. Other shards can still have spare capacity. Expired payload can be reclaimed before admission.
7. **Is TTL deterministic?** Boundary tests use an injected clock. Real expiry uses wall time and is subject to scheduling/clock changes. The deadline comparison is strict: an entry at its deadline is absent.
8. **What does SAVE guarantee?** A consistent captured state is atomically installed and synced before success. It does not persist subsequent writes. The backing filesystem and hosting medium still determine actual durability.
9. **Can a failed SAVE nevertheless replace the file?** Yes, if rename succeeds but directory fsync fails. The error means durability is uncertain; the implementation does not claim rollback after that point.
10. **Why acquire every shard in the same order?** To avoid cycles between concurrent global operations. Local operations acquire one shard only, and save serialization is outside shard locking.
11. **What does TSan prove?** It found no instrumented data race in the executed workloads. It does not prove linearizability or cover every possible schedule.
12. **What does the history checker prove?** For each bounded completed history, at least one response-consistent sequential execution respects real-time precedence. It does not model TTL or pending calls, and is not a scalable proof for arbitrary traces.
13. **How do you know the checker can reject a bug?** The stale-read negative control places an incorrect read after a completed write and is rejected. An overlapping missing read is accepted when it can legally precede the write.
14. **How fair is the Redis comparison?** Same binary client, machine, value size, keyspace and persistence settings, with alternating trial order. It is not CPU-pinned, and the historical Redis release is clearly named. It measures this workload, not general product superiority.
15. **Why is the viewer useful?** It connects a real command to a real shard and shows expiry, quota accounting and rejected operations. Its cross-shard telemetry is explicitly observational; SAVE provides the consistent image.

## What I would do with three more months

First add sustained contention/skew tests and measure lock wait, allocation overhead and resident memory under larger values. Then compare an event-loop server and incremental expiry against the current baseline without changing storage semantics. Introduce a WAL only with power-loss/crash-point tests, recovery invariants and a documented acknowledgement boundary. Replace wall-clock in-process deadlines carefully, test clock translation, add a coverage-guided parser fuzzer, and expand compatibility command by command. Replication and failover need a separate consensus design, not a claim inferred from local locks. Integrate with Tidal only after both independently pass and the interface/ownership change is reviewed.
