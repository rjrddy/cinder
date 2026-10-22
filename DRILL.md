# Cinder follow-up drill

1. **If GET copies a huge value, does a writer wait?** Yes, its exclusive lock waits until the shared copy completes. Bound values and measure tail latency before changing ownership.
2. **Can shared_mutex starve writers?** Fairness is implementation dependent. No starvation bound is claimed; sustained skew and lock-wait measurements are next work.
3. **Is FNV safe for hostile keys?** No keyed-hash protection is provided. Deliberate collision/contended-shard traffic can degrade service. Default use is local synthetic data.
4. **Why are routing and bucket collisions different?** Routing chooses the shard; unordered_map's own hash chooses a bucket inside it. Full equality is always checked.
5. **Can payload accounting underflow on overwrite?** The existing entry's bytes are subtracted under the same lock and only after locating that entry. Reference-model checks compare the total after every randomized step.
6. **What if the quota check triggers a sweep?** It removes expired entries under the same exclusive shard lock, then rechecks admission. A live overwritten entry stays present until successful replacement.
7. **Does erase shrink the bucket array?** Usually not; the code does not rehash smaller. Logical payload drops without promising an RSS decrease.
8. **Can a snapshot see a key before one INCR and another after it?** Capture locks all shards at once, so it sees one cut. There is no multi-key transaction guarantee for independently issued commands.
9. **What if a snapshot temp write fails halfway?** The incomplete temp is unlinked and the prior named snapshot remains. Rename happens only after the file sync succeeds.
10. **Why sync the parent directory?** File fsync alone does not establish persistence of the renamed directory entry. The code checks both results.
11. **What if another process writes the same snapshot path?** Not supported. The in-process save mutex does not coordinate independent processes; use one owner per path.
12. **Could a malicious snapshot pass the checksum?** Yes. FNV is unkeyed and not cryptographic; the checksum is an accidental-corruption detector. Structural bounds still validate records.
13. **How are incomplete requests treated?** parse returns no result without consuming the retained suffix. Invalid lengths/terminators throw, cause an error and close that connection.
14. **Can a valid pipeline exceed the receive limit?** A single incoming retained buffer is bounded; very large aggregate pipelines may be rejected even when individual commands are valid. Tested benchmark depths stay within the bound.
15. **Does AUTH provide transport security?** No. It gates commands, but native traffic is plaintext. Keep loopback/private networking or add TLS termination deliberately.
16. **How does the viewer avoid XSS?** Server JSON escapes binary/control bytes, and client inserts row values with textContent. The real browser test writes HTML-looking values and checks no image node appears.
17. **Could cross-origin JavaScript write local state?** The API rejects foreign Origin and checks loopback Host in local mode; it emits no CORS permission. The non-loopback server requires an environment token.
18. **Are benchmark key sequences deterministic?** Not with the selected redis-benchmark release's random generator. Parameters and raw outputs are recorded; deterministic seeds apply to the model/fuzz/history workloads, not an invented Redis seed.
19. **Is a median of run p99s a pooled p99?** No. It is explicitly labelled a median of per-run tail measurements. Pooling requires underlying request samples, which these CSVs do not contain.
20. **Why stop at bounded histories?** Searching legal orders grows exponentially. Small recorded histories plus negative controls give useful finite evidence; a larger checker needs state reduction and a stronger formal model, not an exaggerated proof claim.
