#pragma once
#include "core/config.h"
#include "core/system.h"
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <string>
namespace mm {
namespace net {
struct Response {
  std::string data;
  long status_code = 0;
  bool ok = false;
};
extern std::atomic<int> failed_requests;
extern std::array<sys::Mutex, CURL_LOCK_DATA_LAST> share_locks;
extern sys::CurlShare share;
void share_lock_cb(CURL *, curl_lock_data data, curl_lock_access, void *);
void share_unlock_cb(CURL *, curl_lock_data data, void *);
void share_init();
void share_cleanup();
std::size_t write_to_string(void *contents, std::size_t size, std::size_t nmemb, void *userp);
/**
 * Configure a valid curl handle for bounded responses, redirects, TLS, and timeouts.
 * Use the selected CA bundle and available DNS share. Some TLS option failures
 * return early without returning an error or completing configuration.
 */
void configure(CURL *curl) noexcept;
/** Select the cached SD CA bundle or ROMFS fallback for subsequently configured handles. */
void init_ca_bundle() noexcept;
/**
 * Fetch and select a new CA bundle, caching an ETag when available.
 * Downloads without TLS verification require a digest fetched with TLS verification.
 * A verified download may be accepted when its digest is unavailable; a supplied
 * invalid digest rejects it. Return false for cancellation, HTTP 304, or refresh
 * failure. ETag save failure still returns true,
 * and replacement failure may leave the temporary bundle active.
 */
[[nodiscard]] bool refresh_ca_bundle(const std::atomic<bool> *cancel = nullptr);
/**
 * Perform one request on a configured curl handle; cancel must outlive the transfer.
 * Only HTTP 2xx responses are successful; failure returns an empty body. A null
 * handle or pre-set cancellation flag skips the transfer. Count failed attempts
 * except those aborted by the cancellation callback.
 */
[[nodiscard]] Response get(CURL *curl, const std::string &url,
                           const std::atomic<bool> *cancel = nullptr);
[[nodiscard]] constexpr bool request_too_large(long code) noexcept {
  return code == 400 || code == 413 || code == 414;
}
/**
 * Try get at most max_attempts times and return the last response.
 * Nonpositive limits or cancellation before the first attempt return an empty
 * failure. Backoff doubles up to 30 seconds; 429/503 add a throttle wait, while
 * 400/413/414 stop retries. The optional cancel flag also interrupts waits.
 */
[[nodiscard]] Response get_with_retry(CURL *curl, const std::string &url, int max_attempts,
                                      const std::atomic<bool> *cancel = nullptr);
template <typename Fn>
concept JobFunction = std::invocable<Fn &, CURL *, std::size_t>;
template <JobFunction Fn>
struct JobPool {
  std::atomic<std::size_t> next{0};
  std::size_t count = 0;
  Fn *job = nullptr;
  void run() noexcept {
    sys::CurlHandle curl;
    if (!curl) {
      return;
    }
    configure(curl.get());
    for (;;) {
      const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= count) {
        break;
      }
      (*job)(curl.get(), index);
    }
  }
  static void entry(void *self) noexcept {
    static_cast<JobPool *>(self)->run();
  }
};
template <JobFunction Fn>
void run_jobs(std::size_t count, Fn &job) {
  if (count == 0) {
    return;
  }
  JobPool<Fn> pool;
  pool.count = count;
  pool.job = &job;
  {
    sys::CurlHandle warm;
    if (warm) {
      configure(warm.get());
      const std::size_t index = pool.next.fetch_add(1, std::memory_order_relaxed);
      if (index < pool.count) {
        job(warm.get(), index);
      }
    }
  }
  if (pool.next.load(std::memory_order_relaxed) >= pool.count) {
    return;
  }
  const std::size_t remaining = pool.count - pool.next.load(std::memory_order_relaxed);
  const std::size_t wanted = std::min(remaining, cfg::FETCH_WORKERS);
  std::array<sys::Thread, cfg::FETCH_WORKERS> workers;
  std::size_t started = 0;
  for (std::size_t i = 0; i < wanted; ++i) {
    workers[i] = sys::Thread::spawn(&JobPool<Fn>::entry, &pool,
                                    cfg::WORKER_STACK_SIZE, cfg::WORKER_PRIORITY,
                                    cfg::ANY_CORE);
    if (!workers[i]) {
      break;
    }
    ++started;
  }
  if (started == 0) {
    pool.run();
    return;
  }
  for (std::size_t i = 0; i < started; ++i) {
    workers[i].join();
  }
}
}
}
