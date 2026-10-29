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
sys::Mutex &label_mutex();
[[nodiscard]] std::string label();
void begin(const char *label, std::size_t total);
void step();
}
}
