#pragma once
#include <string>
namespace mm {
/**
 * Append and flush an event with a timestamp when available, suppressing consecutive duplicates.
 * On first open, truncate a log larger than LOG_MAX_SIZE; otherwise append. Open
 * failure drops the event, and write and flush errors are ignored.
 */
void log_event(const std::string &text);
}