#include "protocol.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace cinder;
namespace {
std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void stop(int) { stopping.store(true, std::memory_order_relaxed); }
bool send_all(int fd, const std::string &s) {
  size_t pos = 0;
  while (pos < s.size()) {
    auto n = ::send(fd, s.data() + pos, s.size() - pos, 0);
    if (n <= 0)
      return false;
    pos += static_cast<size_t>(n);
  }
  return true;
}
int listener(int port, const std::string &bind) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    throw std::runtime_error("socket failed");
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, bind.c_str(), &a.sin_addr) != 1 ||
      ::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || ::listen(fd, 128) != 0) {
    ::close(fd);
    throw std::runtime_error("bind/listen failed");
  }
  return fd;
}
struct Pool {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<int> queue;
  bool done = false;
  std::vector<std::thread> threads;
  Pool(size_t n, std::function<void(int)> task) {
    for (size_t i = 0; i < n; ++i)
      threads.emplace_back([this, task] {
        for (;;) {
          int fd;
          {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return done || !queue.empty(); });
            if (done && queue.empty())
              return;
            fd = queue.front();
            queue.pop_front();
          }
          try {
            task(fd);
          } catch (const std::exception &) { /* client-local failure */
          }
          ::close(fd);
        }
      });
  }
  bool push(int fd) {
    std::lock_guard lock(mutex);
    if (queue.size() >= 64)
      return false;
    queue.push_back(fd);
    cv.notify_one();
    return true;
  }
  ~Pool() {
    {
      std::lock_guard lock(mutex);
      done = true;
      for (int fd : queue)
        ::close(fd);
      queue.clear();
    }
    cv.notify_all();
    for (auto &t : threads)
      t.join();
  }
};
std::string read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("asset unavailable");
  return std::string(std::istreambuf_iterator<char>(f), {});
}
void http_reply(int fd, int status, const std::string &type, const std::string &body) {
  send_all(fd, "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK\r\n" : " Error\r\n") +
                   "Connection: close\r\nContent-Type: " + type +
                   "\r\nContent-Length: " + std::to_string(body.size()) +
                   "\r\nCache-Control: no-store\r\nX-Content-Type-Options: "
                   "nosniff\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; "
                   "style-src 'self'; frame-ancestors 'none'\r\n\r\n" +
                   body);
}
std::string lower(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return s;
}
void http(int fd, Engine &engine, const std::string &web, const std::string &token, int http_port,
          bool loopback_only) {
  std::string input;
  char buffer[8192];
  size_t split;
  while ((split = input.find("\r\n\r\n")) == std::string::npos) {
    auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0)
      return;
    input.append(buffer, n);
    if (input.size() > 16384) {
      http_reply(fd, 413, "text/plain", "header too large");
      return;
    }
  }
  std::istringstream head(input.substr(0, split));
  std::string method, path, version;
  head >> method >> path >> version;
  if (version != "HTTP/1.1") {
    http_reply(fd, 400, "text/plain", "HTTP/1.1 required");
    return;
  }
  std::unordered_map<std::string, std::string> headers;
  std::string line;
  std::getline(head, line);
  while (std::getline(head, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      http_reply(fd, 400, "text/plain", "bad header");
      return;
    }
    auto name = lower(line.substr(0, colon));
    auto value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ')
      value.erase(0, 1);
    if (headers.contains(name)) {
      http_reply(fd, 400, "text/plain", "duplicate header");
      return;
    }
    headers[name] = value;
  }
  if (loopback_only && headers["host"] != "127.0.0.1:" + std::to_string(http_port) &&
      headers["host"] != "localhost:" + std::to_string(http_port)) {
    http_reply(fd, 403, "text/plain", "loopback host required");
    return;
  }
  if (headers.contains("transfer-encoding")) {
    http_reply(fd, 400, "text/plain", "chunked requests unsupported");
    return;
  }
  if (headers.contains("origin") && headers["origin"] != "http://" + headers["host"] &&
      headers["origin"] != "https://" + headers["host"]) {
    http_reply(fd, 403, "text/plain", "cross-origin request rejected");
    return;
  }
  if (method == "GET" && (path == "/" || path == "/app.js" || path == "/style.css")) {
    auto asset = path == "/" ? "index.html" : path.substr(1);
    http_reply(fd, 200,
               path == "/"         ? "text/html; charset=utf-8"
               : path == "/app.js" ? "text/javascript"
                                   : "text/css",
               read_file(web + "/" + asset));
    return;
  }
  if (!token.empty() && headers["authorization"] != "Bearer " + token) {
    http_reply(fd, 401, "text/plain", "token required");
    return;
  }
  if (method == "GET" && path == "/api/state") {
    http_reply(fd, 200, "application/json", engine.state());
    return;
  }
  if (method == "GET" && path == "/health") {
    http_reply(fd, 200, "application/json", "{\"status\":\"ok\"}");
    return;
  }
  if (method != "POST" || path != "/api/command") {
    http_reply(fd, 404, "text/plain", "not found");
    return;
  }
  size_t length = 0;
  try {
    size_t p = 0;
    const auto &s = headers.at("content-length");
    length = std::stoull(s, &p);
    if (p != s.size() || length > 2 * 1024 * 1024)
      throw std::runtime_error("size");
  } catch (...) {
    http_reply(fd, 413, "text/plain", "body size limit");
    return;
  }
  auto body = input.substr(split + 4);
  while (body.size() < length) {
    auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0)
      return;
    body.append(buffer, n);
  }
  try {
    auto parsed = parse(std::string_view(body).substr(0, length));
    if (!parsed || parsed->consumed != length)
      throw std::runtime_error("one complete frame required");
    http_reply(fd, 200, "application/json",
               "{\"response\":" + json_string(engine.execute(parsed->args)) + "}");
  } catch (const std::exception &) {
    http_reply(fd, 400, "text/plain", "invalid RESP frame");
  }
}
void resp(int fd, Engine &engine, const std::string &token) {
  std::string input;
  char buffer[16384];
  bool authed = token.empty();
  while (!stopping.load(std::memory_order_relaxed)) {
    auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0)
      return;
    input.append(buffer, n);
    if (input.size() > 2 * 1024 * 1024) {
      send_all(fd, "-ERR frame size limit\r\n");
      return;
    }
    size_t consumed = 0;
    std::string replies;
    try {
      while (consumed < input.size()) {
        auto parsed = parse(std::string_view(input).substr(consumed));
        if (!parsed)
          break;
        auto &a = parsed->args;
        if (lower(a[0]) == "auth" && a.size() == 2) {
          authed = !token.empty() && a[1] == token;
          replies += authed ? "+OK\r\n" : "-ERR invalid token\r\n";
        } else if (!authed)
          replies += "-NOAUTH token required\r\n";
        else
          replies += engine.execute(a);
        consumed += parsed->consumed;
        if (lower(a[0]) == "quit") {
          send_all(fd, replies);
          return;
        }
        if (replies.size() > 2 * 1024 * 1024) {
          if (!send_all(fd, replies))
            return;
          replies.clear();
        }
      }
    } catch (const std::exception &) {
      ++engine.errors;
      send_all(fd, replies + "-ERR malformed RESP\r\n");
      return;
    }
    if (!replies.empty() && !send_all(fd, replies))
      return;
    input.erase(0, consumed);
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    int port = 6380, http_port = 8084;
    size_t memory = 64 * 1024 * 1024;
    std::string bind = "127.0.0.1", snapshot = "cinder.snap", web = "web";
    const char *env_token = std::getenv("CINDER_TOKEN");
    std::string token = env_token ? env_token : "";
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (i + 1 == argc)
        throw std::invalid_argument("missing argument");
      std::string v = argv[++i];
      if (arg == "--port")
        port = std::stoi(v);
      else if (arg == "--http-port")
        http_port = std::stoi(v);
      else if (arg == "--bind")
        bind = v;
      else if (arg == "--snapshot")
        snapshot = v;
      else if (arg == "--web")
        web = v;
      else if (arg == "--memory")
        memory = std::stoull(v);
      else
        throw std::invalid_argument("unknown option");
    }
    if (port < 1 || port > 65535 || http_port < 1 || http_port > 65535 || port == http_port ||
        memory < 1024 || memory > 64 * 1024 * 1024)
      throw std::invalid_argument("configuration limit");
    if (bind != "127.0.0.1" && token.size() < 24)
      throw std::invalid_argument("non-loopback bind requires CINDER_TOKEN of at least 24 bytes");
    Engine engine(memory, snapshot);
    if (std::filesystem::exists(snapshot))
      engine.store.load(snapshot);
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, stop);
    ::signal(SIGTERM, stop);
    int tcp = listener(port, bind), web_fd;
    try {
      web_fd = listener(http_port, bind);
    } catch (...) {
      ::close(tcp);
      throw;
    }
    std::atomic<bool> cleanup_stop{false};
    std::thread cleanup([&] {
      while (!cleanup_stop) {
        engine.store.sweep();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
    {
      Pool tcp_pool(64, [&](int fd) { resp(fd, engine, token); });
      Pool web_pool(4,
                    [&](int fd) { http(fd, engine, web, token, http_port, bind == "127.0.0.1"); });
      std::cout << "{\"event\":\"ready\",\"resp_port\":" << port << ",\"http_port\":" << http_port
                << "}" << std::endl;
      while (!stopping.load(std::memory_order_relaxed)) {
        pollfd fds[2] = {{tcp, POLLIN, 0}, {web_fd, POLLIN, 0}};
        if (::poll(fds, 2, 200) <= 0)
          continue;
        for (int i = 0; i < 2; ++i)
          if (fds[i].revents & POLLIN) {
            int fd = ::accept(fds[i].fd, nullptr, nullptr);
            if (fd < 0)
              continue;
            timeval timeout{2, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            if (!(i == 0 ? tcp_pool : web_pool).push(fd)) {
              ::close(fd);
            }
          }
      }
    }
    cleanup_stop = true;
    cleanup.join();
    ::close(tcp);
    ::close(web_fd);
    std::cout << "{\"event\":\"stopped\",\"commands\":" << engine.commands << "}" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "{\"event\":\"fatal\",\"message\":" << json_string(e.what()) << "}" << std::endl;
    return 1;
  }
}
