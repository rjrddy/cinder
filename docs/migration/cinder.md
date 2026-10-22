# Cinder migration

Reviewed the old Distributed KV store with failure injection at `projects/m5-distributed-kv-store-with-failure-injection` in the coordinator workspace. It used a fixed-coordinator Python cluster, not a sharded C++ engine. No old source files or dependencies are linked at runtime, and the old repository was not modified or removed.

Salvaged: the verification idea of enumerating legal orderings while honoring invocation/response precedence, and the stale-read negative control. The new bounded DFS checker is independently implemented in tests/integration.py: it tracks enabled operations, branches model state, and checks SET/NX/GET/DEL/INCR responses. Real concurrent TCP clients produce the histories, including return values. The test has an explicit size bound rather than exposing factorial enumeration as a scalable verifier.

Rewritten: all storage, collision handling, locking, expiry, RESP networking, snapshots/checksum, quota accounting, CLI, viewer, sanitizers and benchmark harness. No consensus or replicated-availability guarantees are transferred. No dependencies on the old projects directory remain in build or runtime paths. This document is provenance, not permission to delete the source repository.
