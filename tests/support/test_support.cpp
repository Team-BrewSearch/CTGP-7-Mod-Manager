#include "test_support.h"
#include <3ds.h>
#include <json-c/json.h>
#include <cerrno>
#include <cstdarg>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>

namespace test {
std::vector<Case> &cases() { static std::vector<Case> value; return value; }
std::time_t now = NOW;
std::vector<std::int64_t> sleeps;
std::function<void()> on_sleep;
std::vector<std::string> logs;
std::deque<int> rename_errors;
std::vector<std::pair<std::string, std::string>> renames;
bool fail_json_write = false;
std::deque<Reply> replies;
std::function<Reply(const Request &)> responder;
std::vector<Request> requests;
std::map<CURL *, Transfer> transfers;
void reset() {
  now = NOW;
  sleeps.clear();
  on_sleep = {};
  logs.clear();
  rename_errors.clear();
  renames.clear();
  fail_json_write = false;
  replies.clear();
  responder = {};
  requests.clear();
  transfers.clear();
}
void write(std::string_view path, std::string_view contents) {
  const std::filesystem::path target{path};
  if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
  std::ofstream stream{target, std::ios::binary};
  stream.write(contents.data(), std::streamsize(contents.size()));
  stream.close();
  CHECK(!stream.fail());
}
std::string read(std::string_view path) {
  std::ifstream stream{std::string(path), std::ios::binary};
  CHECK(stream.good());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
bool exists(std::string_view path) { return std::filesystem::exists(std::filesystem::path{path}); }
Sandbox::Sandbox() : previous_{std::filesystem::current_path()} {
  char pattern[] = "/tmp/ctgp7-tests-XXXXXX";
  char *created = ::mkdtemp(pattern);
  CHECK(created != nullptr);
  directory_ = created;
  std::filesystem::current_path(directory_);
}
Sandbox::~Sandbox() {
  std::filesystem::current_path(previous_);
  std::filesystem::remove_all(directory_);
}
}
void svcSleepThread(s64 duration) {
  test::sleeps.push_back(duration);
  if (test::on_sleep) test::on_sleep();
}
namespace mm {
#ifndef TEST_REAL_LOG
void log_event(const std::string &text) { test::logs.push_back(text); }
#endif
}
extern "C" std::time_t __wrap_time(std::time_t *out) {
  if (out) *out = test::now;
  return test::now;
}
extern "C" int __real_rename(const char *, const char *);
extern "C" int __wrap_rename(const char *source, const char *destination) {
  test::renames.emplace_back(source, destination);
  if (!test::rename_errors.empty()) {
    const int error = test::rename_errors.front();
    test::rename_errors.pop_front();
    if (error != 0) { errno = error; return -1; }
  }
  return __real_rename(source, destination);
}
extern "C" int __real_json_object_to_file_ext(const char *, json_object *, int);
extern "C" int __wrap_json_object_to_file_ext(const char *path, json_object *root, int flags) {
  if (test::fail_json_write) {
    test::write(path, "partial");
    return -1;
  }
  return __real_json_object_to_file_ext(path, root, flags);
}
// Linker wrappers retain real CURL handles and replace every network transfer.
// A missing scripted reply fails immediately, so tests never reach the Internet.
extern "C" CURLcode __wrap_curl_easy_setopt(CURL *curl, CURLoption option, ...) {
  auto &transfer = test::transfers[curl];
  va_list args;
  va_start(args, option);
  switch (option) {
    case CURLOPT_URL: transfer.url = va_arg(args, const char *); break;
    case CURLOPT_CAINFO: transfer.ca = va_arg(args, const char *); break;
    case CURLOPT_SSL_VERIFYPEER: transfer.verify_peer = va_arg(args, long); break;
    case CURLOPT_SSL_VERIFYHOST: transfer.verify_host = va_arg(args, long); break;
    case CURLOPT_NOPROGRESS: transfer.no_progress = va_arg(args, long) != 0; break;
    case CURLOPT_WRITEFUNCTION: transfer.write = va_arg(args, curl_write_callback); break;
    case CURLOPT_HEADERFUNCTION: transfer.header = va_arg(args, curl_write_callback); break;
    case CURLOPT_XFERINFOFUNCTION: transfer.progress = va_arg(args, curl_xferinfo_callback); break;
    case CURLOPT_WRITEDATA: transfer.body_data = va_arg(args, void *); break;
    case CURLOPT_HEADERDATA: transfer.header_data = va_arg(args, void *); break;
    case CURLOPT_XFERINFODATA: transfer.progress_data = va_arg(args, void *); break;
    case CURLOPT_HTTPHEADER: transfer.headers = va_arg(args, curl_slist *); break;
    default: break;
  }
  va_end(args);
  return CURLE_OK;
}
extern "C" CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO info, ...) {
  va_list args;
  va_start(args, info);
  if (info == CURLINFO_RESPONSE_CODE) {
    *va_arg(args, long *) = test::transfers.at(curl).status;
  } else if (info == CURLINFO_SIZE_DOWNLOAD_T) {
    *va_arg(args, curl_off_t *) = test::transfers.at(curl).bytes;
  } else {
    va_end(args);
    return CURLE_UNKNOWN_OPTION;
  }
  va_end(args);
  return CURLE_OK;
}
extern "C" CURLcode __wrap_curl_easy_perform(CURL *curl) {
  auto &transfer = test::transfers.at(curl);
  test::Request request{transfer.url, transfer.ca, transfer.verify_peer, transfer.verify_host, {}};
  for (curl_slist *header = transfer.headers; header; header = header->next) {
    request.headers.emplace_back(header->data);
  }
  test::requests.push_back(std::move(request));
  CHECK(!test::replies.empty() || bool(test::responder));
  test::Reply reply;
  if (!test::replies.empty()) {
    reply = std::move(test::replies.front());
    test::replies.pop_front();
  } else {
    reply = test::responder(test::requests.back());
  }
  if (reply.before_body) reply.before_body();
  if (!transfer.no_progress && transfer.progress &&
      transfer.progress(transfer.progress_data, reply.body.size(), 0, 0, 0) != 0) {
    return CURLE_ABORTED_BY_CALLBACK;
  }
  transfer.status = reply.status;
  transfer.bytes = reply.reported_bytes < 0 ? curl_off_t(reply.body.size()) : reply.reported_bytes;
  if (reply.outcome != CURLE_OK) return reply.outcome;
  for (std::string &header : reply.headers) {
    if (transfer.header && transfer.header(header.data(), 1, header.size(), transfer.header_data) != header.size()) {
      return CURLE_WRITE_ERROR;
    }
  }
  if (!reply.body.empty()) {
    CHECK(transfer.write != nullptr);
    if (transfer.write(reply.body.data(), 1, reply.body.size(), transfer.body_data) != reply.body.size()) {
      return CURLE_WRITE_ERROR;
    }
  }
  return CURLE_OK;
}
extern "C" void __real_curl_easy_cleanup(CURL *);
extern "C" void __wrap_curl_easy_cleanup(CURL *curl) {
  test::transfers.erase(curl);
  __real_curl_easy_cleanup(curl);
}
int main() {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
  int failed = 0;
  for (const auto &entry : test::cases()) {
    test::reset();
    try { entry.run(); std::cout << "PASS " << entry.name << '\n'; }
    catch (const std::exception &error) {
      ++failed;
      std::cerr << "FAIL " << entry.name << ": " << error.what() << '\n';
    }
  }
  curl_global_cleanup();
  return failed == 0 ? 0 : 1;
}
