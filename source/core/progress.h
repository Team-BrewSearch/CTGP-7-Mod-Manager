#pragma once
#include "core/system.h"
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
namespace mm {
namespace progress {
extern std::atomic<int> jobs_done;
extern std::atomic<int> jobs_total;
extern std::string phase_label_storage;
/** Return the mutex used to protect phase_label_storage. */
sys::Mutex &label_mutex();
/** Return a copy of the phase label while holding its mutex. */
[[nodiscard]] std::string label();
/** Copy the phase label, reset completed jobs to zero, and publish progress for total jobs. */
void begin(const char *label, std::size_t total);
/** Increment completed jobs and publish progress; the count is not capped at the total. */
void step();
}
}
