#include "protocol.hpp"
#include <charconv>
#include <limits>
#include <stdexcept>

namespace cinder {
static int64_t number(std::string_view s) {
  int64_t n = 0;
  auto r = std::from_chars(s.data(), s.data() + s.size(), n);
  if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size())
    throw std::invalid_argument("invalid integer");
  return n;
}
std::optional<Parsed> parse(std::string_view b) {
  if (b.empty())
    return std::nullopt;
  if (b[0] != '*')
    throw std::invalid_argument("expected RESP array");
  size_t p = 1;
  auto line = [&]() -> std::optional<std::string_view> {
    auto end = b.find("\r\n", p);
    if (end == std::string_view::npos) {
      if (b.size() - p > 32)
        throw std::invalid_argument("header too long");
      return std::nullopt;
    }
    if (end - p > 32)
      throw std::invalid_argument("header too long");
    auto result = b.substr(p, end - p);
    p = end + 2;
    return result;
  };
  auto count_line = line();
  if (!count_line)
    return std::nullopt;
  auto count = number(*count_line);
  if (count < 1 || count > 16)
    throw std::invalid_argument("argument count limit");
  Parsed result;
  for (int64_t i = 0; i < count; ++i) {
    if (p == b.size())
      return std::nullopt;
    if (b[p++] != '$')
      throw std::invalid_argument("expected bulk string");
    auto length_line = line();
    if (!length_line)
      return std::nullopt;
    auto len = number(*length_line);
    if (len < 0 || len > 1024 * 1024)
      throw std::invalid_argument("bulk size limit");
    auto size = static_cast<size_t>(len);
    if (b.size() - p < size + 2)
      return std::nullopt;
    if (b.substr(p + size, 2) != "\r\n")
      throw std::invalid_argument("bulk terminator invalid");
    result.args.emplace_back(b.substr(p, size));
    p += size + 2;
    if (p > 2 * 1024 * 1024)
      throw std::invalid_argument("frame size limit");
  }
  result.consumed = p;
  return result;
}
std::string bulk(const std::string &v) {
  return "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n";
}
std::string json_string(const std::string &v) {
  const char *hex = "0123456789abcdef";
  std::string out = "\"";
  // Encode all non-ASCII bytes as \u00xx: binary safe, never raw HTML/control data.
  for (unsigned char c : v) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c < 32 || c >= 127) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += c;
  }
  return out + '"';
}
static std::string upper(std::string s) {
  for (char &c : s)
    if (c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
  return s;
}
std::string Engine::execute(const std::vector<std::string> &a) {
  ++commands;
  try {
    if (a.empty())
      throw std::invalid_argument("empty command");
    auto cmd = upper(a[0]);
    auto n = a.size();
    if (cmd == "PING" && n <= 2)
      return n == 1 ? "+PONG\r\n" : bulk(a[1]);
    if (cmd == "ECHO" && n == 2)
      return bulk(a[1]);
    if (cmd == "GET" && n == 2) {
      auto value = store.get(a[1]);
      if (value) {
        ++hits;
        return bulk(*value);
      }
      ++misses;
      return "$-1\r\n";
    }
    if (cmd == "SET" && n >= 3) {
      int64_t ttl = 0;
      bool nx = false;
      for (size_t i = 3; i < n; ++i) {
        auto option = upper(a[i]);
        if (option == "NX" && !nx)
          nx = true;
        else if (option == "PX" && !ttl && i + 1 < n) {
          ttl = number(a[++i]);
          if (ttl <= 0)
            throw std::invalid_argument("PX must be positive");
        } else
          throw std::invalid_argument("unsupported SET option");
      }
      return store.set(a[1], a[2], ttl, nx) ? "+OK\r\n" : "$-1\r\n";
    }
    if (cmd == "DEL" && n == 2)
      return ":" + std::to_string(store.erase(a[1])) + "\r\n";
    if (cmd == "EXISTS" && n == 2)
      return ":" + std::to_string(store.get(a[1]).has_value()) + "\r\n";
    if (cmd == "INCR" && n == 2)
      return ":" + std::to_string(store.increment(a[1])) + "\r\n";
    if (cmd == "PTTL" && n == 2)
      return ":" + std::to_string(store.pttl(a[1])) + "\r\n";
    if (cmd == "SAVE" && n == 1) {
      store.save(snapshot);
      return "+OK\r\n";
    }
    if (cmd == "DBSIZE" && n == 1) {
      store.sweep();
      size_t keys = 0;
      for (auto s : store.stats())
        keys += s.keys;
      return ":" + std::to_string(keys) + "\r\n";
    }
    if (cmd == "INFO" && n <= 2)
      return bulk("# Server\r\ncinder_version:0.1.0\r\n" + state());
    if (cmd == "CONFIG" && n == 3 && upper(a[1]) == "GET")
      return "*0\r\n";
    if (cmd == "CLIENT" && n >= 2 &&
        ((upper(a[1]) == "SETINFO" && n == 4) || (upper(a[1]) == "SETNAME" && n == 3)))
      return "+OK\r\n";
    if (cmd == "QUIT" && n == 1)
      return "+OK\r\n";
    throw std::invalid_argument("unsupported command or wrong arity");
  } catch (const std::exception &e) {
    ++errors;
    return "-ERR " + std::string(e.what()) + "\r\n";
  }
}
std::string Engine::state() const {
  auto shards = store.stats();
  size_t keys = 0, bytes = 0;
  std::string out = "{\"shards\":[";
  for (size_t i = 0; i < shards.size(); ++i) {
    auto s = shards[i];
    keys += s.keys;
    bytes += s.bytes;
    if (i)
      out += ',';
    out += "{\"id\":" + std::to_string(i) + ",\"keys\":" + std::to_string(s.keys) +
           ",\"payload_bytes\":" + std::to_string(s.bytes) +
           ",\"buckets\":" + std::to_string(s.buckets) +
           ",\"max_chain\":" + std::to_string(s.max_chain) + "}";
  }
  out += "],\"resident_keys\":" + std::to_string(keys) +
         ",\"payload_bytes\":" + std::to_string(bytes) +
         ",\"shard_quota\":" + std::to_string(store.quota()) +
         ",\"commands\":" + std::to_string(commands.load()) +
         ",\"errors\":" + std::to_string(errors.load()) +
         ",\"hits\":" + std::to_string(hits.load()) +
         ",\"misses\":" + std::to_string(misses.load()) + ",\"sample\":[";
  bool first = true;
  for (auto &r : store.sample(80)) {
    if (!first)
      out += ',';
    first = false;
    out += "{\"key\":" + json_string(r.key) + ",\"preview\":" + json_string(r.value) +
           ",\"expires_unix_ms\":" + std::to_string(r.expires) +
           ",\"shard\":" + std::to_string(r.shard) + "}";
  }
  return out + "]}";
}
} // namespace cinder
