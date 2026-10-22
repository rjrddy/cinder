#include "protocol.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
using namespace cinder;
static size_t checks = 0;
void check(bool ok, const char *message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
template <typename F> void rejects(F f, const char *message) {
  bool caught = false;
  try {
    f();
  } catch (const std::exception &) {
    caught = true;
  }
  check(caught, message);
}
int main() {
  try {
    int64_t now = 1000;
    Store s(1024 * 1024, [&] { return now; });
    check(!s.get("missing"), "absent");
    check(s.set("a", "one", 10), "set");
    check(!s.set("a", "two", 0, true), "NX");
    check(s.get("a") == "one", "GET");
    check(s.pttl("a") == 10, "PTTL");
    now += 10;
    check(!s.get("a"), "expiry boundary");
    check(s.pttl("a") == -2, "expired PTTL");
    check(!s.erase("a"), "expired DEL");
    check(s.increment("count") == 1, "new increment");
    s.set("max", "9223372036854775807");
    rejects([&] { s.increment("max"); }, "overflow rejected");
    check(s.get("max") == "9223372036854775807", "overflow no mutation");
    s.set("bad", "2x");
    rejects([&] { s.increment("bad"); }, "non integer");
    s.set("ttl", "4", 20);
    s.increment("ttl");
    check(s.pttl("ttl") == 20, "increment preserves TTL");
    rejects([&] { s.set("huge", std::string(40000, 'x')); }, "quota enforced");
    rejects([&] { s.set("a", "v", -1); }, "negative TTL");
    check(!s.get("huge"), "rejection atomic");
    std::string binary("\0\r\n\xff", 4);
    s.set(binary, binary);
    check(s.get(binary) == binary, "binary safety");
    auto path = (std::filesystem::temp_directory_path() /
                 ("cinder-core-" + std::to_string(wall_ms()) + ".snap"))
                    .string();
    s.save(path);
    Store restored(1024 * 1024, [&] { return now; });
    restored.load(path);
    check(restored.get(binary) == binary, "snapshot binary roundtrip");
    now += 21;
    check(!restored.get("ttl"), "restored TTL");
    rejects([&] { restored.load(path); }, "live restore forbidden");
    {
      std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(20);
      f.put('X');
    }
    Store empty;
    rejects([&] { empty.load(path); }, "corruption caught");
    check(!empty.get("count"), "failed restore atomic");
    std::filesystem::remove(path);
    auto wire = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4\r\n" + binary + "\r\n";
    for (size_t i = 0; i < wire.size(); ++i)
      check(!parse(std::string_view(wire).substr(0, i)), "fragment incomplete");
    auto p = parse(wire + wire);
    check(p && p->consumed == wire.size() && p->args[2] == binary, "pipeline parse");
    rejects([] { parse("*999\r\n"); }, "count bound");
    rejects([] { parse("*1\r\n$999999999\r\n"); }, "bulk bound");
    rejects([] { parse("*1\r\n$1\r\nx!!"); }, "terminator");
    Engine e(1024 * 1024, path);
    check(e.execute({"SET", "k", "v", "PX", "0"})[0] == '-', "invalid PX");
    check(e.execute({"SET", "k", "v"}) == "+OK\r\n", "execute SET");
    check(e.execute({"GET", "k"}) == bulk("v"), "execute GET");
    // Deterministic reference-model property: expiry, overwrite, NX, delete and quota accounting.
    Store model_store(1024 * 1024, [&] { return now; });
    std::unordered_map<std::string, Entry> model;
    std::mt19937 rng(84017);
    for (int step = 0; step < 4000; ++step) {
      now += rng() % 3;
      auto key = "model:" + std::to_string(rng() % 40);
      for (auto it = model.begin(); it != model.end();) {
        if (it->second.expires && it->second.expires <= now)
          it = model.erase(it);
        else
          ++it;
      }
      int kind = rng() % 4;
      if (kind == 0 || kind == 1) {
        auto value = std::to_string(rng());
        int64_t ttl = rng() % 15;
        bool expected = kind == 0 || !model.contains(key);
        check(model_store.set(key, value, ttl, kind == 1) == expected, "model SET/NX");
        if (expected)
          model[key] = {value, ttl ? now + ttl : 0};
      } else if (kind == 2)
        check(model_store.erase(key) == bool(model.erase(key)), "model DEL");
      else
        check(
            model_store.get(key) ==
                (model.contains(key) ? std::optional<std::string>(model[key].value) : std::nullopt),
            "model GET");
      model_store.sweep();
      size_t expected_bytes = 0, actual_bytes = 0;
      for (auto &[k, entry] : model)
        expected_bytes += k.size() + entry.value.size();
      for (auto stats : model_store.stats())
        actual_bytes += stats.bytes;
      check(actual_bytes == expected_bytes, "exact payload accounting");
    }
    for (int i = 0; i < 10000; ++i) {
      std::string fuzz(rng() % 100, '\0');
      for (auto &c : fuzz)
        c = static_cast<char>(rng());
      try {
        parse(fuzz);
      } catch (const std::invalid_argument &) {
      }
    }
    rejects([&] { s.save("/nonexistent-cinder-directory/state.snap"); },
            "snapshot I/O failure surfaced");
    check(s.get("count") == "1", "failed save leaves live state intact");
    Store concurrent;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
      threads.emplace_back([&, t] {
        for (int i = 0; i < 2000; ++i) {
          concurrent.increment("shared");
          concurrent.set("worker" + std::to_string(t), std::to_string(i));
          concurrent.get("shared");
          if (i % 250 == 0)
            concurrent.sweep();
        }
      });
    for (auto &t : threads)
      t.join();
    check(concurrent.get("shared") == "16000", "no lost increments");
    // Generate keys mapped to one shard: routing collisions must not alias values.
    std::vector<std::string> collisions;
    for (int i = 0; collisions.size() < 100; ++i) {
      auto key = "collision:" + std::to_string(i);
      if (hash_bytes(key) % Store::shard_count == 0)
        collisions.push_back(key);
    }
    for (auto &k : collisions)
      concurrent.set(k, k);
    for (auto &k : collisions)
      check(concurrent.get(k) == k, "collision isolation");
    std::thread writer([&] {
      for (int i = 0; i < 500; ++i)
        concurrent.increment("shared");
    });
    concurrent.save(path);
    writer.join();
    Store snap;
    snap.load(path);
    auto captured = std::stoll(*snap.get("shared"));
    check(captured >= 16000 && captured <= 16500, "snapshot consistent concurrent state");
    std::filesystem::remove(path);
    std::cout << "{\"checks\":" << checks
              << ",\"concurrent_increments\":16000,\"collision_keys\":100,\"model_steps\":4000,"
                 "\"parser_fuzz_cases\":10000,\"status\":\"pass\"}\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
