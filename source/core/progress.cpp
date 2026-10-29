#include "core/progress.h"
#include "core/status.h"
namespace mm {
namespace progress {
std::atomic<int> jobs_done{0};
std::atomic<int> jobs_total{0};
std::string phase_label_storage;
sys::Mutex label_mutex_;
sys::Mutex &label_mutex() {
  return label_mutex_;
}
std::string label() {
  const std::scoped_lock guard{label_mutex_};
  return phase_label_storage;
}
void begin(const char *label, std::size_t total) {
  {
    const std::scoped_lock guard{label_mutex_};
    phase_label_storage = label;
  }
  const int count = static_cast<int>(total);
  jobs_total.store(count, std::memory_order_release);
  jobs_done.store(0, std::memory_order_release);
  status::print("{}0/{}...", label, count);
}
void step() {
  const int done = jobs_done.fetch_add(1, std::memory_order_release) + 1;
  status::print("{}{}/{}...", label(), done,
                jobs_total.load(std::memory_order_acquire));
}
}
}
