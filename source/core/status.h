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
template <typename... Args>
void print(fmt::Spec<std::type_identity_t<Args>...> spec, const Args &...args) {
  std::string text = fmt::format(spec, args...);
  {
    std::scoped_lock guard{lock};
    line = std::move(text);
  }
  version.fetch_add(1, std::memory_order_release);
}
[[nodiscard]] u32 revision() noexcept;
[[nodiscard]] std::string current();
void begin_feed();
void mark_feed_done(bool ok, bool updated);
[[nodiscard]] bool feed_finished();
[[nodiscard]] bool feed_succeeded();
[[nodiscard]] bool feed_updated();
[[nodiscard]] int64_t last_feed_time() noexcept;
}
}
