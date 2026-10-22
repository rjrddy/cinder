#include "store.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace cinder {
int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
uint64_t hash_bytes(const std::string &s) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}
Store::Store(size_t limit, Clock clock) : quota_(limit / shard_count), now_(std::move(clock)) {
  if (quota_ == 0)
    throw std::invalid_argument("memory budget too small");
}
size_t Store::index(const std::string &key) const { return hash_bytes(key) % shard_count; }
bool Store::live(const Entry &e, int64_t now) const { return e.expires == 0 || e.expires > now; }
void Store::purge(Shard &s, int64_t now) {
  for (auto i = s.map.begin(); i != s.map.end();) {
    if (!live(i->second, now)) {
      s.bytes -= i->first.size() + i->second.value.size();
      i = s.map.erase(i);
    } else
      ++i;
  }
}
std::optional<std::string> Store::get(const std::string &key) const {
  auto &s = shards_[index(key)];
  std::shared_lock lock(s.mutex);
  auto i = s.map.find(key);
  if (i == s.map.end() || !live(i->second, now_()))
    return std::nullopt;
  return i->second.value;
}
bool Store::set(const std::string &key, std::string value, int64_t ttl, bool nx) {
  if (key.size() > 4096 || value.size() > 1024 * 1024 || ttl < 0)
    throw std::invalid_argument("size or TTL limit");
  auto &s = shards_[index(key)];
  std::unique_lock lock(s.mutex);
  auto now = now_();
  if (ttl > std::numeric_limits<int64_t>::max() - now)
    throw std::invalid_argument("TTL overflow");
  auto i = s.map.find(key);
  if (i != s.map.end() && !live(i->second, now)) {
    s.bytes -= key.size() + i->second.value.size();
    s.map.erase(i);
    i = s.map.end();
  }
  if (nx && i != s.map.end())
    return false;
  const size_t old = i == s.map.end() ? 0 : key.size() + i->second.value.size();
  const size_t added = key.size() + value.size();
  if (i == s.map.end() && s.map.size() >= 32768) {
    purge(s, now);
    if (s.map.size() >= 32768)
      throw std::runtime_error("shard entry limit");
  }
  if (added > quota_ || s.bytes - old > quota_ - added) {
    purge(s, now);
    if (added > quota_ || s.bytes - old > quota_ - added)
      throw std::runtime_error("OOM shard payload quota exceeded");
  }
  s.map.insert_or_assign(key, Entry{std::move(value), ttl ? now + ttl : 0});
  s.bytes = s.bytes - old + added;
  return true;
}
bool Store::erase(const std::string &key) {
  auto &s = shards_[index(key)];
  std::unique_lock lock(s.mutex);
  auto i = s.map.find(key);
  if (i == s.map.end())
    return false;
  bool result = live(i->second, now_());
  s.bytes -= key.size() + i->second.value.size();
  s.map.erase(i);
  return result;
}
int64_t Store::increment(const std::string &key) {
  if (key.size() > 4096)
    throw std::invalid_argument("key size limit");
  auto &s = shards_[index(key)];
  std::unique_lock lock(s.mutex);
  auto now = now_();
  auto i = s.map.find(key);
  int64_t value = 0, expires = 0;
  if (i != s.map.end() && !live(i->second, now)) {
    s.bytes -= key.size() + i->second.value.size();
    s.map.erase(i);
    i = s.map.end();
  }
  size_t old = 0;
  if (i != s.map.end()) {
    const auto &v = i->second.value;
    auto parsed = std::from_chars(v.data(), v.data() + v.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != v.data() + v.size() || v.empty())
      throw std::invalid_argument("not an integer");
    old = key.size() + v.size();
    expires = i->second.expires;
  }
  if (value == std::numeric_limits<int64_t>::max())
    throw std::invalid_argument("integer overflow");
  if (i == s.map.end() && s.map.size() >= 32768)
    throw std::runtime_error("shard entry limit");
  auto v = std::to_string(value + 1);
  size_t added = key.size() + v.size();
  if (added > quota_ || s.bytes - old > quota_ - added)
    throw std::runtime_error("OOM shard payload quota exceeded");
  s.map.insert_or_assign(key, Entry{v, expires});
  s.bytes = s.bytes - old + added;
  return value + 1;
}
int64_t Store::pttl(const std::string &key) const {
  auto &s = shards_[index(key)];
  std::shared_lock lock(s.mutex);
  auto i = s.map.find(key);
  auto now = now_();
  if (i == s.map.end() || !live(i->second, now))
    return -2;
  return i->second.expires == 0 ? -1 : i->second.expires - now;
}
void Store::sweep() {
  for (auto &s : shards_) {
    std::unique_lock lock(s.mutex);
    purge(s, now_());
  }
}
std::vector<ShardStats> Store::stats() const {
  std::vector<ShardStats> out;
  for (auto &s : shards_) {
    std::shared_lock lock(s.mutex);
    size_t chain = 0;
    for (size_t b = 0; b < s.map.bucket_count(); ++b)
      chain = std::max(chain, s.map.bucket_size(b));
    out.push_back({s.map.size(), s.bytes, s.map.bucket_count(), chain});
  }
  return out;
}
std::vector<Store::Row> Store::sample(size_t limit) const {
  std::vector<Row> out;
  for (size_t n = 0; n < shard_count && out.size() < limit; ++n) {
    auto &s = shards_[n];
    std::shared_lock lock(s.mutex);
    for (auto &[k, e] : s.map)
      if (live(e, now_()) && out.size() < limit)
        out.push_back({k, e.value.substr(0, 128), e.expires, n});
  }
  return out;
}
static void put64(std::string &b, uint64_t n) {
  for (int i = 0; i < 8; ++i)
    b.push_back(static_cast<char>(n >> (i * 8)));
}
static uint64_t take64(const std::string &b, size_t &p) {
  if (p > b.size() || b.size() - p < 8)
    throw std::runtime_error("truncated snapshot");
  uint64_t n = 0;
  for (int i = 0; i < 8; ++i)
    n |= uint64_t(static_cast<unsigned char>(b[p++])) << (i * 8);
  return n;
}
void Store::save(const std::string &path) {
  std::lock_guard serial(snapshot_mutex_);
  std::string bytes = "CINDER01";
  {
    std::vector<std::unique_lock<std::shared_mutex>> locks;
    for (auto &s : shards_)
      locks.emplace_back(s.mutex);
    auto now = now_();
    uint64_t count = 0;
    for (auto &s : shards_)
      for (auto &[k, e] : s.map) {
        (void)k;
        if (live(e, now))
          ++count;
      }
    put64(bytes, count);
    for (auto &s : shards_)
      for (auto &[k, e] : s.map)
        if (live(e, now)) {
          put64(bytes, k.size());
          put64(bytes, e.value.size());
          put64(bytes, e.expires);
          bytes += k;
          bytes += e.value;
        }
  }
  put64(bytes, hash_bytes(bytes));
  auto temp = path + ".tmp";
  int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    throw std::runtime_error("snapshot open failed");
  size_t offset = 0;
  bool ok = true;
  while (offset < bytes.size()) {
    auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (n <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0)
    ok = false;
  if (::close(fd) != 0)
    ok = false;
  if (!ok || ::rename(temp.c_str(), path.c_str()) != 0) {
    ::unlink(temp.c_str());
    throw std::runtime_error("snapshot write/rename failed");
  }
  auto dir = std::filesystem::path(path).parent_path();
  if (dir.empty())
    dir = ".";
  fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0)
    throw std::runtime_error("snapshot directory open failed");
  int sync = ::fsync(fd);
  ::close(fd);
  if (sync != 0)
    throw std::runtime_error("snapshot directory sync failed");
}
void Store::load(const std::string &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    throw std::runtime_error("snapshot read failed");
  auto size = f.tellg();
  // Metadata is bounded independently of the payload quota.
  if (size < 24 || size > 256 * 1024 * 1024)
    throw std::runtime_error("snapshot size limit");
  std::string b(static_cast<size_t>(size), '\0');
  f.seekg(0);
  f.read(b.data(), size);
  if (!f || b.substr(0, 8) != "CINDER01")
    throw std::runtime_error("snapshot header invalid");
  size_t checksum_pos = b.size() - 8, cp = checksum_pos;
  auto expected = take64(b, cp);
  if (hash_bytes(b.substr(0, checksum_pos)) != expected)
    throw std::runtime_error("snapshot checksum mismatch");
  Store staged(quota_ * shard_count, now_);
  size_t p = 8;
  auto count = take64(b, p);
  if (count > 1000000)
    throw std::runtime_error("snapshot entry limit");
  for (uint64_t i = 0; i < count; ++i) {
    auto kl = take64(b, p), vl = take64(b, p), expiration = take64(b, p);
    if (kl > 4096 || vl > 1024 * 1024 ||
        expiration > uint64_t(std::numeric_limits<int64_t>::max()) || p > checksum_pos ||
        kl + vl > checksum_pos - p)
      throw std::runtime_error("snapshot record invalid");
    auto k = b.substr(p, kl);
    p += kl;
    auto v = b.substr(p, vl);
    p += vl;
    auto &s = staged.shards_[index(k)];
    if (s.map.contains(k))
      throw std::runtime_error("snapshot duplicate key");
    if (expiration && expiration <= uint64_t(now_()))
      continue;
    if (kl + vl > quota_ || s.bytes > quota_ - kl - vl)
      throw std::runtime_error("snapshot quota exceeded");
    s.map.emplace(k, Entry{v, static_cast<int64_t>(expiration)});
    s.bytes += kl + vl;
  }
  if (p != checksum_pos)
    throw std::runtime_error("snapshot trailing bytes");
  std::vector<std::unique_lock<std::shared_mutex>> locks;
  for (auto &s : shards_)
    locks.emplace_back(s.mutex);
  for (auto &s : shards_)
    if (!s.map.empty())
      throw std::runtime_error("restore requires empty store");
  for (size_t i = 0; i < shard_count; ++i) {
    shards_[i].map.swap(staged.shards_[i].map);
    shards_[i].bytes = staged.shards_[i].bytes;
  }
}
} // namespace cinder
