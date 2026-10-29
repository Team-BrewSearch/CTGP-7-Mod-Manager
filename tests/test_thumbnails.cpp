#include "support/test_support.h"
#include "backend/sd_card.h"
#include "backend/thumbnail_source.h"
#include "frontend/model.h"
#include "frontend/thumbnail_cache.h"

// These tests exercise cache scheduling/publication, without a JPEG decoder or GPU.
namespace mm::store { Registry installed; }
namespace mm::sd { bool make_directories(std::string_view) { return true; } }
namespace mm::thumbs {
std::atomic<bool> quit_requested{false};
bool produce(CURL *, int, const char *, u16 *) { throw std::runtime_error("unexpected worker"); }
extern int failure_count;
}
namespace {
namespace thumbs = mm::thumbs;
struct Fixture {
  Fixture() {
    thumbs::ready = false;
    thumbs::failure_count = thumbs::failure_cursor = 0;
    thumbs::failures.fill({0, 0});
    thumbs::in_flight.fill(0);
    thumbs::wanted_count = 0;
    thumbs::queue_size = 0;
    thumbs::frame_counter = 0;
    mm::model::mods.clear();
    mm::model::window_start = 0;
    for (auto &slot : thumbs::slots) slot.mod_id = 0;
    for (auto &publication : thumbs::publications) {
      publication.ready = publication.ok = false;
      publication.mod_id = 0;
    }
  }
  ~Fixture() { thumbs::ready = false; }
};
mm::store::ModData mod(int id) {
  mm::store::ModData result;
  result.id = id;
  result.thumbnail_url = "https://example.test/thumb.jpg";
  return result;
}
}
TEST(has_arrivals_is_false_until_ready_and_tracks_publication_consumption) {
  Fixture fixture;
  thumbs::publications[1].ready = true;
  CHECK(!thumbs::has_arrivals());
  thumbs::ready = true;
  CHECK(thumbs::has_arrivals());
  thumbs::publications[1].ok = false;
  thumbs::publications[1].mod_id = 7;
  thumbs::tick();
  CHECK(!thumbs::has_arrivals());
  CHECK(thumbs::has_failed(7));
}
TEST(failed_publication_is_not_immediately_requeued) {
  Fixture fixture;
  mm::model::mods = {mod(7), mod(8)};
  thumbs::ready = true;
  thumbs::publications[0].mod_id = 7;
  thumbs::publications[0].ok = false;
  thumbs::publications[0].ready = true;
  thumbs::tick();
  CHECK(thumbs::queue_size == 1);
  CHECK(thumbs::queue[0].mod_id == 8);
  CHECK(!thumbs::publications[0].ready);
  thumbs::tick();
  CHECK(thumbs::queue_size == 1);
  CHECK(thumbs::queue[0].mod_id == 8);
}
TEST(repeated_failure_of_one_mod_does_not_evict_other_failure_records) {
  Fixture fixture;
  thumbs::mark_failed(1);
  thumbs::mark_failed(2);
  for (int i = 0; i < 100; ++i) thumbs::mark_failed(1);
  CHECK(thumbs::failure_count == 2);
  CHECK(thumbs::has_failed(1));
  CHECK(thumbs::has_failed(2));
  CHECK(thumbs::failures[0].second == mm::cfg::THUMB_FAIL_RETRY);
  thumbs::mark_failed(0);
  CHECK(thumbs::failure_count == 2);
  CHECK(thumbs::has_failed(0));
}
TEST(failure_ring_wraps_at_capacity_without_losing_recent_ids) {
  Fixture fixture;
  for (int id = 1; id <= mm::cfg::THUMB_FAIL_RING + 1; ++id) thumbs::mark_failed(id);
  CHECK(thumbs::failure_count == mm::cfg::THUMB_FAIL_RING);
  CHECK(!thumbs::has_failed(1));
  CHECK(thumbs::has_failed(2));
  CHECK(thumbs::has_failed(mm::cfg::THUMB_FAIL_RING + 1));
}
TEST(successful_unwanted_arrival_is_acknowledged_without_installing_a_texture) {
  Fixture fixture;
  thumbs::ready = true;
  thumbs::publications[0].mod_id = 99;
  thumbs::publications[0].ok = true;
  thumbs::publications[0].ready = true;
  thumbs::in_flight[0] = 99;
  thumbs::tick();
  CHECK(!thumbs::has_arrivals());
  CHECK(thumbs::in_flight[0] == 0);
  CHECK(!thumbs::is_resident(99));
  CHECK(!thumbs::has_failed(99));
}
