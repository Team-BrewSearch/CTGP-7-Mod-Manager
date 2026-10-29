#include "core/status.h"
#include <ctime>
namespace mm {
namespace status {
sys::Mutex lock;
std::string line = "Initializing...";
bool feed_done = false;
bool feed_ok = false;
bool feed_wrote_list = false;
std::atomic<u32> version{0};
std::atomic<int64_t> feed_time{0};
u32 revision() noexcept {
  return version.load(std::memory_order_acquire);
}
std::string current() {
  std::scoped_lock guard{lock};
  return line;
}
void begin_feed() {
  {
    std::scoped_lock guard{lock};
    feed_done = false;
    feed_ok = false;
    feed_wrote_list = false;
  }
  version.fetch_add(1, std::memory_order_release);
}
void seed_feed_time(int64_t mtime) noexcept {
  if (mtime > 0) {
    feed_time.store(mtime, std::memory_order_release);
  }
}
void mark_feed_done(bool ok, bool updated) {
  {
    std::scoped_lock guard{lock};
    feed_done = true;
    feed_ok = ok;
    feed_wrote_list = updated;
    if (ok && updated) {
      feed_time.store(static_cast<int64_t>(std::time(nullptr)),
                      std::memory_order_release);
    }
  }
  version.fetch_add(1, std::memory_order_release);
}
bool feed_finished() {
  std::scoped_lock guard{lock};
  return feed_done;
}
bool feed_succeeded() {
  std::scoped_lock guard{lock};
  return feed_ok;
}
bool feed_updated() {
  std::scoped_lock guard{lock};
  return feed_wrote_list;
}
int64_t last_feed_time() noexcept {
  return feed_time.load(std::memory_order_acquire);
}
}
}
