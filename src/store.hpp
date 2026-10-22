#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cinder {
int64_t wall_ms();
uint64_t hash_bytes(const std::string &s);
struct Entry {
  std::string value;
  int64_t expires = 0;
};
struct Shard {
  mutable std::shared_mutex mutex;
  std::unordered_map<std::string, Entry> map;
  size_t bytes = 0;
};
struct ShardStats {
  size_t keys, bytes, buckets, max_chain;
};
class Store {
public:
  static constexpr size_t shard_count = 32;
  using Clock = std::function<int64_t()>;
  explicit Store(size_t limit = 64 * 1024 * 1024, Clock clock = wall_ms);
  std::optional<std::string> get(const std::string &key) const;
  // true: stored; false: NX precondition failed; throws on quota / invalid TTL.
  bool set(const std::string &key, std::string value, int64_t ttl = 0, bool nx = false);
  bool erase(const std::string &key);
  int64_t increment(const std::string &key);
  int64_t pttl(const std::string &key) const;
  void sweep();
  std::vector<ShardStats> stats() const;
  struct Row {
    std::string key, value;
    int64_t expires;
    size_t shard;
  };
  std::vector<Row> sample(size_t limit) const;
  void save(const std::string &path);
  // Startup only; requires empty store. Invalid files never modify the store.
  void load(const std::string &path);
  size_t quota() const { return quota_; }

private:
  mutable std::array<Shard, shard_count> shards_;
  size_t quota_;
  Clock now_;
  std::mutex snapshot_mutex_;
  size_t index(const std::string &key) const;
  bool live(const Entry &entry, int64_t now) const;
  void purge(Shard &shard, int64_t now);
};
} // namespace cinder
