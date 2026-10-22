# Blocking capture, atomic replacement and wall-clock TTL

Accepted. Stop all writers briefly to capture one cross-shard image, release locks, then fsync/rename/fsync. Restore only on startup into a staged store. Avoid online replacement races and unmeasured WAL durability. Persist absolute Unix expiration deadlines so downtime counts toward TTL. Record clock-jump sensitivity and the gap after the last SAVE. FNV checksum is for accidental corruption only. No Redis snapshot compatibility or tamper-evidence claim.
