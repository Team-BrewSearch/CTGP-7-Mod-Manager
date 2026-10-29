#pragma once
#include "core/format.h"
#include "core/system.h"
#include <atomic>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
namespace mm {
namespace status {
extern sys::Mutex lock;
extern std::string line;
extern bool feed_done;
extern bool feed_ok;
extern std::atomic<u32> version;
/** Replace the shared status line and advance its revision, even if the text is unchanged. */
template <typename... Args>
void print(fmt::Spec<std::type_identity_t<Args>...> spec, const Args &...args) {
  std::string text = fmt::format(spec, args...);
  {
    std::scoped_lock guard{lock};
    line = std::move(text);
  }
  version.fetch_add(1, std::memory_order_release);
}
/** Return the wrapping revision counter for status text and feed lifecycle changes. */
[[nodiscard]] u32 revision() noexcept;
[[nodiscard]] std::string current();
/** Reset feed completion and outcome flags and advance the revision, retaining the timestamp. */
void begin_feed();
/**
 * Record completion, success in ok, and whether a list was written in updated.
 * Set the feed time to current Unix seconds only when both flags are true, then
 * advance the revision.
 */
void mark_feed_done(bool ok, bool updated);
/** Set the feed timestamp from a positive modification time in Unix seconds; ignore other values. */
void seed_feed_time(int64_t mtime) noexcept;
[[nodiscard]] bool feed_finished();
/** Return the recorded success flag, which begin_feed resets until completion. */
[[nodiscard]] bool feed_succeeded();
/** Return the recorded list-write flag, which begin_feed resets for each refresh. */
[[nodiscard]] bool feed_updated();
/** Return the last seeded or successful-write time in Unix seconds, initially zero. */
[[nodiscard]] int64_t last_feed_time() noexcept;
}
}
