# Shard locks and owned reads

Accepted. Use standard unordered_map and per-shard shared_mutex instead of lock-free reads or a custom allocator. C++ object reclamation and versioned reads would obscure the correctness argument. Return copies, never references into unlocked storage. Hot-key writers serialize; memory layout remains node-based, so locality is an explicit optimization opportunity rather than an unsupported claim. Show routing/collision statistics and payload bytes in the viewer.
