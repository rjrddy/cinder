# Cinder design

## Why the hard part is hard

A thread-safe map is not enough: an increment must read, validate, increment and write in one critical section; expiry cannot resurrect a stale value; rejected writes must leave bytes and counters coherent; snapshots must capture one state rather than a cross-shard time collage. Pointer lifetime matters as much as mutex placement. Returning an owned copy avoids exposing map storage after unlocking.

## Guarantees and lock order

`Store` owns a fixed array of shards. FNV-1a routes a full binary key to one shard; `std::unordered_map` then handles bucket collisions by comparing the full key. Shard routing collisions never imply key equality. Each shard has `std::shared_mutex`. GET and PTTL take shared locks; SET, DEL, INCR and expiry sweep take unique locks. Single-key commands are linearizable at their lookup/update while locked. NX is tested and written under the same lock. INCR preserves TTL and rejects signed overflow and malformed integers before mutation.

SAVE serializes concurrent saves with a separate mutex, then takes every shard exclusively in ascending order. No normal operation takes more than one shard; no operation takes the save mutex while holding a shard. Serialization captures only currently live values, including absolute expiration deadlines. Locks release before disk I/O. A temporary file is written, fsynced, renamed atomically, then the parent directory is fsynced. A failed directory fsync reports failure even if rename already succeeded: durability is uncertain. Only a successful SAVE is an acknowledgement of a snapshot; later commands are not durable. There is no WAL or automatic shutdown save.

Startup restore checks format, total size, checksum, lengths, quotas, duplicate live keys and trailing bytes into a staged store. It commits by swapping maps only after validation. A malformed existing snapshot prevents startup; it is never silently replaced. Restore is deliberately unavailable as a live command. `CINDER01` is a private versioned format, not an RDB file. FNV checksum detects accidental corruption, not deliberate tampering.

## Memory and expiry

A payload budget divides evenly among shards. Accounting is exact for resident key bytes plus value bytes, with a separate entry-count cap. It is **not** a bound on RSS: nodes, bucket arrays, mutexes, thread stacks, responses and snapshot staging add overhead. A hot shard can reject a write while other shards have room. The quota check precedes insertion; a rejected overwrite retains its old value. Expired entries can occupy resident accounting until swept. Reads never return them. A background sweep scans shards; this trades simplicity for periodic latency and does not return bucket capacity to the allocator.

TTL uses absolute Unix milliseconds, allowing expiry across restart. Wall-clock adjustments can shorten or lengthen TTL. This is an explicit limitation; a production revision should use steady-clock deadlines in-process and carefully translate during persistence. Tests inject time for deterministic expiry-boundary checks; socket tests use actual clocks. DBSIZE sweeps first but is an observational aggregate, not a multi-key transaction. The viewer samples shards independently and labels its counters as resident.

## Networking

The native protocol accepts RESP2 arrays of bulk strings, including fragmentation and pipelining. Length/count/frame limits are checked before materialization. Inline commands, RESP3, transactions, multiple databases and Redis data types other than strings are out of scope. Supported commands and compatibility shims are listed in docs/PROTOCOL.md. A fixed native connection pool and bounded waiting queue avoid unbounded thread creation. Idle I/O times out. Slow clients can still occupy the worker pool; this is a local engineering demonstration, not a hostile multi-tenant edge service.

The viewer is served by the same process on a separate HTTP listener. Its command endpoint accepts exactly one RESP frame and calls the same engine. It never interpolates user data as HTML. No cross-origin access is enabled. Origin checks and loopback Host checks protect default local operation against browser cross-origin requests and DNS rebinding. Non-loopback binding requires a token from the environment. Native AUTH and HTTP Bearer use that token. No TLS in the binary; a deployment needs a TLS reverse proxy. Logs contain lifecycle events and counts, not keys, values or credentials.

## What verification proves

The C++ reference-model test exercises expiry, NX, overwrite/delete and payload accounting against a separate sequential model. Parser fuzzing is deterministic input robustness, not a coverage-guided security audit. Routing-collision tests and concurrent hot-key increments exercise the shared state. The socket suite records real invocation/return intervals and a bounded DFS searches orders consistent with real-time precedence and observed results. It covers completed SET/NX/GET/DEL/INCR histories without TTL; deterministic time tests cover TTL separately. This is finite evidence, not a formal proof of every schedule. A stale-read negative control proves the checker can fail. Both sanitizer modes run the core and actual socket tests, including shutdown and snapshot recovery.

Benchmarks use the same redis-benchmark binary and workload against one server at a time. Baseline Redis is a pinned historical release, not a claim about current Redis performance. Trial ordering alternates. Pipeline and client counts are reported separately. Laptop background load and lack of CPU affinity limit inference. No claim of universal speedup is made.
