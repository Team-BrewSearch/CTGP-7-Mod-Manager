#include "core/time.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
namespace mm {
namespace timeutil {
std::string format_update_label(int64_t ts_seconds) {
  if (ts_seconds <= 0) {
    return std::string("Updated: unknown");
  }
  std::time_t t = static_cast<std::time_t>(ts_seconds);
  std::tm *gmt = std::gmtime(&t);
  if (gmt == nullptr) return std::string("Updated: unknown");
  std::ostringstream out;
  out << std::put_time(gmt, "%b %d %Y");
  return std::string("Updated: ") + out.str();
}
std::string format_age(int64_t ts_seconds) {
  if (ts_seconds <= 0) {
    return "unknown";
  }
  const std::time_t now = std::time(nullptr);
  if (now <= 0) {
    return "unknown";
  }
  const int64_t delta = static_cast<int64_t>(now) - ts_seconds;
  if (delta < 60) {
    return "just now";
  }
  if (delta < 60 * 60) {
    return std::to_string(delta / 60) + "m ago";
  }
  if (delta < 24 * 60 * 60) {
    return std::to_string(delta / (60 * 60)) + "h ago";
  }
  return std::to_string(delta / (24 * 60 * 60)) + "d ago";
}
}
}
