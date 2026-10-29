#include <ctime>
#include <mutex>
#include "core/log.h"
#include "core/config.h"
#include "core/system.h"
namespace mm {
namespace {
sys::Mutex log_mutex;
sys::FileHandle logfile;
std::string last_text;
std::string timestamp() noexcept {
  const std::time_t now = std::time(nullptr);
  if (now <= 0) {
    return std::string{};
  }
  std::tm gmt{};
  gmtime_r(&now, &gmt);
  char buffer[20]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &gmt) == 0) {
    return std::string{};
  }
  return buffer;
}
}
void log_event(const std::string &text) {
  const std::scoped_lock guard{log_mutex};
  if (!logfile) {
    logfile = sys::FileHandle{std::fopen(mm::cfg::LOG_FILE.data(), "a")};
    if (!logfile) {
      return;
    }
  }
  if (text != last_text) {
    const std::string line = timestamp() + " " + text;
    fprintf(logfile.get(), "%s\n", line.c_str());
    fflush(logfile.get());
    last_text = text;
  }
}
}
