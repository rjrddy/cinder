#pragma once
#include "store.hpp"
#include <atomic>
#include <string_view>
namespace cinder {
struct Parsed {
  std::vector<std::string> args;
  size_t consumed = 0;
};
// nullopt means incomplete; malformed input throws. Arrays of bulk strings only.
std::optional<Parsed> parse(std::string_view input);
std::string bulk(const std::string &value);
std::string json_string(const std::string &value);
struct Engine {
  Store store;
  std::string snapshot;
  std::atomic<uint64_t> commands{0}, errors{0}, hits{0}, misses{0};
  explicit Engine(size_t limit, std::string path) : store(limit), snapshot(std::move(path)) {}
  std::string execute(const std::vector<std::string> &args);
  std::string state() const;
};
} // namespace cinder
