#pragma once
#include <cstdint>
#include <string>
namespace mm {
namespace timeutil {
std::string format_update_label(int64_t ts_seconds);
std::string format_age(int64_t ts_seconds);
}
}
