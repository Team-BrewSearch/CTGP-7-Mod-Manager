#pragma once
#include <cstdint>
#include <string>
namespace mm {
namespace timeutil {
std::string format_update_label(int64_t ts_seconds);
/**
 * Format a Unix timestamp in seconds as just now or whole minutes, hours, or days ago.
 * Future times are just now; nonpositive timestamps or an unavailable clock are unknown.
 */
std::string format_age(int64_t ts_seconds);
}
}
