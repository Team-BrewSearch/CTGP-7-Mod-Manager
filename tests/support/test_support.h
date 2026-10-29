#pragma once
#include <curl/curl.h>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test {
struct Case { const char *name; void (*run)(); };
std::vector<Case> &cases();
struct Register {
  Register(const char *name, void (*run)()) { cases().push_back({name, run}); }
};
inline void check(bool condition, const char *expression, const char *file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + expression);
  }
}
#define TEST(name) static void name(); static test::Register reg_##name{#name, name}; static void name()
#define CHECK(...) test::check(bool(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

inline constexpr std::time_t NOW = 1'800'000'000;
extern std::time_t now;
extern std::vector<std::int64_t> sleeps;
extern std::function<void()> on_sleep;
extern std::vector<std::string> logs;
// Each entry forces one rename failure (errno), or delegates to the real call (0).
extern std::deque<int> rename_errors;
extern std::vector<std::pair<std::string, std::string>> renames;
extern bool fail_json_write;
struct Reply {
  Reply(CURLcode code = CURLE_OK, long http_status = 200, std::string data = {},
        std::vector<std::string> response_headers = {}, std::function<void()> callback = {})
      : outcome{code}, status{http_status}, body{std::move(data)},
        headers{std::move(response_headers)}, before_body{std::move(callback)} {}
  CURLcode outcome = CURLE_OK;
  long status = 200;
  std::string body;
  std::vector<std::string> headers;
  std::function<void()> before_body;
  curl_off_t reported_bytes = -1;
};
struct Transfer {
  std::string url;
  std::string ca;
  long verify_peer = 1, verify_host = 2;
  bool no_progress = true;
  curl_write_callback write = nullptr, header = nullptr;
  curl_xferinfo_callback progress = nullptr;
  void *body_data = nullptr, *header_data = nullptr, *progress_data = nullptr;
  curl_slist *headers = nullptr;
  long status = 0;
  curl_off_t bytes = 0;
};
struct Request {
  std::string url, ca;
  long verify_peer, verify_host;
  std::vector<std::string> headers;
};
extern std::deque<Reply> replies;
extern std::function<Reply(const Request &)> responder;
extern std::vector<Request> requests;
extern std::map<CURL *, Transfer> transfers;
void reset();
void write(std::string_view path, std::string_view contents);
std::string read(std::string_view path);
bool exists(std::string_view path);
// All sdmc:/ and romfs:/ paths resolve inside a disposable working directory.
class Sandbox {
 public:
  Sandbox();
  ~Sandbox();
  Sandbox(const Sandbox &) = delete;
  Sandbox &operator=(const Sandbox &) = delete;
 private:
  std::filesystem::path previous_, directory_;
};
}
