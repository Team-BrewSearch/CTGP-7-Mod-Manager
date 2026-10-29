#include "backend/net.h"
#include <3ds.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mbedtls/sha256.h>
#include <unistd.h>
#include "backend/sd_card.h"
#include "core/log.h"
#include "core/status.h"
namespace mm {
namespace net {
std::atomic<int> failed_requests{0};
std::array<sys::Mutex, CURL_LOCK_DATA_LAST> share_locks;
sys::CurlShare share;
void share_lock_cb(CURL *, curl_lock_data data, curl_lock_access, void *) {
  if (data < CURL_LOCK_DATA_LAST) {
    share_locks[static_cast<std::size_t>(data)].lock();
  }
}
void share_unlock_cb(CURL *, curl_lock_data data, void *) {
  if (data < CURL_LOCK_DATA_LAST) {
    share_locks[static_cast<std::size_t>(data)].unlock();
  }
}
void share_init() {
  (void)share.init(&share_lock_cb, &share_unlock_cb);
}
void share_cleanup() {
  share.reset();
}
std::size_t write_to_string(void *contents, std::size_t size, std::size_t nmemb, void *userp) {
  if (size == 0 || nmemb == 0) {
    return 0;
  }
  if (size > std::numeric_limits<std::size_t>::max() / nmemb) {
    return CURL_WRITEFUNC_ERROR;
  }
  const std::size_t total = size * nmemb;
  auto *out = static_cast<std::string *>(userp);
  if (out->size() + total > cfg::MAX_RESPONSE_SIZE) {
    return CURL_WRITEFUNC_ERROR;
  }
  out->append(static_cast<const char *>(contents), total);
  return total;
}
namespace {
std::atomic<const char *> ca_bundle_path{nullptr};
constexpr std::string_view CERT_BEGIN = "-----BEGIN CERTIFICATE-----";
constexpr std::string_view CERT_END = "-----END CERTIFICATE-----";
bool plausible_ca_bundle(const std::string &data) noexcept {
  if (data.size() < cfg::CA_BUNDLE_MIN_SIZE || data.size() > cfg::CA_BUNDLE_MAX_SIZE) {
    return false;
  }
  if (data.find(CERT_BEGIN) == std::string::npos || data.find(CERT_END) == std::string::npos) {
    return false;
  }
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = data.find(CERT_BEGIN, position)) != std::string::npos) {
    ++count;
    position += CERT_BEGIN.size();
  }
  return count >= 10;
}
bool compute_sha256(const std::string &data, unsigned char hash[32]) noexcept {
  return mbedtls_sha256_ret(
             reinterpret_cast<const unsigned char *>(data.data()),
             static_cast<std::size_t>(data.size()), hash, 0) == 0;
}
bool parse_sha256_hex(const std::string &hex, unsigned char expected[32]) noexcept {
  if (hex.size() != 64) {
    return false;
  }
  const auto parse_nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  for (std::size_t i = 0; i < 32; ++i) {
    const int hi = parse_nibble(hex[i * 2]);
    const int lo = parse_nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    expected[i] = static_cast<unsigned char>(hi * 16 + lo);
  }
  return true;
}
bool verify_ca_bundle_hash(const std::string &data, const std::string &expected_hex) noexcept {
  unsigned char hash[32];
  if (!compute_sha256(data, hash)) {
    return false;
  }
  unsigned char expected[32];
  if (!parse_sha256_hex(expected_hex, expected)) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < sizeof(hash); ++i) {
    diff |= static_cast<unsigned char>(hash[i] ^ expected[i]);
  }
  return diff == 0;
}
bool plausible_ca_file(const char *path) noexcept {
  const std::optional<int64_t> size = sd::file_size(path);
  if (!size || *size < static_cast<int64_t>(cfg::CA_BUNDLE_MIN_SIZE) ||
      *size > static_cast<int64_t>(cfg::CA_BUNDLE_MAX_SIZE)) {
    return false;
  }
  sys::FileHandle file = sd::open(path, "rb");
  if (!file) {
    return false;
  }
  std::array<char, 4096> head{};
  const std::size_t head_read = std::fread(head.data(), 1, head.size(), file.get());
  if (head_read == 0) {
    return false;
  }
  const std::string_view head_text{head.data(), head_read};
  if (std::search(head_text.begin(), head_text.end(), CERT_BEGIN.begin(), CERT_BEGIN.end()) == head_text.end()) {
    return false;
  }
  if (*size <= static_cast<int64_t>(head_read)) {
    return std::search(head_text.begin(), head_text.end(), CERT_END.begin(), CERT_END.end()) != head_text.end();
  }
  const std::size_t tail_start = static_cast<std::size_t>(*size) - head_read;
  if (std::fseek(file.get(), static_cast<long>(tail_start), SEEK_SET) != 0) {
    return false;
  }
  std::array<char, 4096> tail{};
  const std::size_t tail_read = std::fread(tail.data(), 1, tail.size(), file.get());
  const std::string_view tail_text{tail.data(), tail_read};
  return std::search(tail_text.begin(), tail_text.end(), CERT_END.begin(), CERT_END.end()) != tail_text.end();
}
bool read_cached_etag(const char *path, int64_t bundle_size, std::string &etag);
const char *choose_ca_store() noexcept {
  const bool sd_ok = plausible_ca_file(cfg::CA_BUNDLE_SD.data());
  const bool tmp_ok = plausible_ca_file(cfg::CA_BUNDLE_SD_TMP.data());
  if (!tmp_ok) {
    return sd_ok ? cfg::CA_BUNDLE_SD.data() : cfg::CA_BUNDLE_ROMFS.data();
  }
  if (!sd_ok) {
    return cfg::CA_BUNDLE_SD_TMP.data();
  }
  const std::optional<int64_t> tmp_size = sd::file_size(cfg::CA_BUNDLE_SD_TMP.data());
  if (tmp_size) {
    std::string etag;
    if (read_cached_etag(cfg::CA_BUNDLE_ETAG.data(), *tmp_size, etag)) {
      return cfg::CA_BUNDLE_SD_TMP.data();
    }
  }
  return cfg::CA_BUNDLE_SD.data();
}
void trim_ascii(std::string_view &text) noexcept {
  const auto is_space = [](char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
}
bool read_cached_etag(const char *path, int64_t bundle_size, std::string &etag) {
  sys::FileHandle file = sd::open(path, "rb");
  if (!file) {
    return false;
  }
  std::array<char, 256> buffer{};
  const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file.get());
  const std::string_view text{buffer.data(), read};
  const std::size_t newline = text.find('\n');
  if (newline == std::string_view::npos || newline == 0) {
    return false;
  }
  int64_t stored_size = 0;
  for (const char digit : text.substr(0, newline)) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    stored_size = stored_size * 10 + (digit - '0');
    if (stored_size > static_cast<int64_t>(cfg::CA_BUNDLE_MAX_SIZE)) {
      return false;
    }
  }
  if (stored_size != bundle_size) {
    return false;
  }
  std::string_view tag = text.substr(newline + 1);
  trim_ascii(tag);
  if (tag.empty()) {
    return false;
  }
  etag.assign(tag);
  return true;
}
bool write_cached_etag(int64_t bundle_size, const std::string &etag) {
  const std::string contents = std::to_string(bundle_size) + "\n" + etag + "\n";
  {
    sys::FileHandle file = sd::open(cfg::CA_BUNDLE_ETAG_TMP.data(), "wb");
    if (!file) {
      return false;
    }
    const bool wrote =
        std::fwrite(contents.data(), 1, contents.size(), file.get()) == contents.size();
    if (!wrote || std::ferror(file.get()) != 0) {
      sd::unlink_quietly(cfg::CA_BUNDLE_ETAG_TMP.data());
      return false;
    }
    if (file.get() != nullptr) {
      (void)std::fflush(file.get());
      (void)fsync(fileno(file.get()));
    }
    const bool closed = file.close();
    if (!wrote || !closed) {
      sd::unlink_quietly(cfg::CA_BUNDLE_ETAG_TMP.data());
      return false;
    }
  }
  if (!sd::replace_file(cfg::CA_BUNDLE_ETAG_TMP.data(), cfg::CA_BUNDLE_ETAG.data())) {
    sd::unlink_quietly(cfg::CA_BUNDLE_ETAG_TMP.data());
    return false;
  }
  return true;
}
std::size_t capture_etag_header(char *buffer, std::size_t size, std::size_t count,
                                void *userp) {
  const std::size_t total = size * count;
  auto *etag = static_cast<std::string *>(userp);
  std::string_view line{buffer, total};
  constexpr std::string_view PREFIX = "etag:";
  if (line.size() < PREFIX.size()) {
    return total;
  }
  for (std::size_t i = 0; i < PREFIX.size(); ++i) {
    const char c = line[i];
    if (c >= 'A' && c <= 'Z') {
      if (c - 'A' + 'a' != PREFIX[i]) {
        return total;
      }
    } else if (c != PREFIX[i]) {
      return total;
    }
  }
  std::string_view value = line.substr(PREFIX.size());
  trim_ascii(value);
  if (!value.empty()) {
    etag->assign(value);
  }
  return total;
}
}
void init_ca_bundle() noexcept {
  const char *store = choose_ca_store();
  if (store != nullptr) {
    ca_bundle_path.store(store, std::memory_order_release);
  } else {
    ca_bundle_path.store(cfg::CA_BUNDLE_ROMFS.data(), std::memory_order_release);
  }
}
void configure(CURL *curl) noexcept {
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_to_string);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, cfg::USER_AGENT.data());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  CURLcode rc;
  rc = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  if (rc != CURLE_OK) {
    status::print("Failed to configure SSL peer verification");
    return;
  }
  rc = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  if (rc != CURLE_OK) {
    status::print("Failed to configure SSL host verification");
    return;
  }
  const char *bundle = ca_bundle_path.load(std::memory_order_acquire);
  if (bundle != nullptr) {
    rc = curl_easy_setopt(curl, CURLOPT_CAINFO, bundle);
    if (rc != CURLE_OK) {
      status::print("Failed to set CA bundle path");
      return;
    }
  }
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (share.get() != nullptr) {
    curl_easy_setopt(curl, CURLOPT_SHARE, share.get());
  }
}
namespace {
bool cancelled(const std::atomic<bool> *cancel) noexcept {
  return cancel != nullptr && cancel->load(std::memory_order_acquire);
}
int progress_abort(void *userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return cancelled(static_cast<const std::atomic<bool> *>(userp)) ? 1 : 0;
}
void configure_cancellation(CURL *curl, const std::atomic<bool> *cancel) noexcept {
  if (cancel != nullptr) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &progress_abort);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool> *>(cancel));
  } else {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  }
}
void sleep_cancellable(u64 delay_ns, const std::atomic<bool> *cancel) noexcept {
  u64 elapsed_ns = 0;
  while (delay_ns > 0) {
    if (cancelled(cancel)) {
      return;
    }
    const u64 slice = elapsed_ns > 0
                          ? std::min<u64>(delay_ns, 250'000'000ULL)
                          : std::min<u64>(delay_ns, 50'000'000ULL);
    svcSleepThread(slice);
    delay_ns -= slice;
    elapsed_ns += slice;
  }
}
}
Response get(CURL *curl, const std::string &url, const std::atomic<bool> *cancel) {
  Response response;
  if (curl == nullptr || cancelled(cancel)) {
    return response;
  }
  response.data.reserve(cfg::RESPONSE_RESERVE);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.data);
  configure_cancellation(curl, cancel);
  const CURLcode outcome = curl_easy_perform(curl);
  if (outcome == CURLE_OK &&
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code) == CURLE_OK) {
    response.ok = response.status_code >= 200 && response.status_code < 300;
  }
  if (!response.ok) {
    if (outcome != CURLE_ABORTED_BY_CALLBACK) {
      failed_requests.fetch_add(1, std::memory_order_relaxed);
    }
    response.data.clear();
  }
  return response;
}
Response get_with_retry(CURL *curl, const std::string &url, int max_attempts,
                        const std::atomic<bool> *cancel) {
  Response response;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (cancelled(cancel)) {
      break;
    }
    if (attempt > 0) {
      u64 delay = cfg::RETRY_BASE_DELAY_NS;
      for (int i = 1; i < attempt; ++i) {
        delay *= 2;
        if (delay > 30'000'000'000ULL) {
          delay = 30'000'000'000ULL;
          break;
        }
      }
      sleep_cancellable(delay, cancel);
      if (cancelled(cancel)) {
        break;
      }
    }
    response = get(curl, url, cancel);
    if (response.ok) {
      return response;
    }
    if (request_too_large(response.status_code)) {
      break;
    }
    if (response.status_code == 429 || response.status_code == 503) {
      sleep_cancellable(cfg::THROTTLE_DELAY_NS, cancel);
    }
  }
  return response;
}
namespace {
CURLcode fetch_ca_bundle(bool verify, const std::string &url,
                         const std::string &cached_etag, std::string &body,
                         std::string &reply_etag, long &status,
                         const std::atomic<bool> *cancel) {
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    return CURLE_FAILED_INIT;
  }
  configure(curl);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &capture_etag_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &reply_etag);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
  if (!verify) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  configure_cancellation(curl, cancel);
  curl_slist *headers = nullptr;
  if (!cached_etag.empty()) {
    const std::string header = "If-None-Match: " + cached_etag;
    headers = curl_slist_append(headers, header.c_str());
    if (headers != nullptr) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
  }
  const CURLcode outcome = curl_easy_perform(curl);
  status = 0;
  if (outcome == CURLE_OK) {
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return outcome;
}
bool install_ca_bundle(const std::string &body, const std::string &expected_hash) {
  if (!plausible_ca_bundle(body)) {
    log_event("CA bundle: downloaded file was rejected");
    return false;
  }
  if (!expected_hash.empty() && !verify_ca_bundle_hash(body, expected_hash)) {
    log_event("CA bundle: downloaded file failed hash verification");
    return false;
  }
  if (!sd::make_directories(cfg::CA_BUNDLE_SD_DIR)) {
    log_event("CA bundle: could not create sdmc:/config/ssl/");
    return false;
  }
  {
    sys::FileHandle file = sd::open(cfg::CA_BUNDLE_SD_TMP.data(), "wb");
    if (!file) {
      log_event("CA bundle: could not open the temporary file");
      return false;
    }
    const bool wrote = std::fwrite(body.data(), 1, body.size(), file.get()) == body.size();
    if (!wrote || std::ferror(file.get()) != 0) {
      sd::unlink_quietly(cfg::CA_BUNDLE_SD_TMP.data());
      log_event("CA bundle: could not write the temporary file");
      return false;
    }
    if (file.get() != nullptr) {
      (void)std::fflush(file.get());
      (void)fsync(fileno(file.get()));
    }
    const bool closed = file.close();
    if (!closed) {
      sd::unlink_quietly(cfg::CA_BUNDLE_SD_TMP.data());
      log_event("CA bundle: could not close the temporary file");
      return false;
    }
  }
  if (sd::replace_file(cfg::CA_BUNDLE_SD_TMP.data(), cfg::CA_BUNDLE_SD.data())) {
    ca_bundle_path.store(cfg::CA_BUNDLE_SD.data(), std::memory_order_release);
    log_event("CA bundle: updated sdmc:/config/ssl/cacert.pem");
    return true;
  }
  if (!plausible_ca_file(cfg::CA_BUNDLE_SD.data())) {
    sd::unlink_quietly(cfg::CA_BUNDLE_SD.data());
    if (sd::replace_file(cfg::CA_BUNDLE_SD_TMP.data(), cfg::CA_BUNDLE_SD.data())) {
      ca_bundle_path.store(cfg::CA_BUNDLE_SD.data(), std::memory_order_release);
      log_event("CA bundle: updated sdmc:/config/ssl/cacert.pem");
      return true;
    }
  }
  if (plausible_ca_file(cfg::CA_BUNDLE_SD_TMP.data())) {
    ca_bundle_path.store(cfg::CA_BUNDLE_SD_TMP.data(), std::memory_order_release);
    log_event("CA bundle: could not replace sdmc:/config/ssl/cacert.pem; "
              "using sdmc:/config/ssl/cacert.pem.tmp");
    return true;
  }
  sd::unlink_quietly(cfg::CA_BUNDLE_SD_TMP.data());
  log_event("CA bundle: could not replace sdmc:/config/ssl/cacert.pem");
  return false;
}
}
bool refresh_ca_bundle(const std::atomic<bool> *cancel) {
  if (cancelled(cancel)) {
    return false;
  }
  const char *store = choose_ca_store();
  const bool have_store = store != nullptr && store != cfg::CA_BUNDLE_ROMFS.data();
  std::string cached_etag;
  if (have_store) {
    ca_bundle_path.store(store, std::memory_order_release);
    const std::optional<int64_t> size = sd::file_size(store);
    if (size) {
      (void)read_cached_etag(cfg::CA_BUNDLE_ETAG.data(), *size, cached_etag);
    }
  }
  std::string body;
  std::string reply_etag;
  long status = 0;
  CURLcode outcome =
      fetch_ca_bundle(have_store, cfg::CA_BUNDLE_URL.data(), cached_etag, body, reply_etag, status, cancel);
  if (cancelled(cancel)) {
    return false;
  }
  bool fetched_unverified = false;
  if (have_store &&
      (outcome == CURLE_PEER_FAILED_VERIFICATION || outcome == CURLE_SSL_CACERT_BADFILE)) {
    log_event("CA bundle: certificate check failed; retrying unverified with hash verification");
    body.clear();
    reply_etag.clear();
    outcome = fetch_ca_bundle(false, cfg::CA_BUNDLE_URL.data(), std::string{}, body, reply_etag, status, cancel);
    fetched_unverified = true;
    if (cancelled(cancel)) {
      return false;
    }
  }
  if (outcome != CURLE_OK) {
    log_event("CA bundle: refresh failed");
    return false;
  }
  if (status == 304) {
    return false;
  }
  if (status != 200) {
    log_event("CA bundle: unexpected HTTP status");
    return false;
  }
  std::string hash_body;
  std::string hash_etag;
  long hash_status = 0;
  CURLcode hash_outcome =
      fetch_ca_bundle(true, cfg::CA_BUNDLE_HASH_URL.data(), std::string{}, hash_body, hash_etag, hash_status, cancel);
  if (cancelled(cancel)) {
    return false;
  }
  if (hash_outcome != CURLE_OK || hash_status != 200) {
    if (!have_store || fetched_unverified) {
      log_event("CA bundle: hash fetch unavailable; keeping shipped anchor");
      return false;
    }
    log_event("CA bundle: hash fetch unavailable, skipping hash verification");
    hash_body.clear();
  }
  std::string hash_hex;
  if (!hash_body.empty()) {
    const std::size_t first_space = hash_body.find_first_of(" \t\r\n");
    hash_hex = hash_body.substr(0, first_space);
  }
  if (!install_ca_bundle(body, hash_hex)) {
    return false;
  }
  if (reply_etag.empty()) {
    sd::unlink_quietly(cfg::CA_BUNDLE_ETAG.data());
  } else if (!write_cached_etag(static_cast<int64_t>(body.size()), reply_etag)) {
    log_event("CA bundle: updated, but could not cache the ETag");
  }
  return true;
}
}
}
