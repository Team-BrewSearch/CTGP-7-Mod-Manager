#include "support/test_support.h"
#include "backend/sd_card.h"
#include "core/config.h"
#include "core/log.h"

TEST(log_appends_iso8601_timestamps_and_suppresses_only_consecutive_duplicates) {
  // One case per process: production keeps its logfile open for its lifetime.
  test::Sandbox sandbox;
  CHECK(mm::sd::init_paths());
  test::write(mm::cfg::LOG_FILE, "previous session\n");
  mm::log_event("first");
  mm::log_event("first");
  mm::log_event("second");
  mm::log_event("first");
  CHECK(test::read(mm::cfg::LOG_FILE) ==
        "previous session\n"
        "2027-01-15T08:00:00Z first\n"
        "2027-01-15T08:00:00Z second\n"
        "2027-01-15T08:00:00Z first\n");
}
