#include "support/test_support.h"
#include "core/format.h"
#include "core/progress.h"
#include "core/status.h"
#include "core/time.h"
#include <limits>

TEST(format_counts_escaped_and_adjacent_placeholders) {
  for (const auto &[text, count] : std::vector<std::pair<std::string, std::size_t>>{
           {"", 0}, {"{", 0}, {"}", 0}, {"{}{}", 2}, {"{{}}", 0},
           {"{{{}}}", 1}, {"{{}} {} {{}}", 1}, {"plain text", 0}}) {
    CHECK(mm::fmt::count_placeholders(std::string_view{text}) == count);
  }
}
TEST(format_accepts_valid_literals_and_preserves_escapes) {
  // Regression: valid calls must compile as well as produce the expected output.
  // Protect the constructor fix in the updated PR from rejecting valid calls.
  CHECK(mm::fmt::format("") == "");
  CHECK(mm::fmt::format("{{literal}}") == "{literal}");
  CHECK(mm::fmt::format("{{{}}} {}", 42, "ok") == "{42} ok");
  const char *missing = nullptr;
  CHECK(mm::fmt::format("a{}b", missing) == "ab");
  CHECK(mm::fmt::format("{}", std::numeric_limits<std::int64_t>::min()) == "-9223372036854775808");
}
TEST(age_formats_each_boundary_using_a_fixed_clock) {
  for (const auto &[age, label] : std::vector<std::pair<std::int64_t, std::string>>{
           {-1, "just now"}, {0, "just now"}, {59, "just now"}, {60, "1m ago"},
           {3599, "59m ago"}, {3600, "1h ago"}, {86399, "23h ago"},
           {86400, "1d ago"}, {3 * 86400 + 59, "3d ago"}}) {
    CHECK(mm::timeutil::format_age(test::NOW - age) == label);
  }
}
TEST(age_rejects_missing_timestamps_and_unavailable_clock) {
  CHECK(mm::timeutil::format_age(0) == "unknown");
  CHECK(mm::timeutil::format_age(-1) == "unknown");
  test::now = 0;
  CHECK(mm::timeutil::format_age(123) == "unknown");
}
TEST(feed_lifecycle_distinguishes_cached_success_from_a_new_list) {
  namespace status = mm::status;
  status::seed_feed_time(100);
  const auto revision = status::revision();
  status::begin_feed();
  CHECK(status::revision() == revision + 1);
  CHECK(!status::feed_finished());
  CHECK(!status::feed_succeeded());
  CHECK(!status::feed_updated());
  CHECK(status::last_feed_time() == 100);
  status::mark_feed_done(true, false);
  CHECK(status::feed_finished());
  CHECK(status::feed_succeeded());
  CHECK(!status::feed_updated());
  CHECK(status::last_feed_time() == 100);
  status::begin_feed();
  status::mark_feed_done(true, true);
  CHECK(status::feed_updated());
  CHECK(status::last_feed_time() == test::NOW);
}
TEST(failed_refresh_and_invalid_seed_preserve_last_successful_time) {
  mm::status::seed_feed_time(1234);
  mm::status::seed_feed_time(0);
  mm::status::seed_feed_time(-1);
  mm::status::begin_feed();
  mm::status::mark_feed_done(false, false);
  CHECK(mm::status::feed_finished());
  CHECK(!mm::status::feed_succeeded());
  CHECK(!mm::status::feed_updated());
  CHECK(mm::status::last_feed_time() == 1234);
}
TEST(status_print_increments_revision_even_when_text_repeats) {
  const auto before = mm::status::revision();
  mm::status::print("{}", "Ready");
  mm::status::print("{}", "Ready");
  CHECK(mm::status::current() == "Ready");
  CHECK(mm::status::revision() == before + 2);
}
TEST(progress_owns_its_label_and_resets_between_phases) {
  std::string label = "Fetching ";
  mm::progress::begin(label.c_str(), 2);
  label.assign("changed");
  mm::progress::step();
  CHECK(mm::progress::label() == "Fetching ");
  CHECK(mm::status::current() == "Fetching 1/2...");
  mm::progress::step();
  CHECK(mm::status::current() == "Fetching 2/2...");
  mm::progress::begin("Empty ", 0);
  CHECK(mm::progress::jobs_done.load() == 0);
  CHECK(mm::progress::jobs_total.load() == 0);
  CHECK(mm::status::current() == "Empty 0/0...");
}
