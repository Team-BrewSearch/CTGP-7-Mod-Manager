#include "support/test_support.h"
#include "backend/feed.h"
#include "backend/json.h"
#include "backend/net.h"
#include "backend/sd_card.h"
#include "core/config.h"
#include "core/status.h"
#include <utime.h>

namespace {
namespace feed = mm::feed;
namespace cfg = mm::cfg;
using mm::store::ModData;
struct Fixture {
  test::Sandbox sandbox;
  Fixture() {
    CHECK(mm::sd::init_paths());
    feed::quit_requested = false;
    feed::core_batch_cap = cfg::CORE_BATCH_SIZE;
    mm::net::failed_requests = 0;
    mm::status::begin_feed();
    mm::net::init_ca_bundle();
  }
};
ModData mod(int id) { ModData result; result.id = id; return result; }
mm::sys::JsonRef json(const std::string &text) {
  mm::sys::JsonRef root{json_tokener_parse(text.c_str())};
  CHECK(bool(root));
  return root;
}
std::string files(int id = 9, int date = 100) {
  return "{\"aFiles\":[{\"_idRow\":" + std::to_string(id) +
         ",\"_tsDateAdded\":" + std::to_string(date) + ",\"_sFile\":\"mod.zip\"}]}";
}
void cache_at(std::int64_t mtime) {
  CHECK(mm::store::write_mod_list(cfg::MOD_LIST_FILE.data(), {mod(99)}));
  utimbuf times{static_cast<std::time_t>(mtime), static_cast<std::time_t>(mtime)};
  CHECK(::utime(cfg::MOD_LIST_FILE.data(), &times) == 0);
}
void queue_pipeline() {
  test::replies.push_back({CURLE_OK, 200,
      R"({"_aMetadata":{"_nRecordCount":2},"_aRecords":[{"_idRow":1,"_sName":"Mario","_sInitialVisibility":"show"},{"_idRow":2,"_sName":"Non-character","_sInitialVisibility":"show"}]})"});
  test::replies.push_back({CURLE_OK, 200,
      "[[" + files(11) + ",10604],[" + files(22) + ",10608]]"});
}
}
TEST(latest_file_accepts_category_wrappers_in_either_order_and_nested_rows) {
  Fixture fixture;
  for (const std::string &row : std::vector<std::string>{
           "[" + files() + ",10619]", "[10619," + files() + "]",
           "[[[" + files() + ",10619]]]"}) {
    auto root = json(row);
    ModData result;
    CHECK(feed::parse_latest_file(root.get(), result));
    CHECK(result.category_id == 10619);
    CHECK(result.latest_file_url == "https://gamebanana.com/dl/9");
    CHECK(result.latest_file_name == "mod.zip");
    CHECK(result.latest_file_date == 100);
  }
}
TEST(latest_file_preserves_known_category_and_selects_newest_valid_file) {
  Fixture fixture;
  auto root = json(R"([{"aFiles":[null,{"_idRow":0,"_tsDateAdded":999,"_sFile":"invalid.zip"},{"_idRow":2,"_tsDateAdded":300,"_sFile":""},{"_idRow":3,"_tsDateAdded":200,"_sFile":"new.zip"},{"_idRow":4,"_tsDateAdded":100,"_sFile":"old.zip"}]},10608])");
  ModData result;
  result.category_id = 10604;
  CHECK(feed::parse_latest_file(root.get(), result));
  CHECK(result.category_id == 10604);
  CHECK(result.latest_file_url == "https://gamebanana.com/dl/3");
  CHECK(result.latest_file_name == "new.zip");
  CHECK(result.latest_file_date == 200);
}
TEST(latest_file_rejects_empty_wrong_type_and_excessively_nested_rows) {
  Fixture fixture;
  for (const std::string &row : std::vector<std::string>{"[]", "[[]]", "[42]", "\"text\"", "[[[[[" + files() + "]]]]]"}) {
    ModData result;
    auto root = json(row);
    CHECK(!feed::parse_latest_file(root.get(), result));
    CHECK(result.latest_file_url.empty());
  }
  ModData result;
  CHECK(!feed::parse_latest_file(nullptr, result));
}
TEST(latest_file_accepts_four_wrappers_and_legacy_file_dictionary) {
  Fixture fixture;
  ModData result;
  auto root = json("[[[[" + files() + "]]]]");
  CHECK(feed::parse_latest_file(root.get(), result));
  CHECK(result.latest_file_date == 100);
  root = json(R"({"first":{"_idRow":8,"_tsDateAdded":500,"_sFile":"legacy.zip"}})");
  CHECK(feed::parse_latest_file(root.get(), result));
  CHECK(result.latest_file_name == "legacy.zip");
}
TEST(core_request_indexes_only_unresolved_targets_and_maps_rows_in_order) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  mm::net::configure(curl.get());
  std::vector<ModData> mods{mod(1), mod(2), mod(3)};
  mods[1].latest_file_url = "already-resolved";
  test::replies.push_back({CURLE_OK, 200, "[" + files(10) + "," + files(30) + "]"});
  feed::fetch_core_range(curl.get(), mods);
  CHECK(test::requests.size() == 1);
  const auto &url = test::requests[0].url;
  CHECK(url.find("itemid[0]=1") != std::string::npos);
  CHECK(url.find("itemid[1]=3") != std::string::npos);
  CHECK(url.find("itemid[2]") == std::string::npos);
  CHECK(url.find("Files().aFiles(),catid") != std::string::npos);
  CHECK(url.back() != '&');
  CHECK(mods[0].latest_file_url == "https://gamebanana.com/dl/10");
  CHECK(mods[1].latest_file_url == "already-resolved");
  CHECK(mods[2].latest_file_url == "https://gamebanana.com/dl/30");
}
TEST(core_splits_size_rejections_and_lowers_cap_but_not_for_bad_json) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  mm::net::configure(curl.get());
  for (bool size_rejection : {true, false}) {
    feed::core_batch_cap = cfg::CORE_BATCH_SIZE;
    std::vector<ModData> mods{mod(1), mod(2)};
    test::replies.push_back({CURLE_OK, size_rejection ? 413L : 200L, "invalid json"});
    test::replies.push_back({CURLE_OK, 200, "[" + files(10) + "]"});
    test::replies.push_back({CURLE_OK, 200, "[" + files(20) + "]"});
    feed::fetch_core_range(curl.get(), mods);
    CHECK(!mods[0].latest_file_url.empty());
    CHECK(!mods[1].latest_file_url.empty());
    CHECK(feed::core_batch_cap == (size_rejection ? 1 : cfg::CORE_BATCH_SIZE));
  }
}
TEST(core_url_limit_does_not_drop_the_remaining_mods) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  mm::net::configure(curl.get());
  feed::core_batch_cap = 100;
  std::vector<ModData> mods;
  for (int id = 1; id <= 100; ++id) mods.push_back(mod(id));
  test::responder = [](const test::Request &request) {
    CHECK(request.url.size() <= 4096);
    std::string rows = "[";
    std::size_t at = 0;
    while ((at = request.url.find("itemid[", at)) != std::string::npos) {
      if (rows.size() > 1) rows += ',';
      const auto equals = request.url.find('=', at);
      rows += files(std::stoi(request.url.substr(equals + 1)));
      at = equals + 1;
    }
    return test::Reply{CURLE_OK, 200, rows + "]"};
  };
  feed::fetch_core_range(curl.get(), mods);
  CHECK(test::requests.size() > 1);
  for (const auto &entry : mods) {
    CHECK(entry.latest_file_url == "https://gamebanana.com/dl/" + std::to_string(entry.id));
  }
}
TEST(core_empty_resolved_and_cancelled_ranges_do_not_request) {
  Fixture fixture;
  std::vector<ModData> mods{mod(1)};
  mods[0].latest_file_url = "ready";
  feed::fetch_core_range(nullptr, {});
  feed::fetch_core_range(nullptr, mods);
  feed::quit_requested = true;
  mods[0].latest_file_url.clear();
  feed::fetch_core_range(nullptr, mods);
  CHECK(test::requests.empty());
  feed::lower_core_cap(0);
  CHECK(feed::core_batch_cap == 1);
  feed::lower_core_cap(50);
  CHECK(feed::core_batch_cap == 1);
}
TEST(pipeline_filters_all_non_character_categories_before_persisting) {
  Fixture fixture;
  std::string records, rows;
  std::vector<int> categories{10604, 0};
  categories.insert(categories.end(), cfg::NON_CHARACTER_CATEGORIES.begin(), cfg::NON_CHARACTER_CATEGORIES.end());
  for (std::size_t i = 0; i < categories.size(); ++i) {
    if (i) { records += ','; rows += ','; }
    records += "{\"_idRow\":" + std::to_string(i + 1) + ",\"_sInitialVisibility\":\"show\"}";
    rows += "[" + files(int(i + 1)) + "," + std::to_string(categories[i]) + "]";
  }
  test::replies.push_back({CURLE_OK, 200, "{\"_aRecords\":[" + records + "]}"});
  test::replies.push_back({CURLE_OK, 200, "[" + rows + "]"});
  CHECK(feed::run_pipeline());
  std::vector<ModData> loaded;
  CHECK(mm::store::read_mod_list(cfg::MOD_LIST_FILE.data(), loaded));
  CHECK(loaded.size() == 2);
  CHECK(loaded[0].id == 1);
  CHECK(loaded[1].id == 2);
}
TEST(pipeline_failed_save_and_empty_response_preserve_cached_list) {
  Fixture fixture;
  cache_at(test::NOW - 10);
  const auto original = test::read(cfg::MOD_LIST_FILE);
  test::fail_json_write = true;
  queue_pipeline();
  CHECK(!feed::run_pipeline());
  CHECK(test::read(cfg::MOD_LIST_FILE) == original);
  test::fail_json_write = false;
  test::replies.push_back({CURLE_OK, 200, R"({"_aRecords":[]})"});
  CHECK(!feed::run_pipeline());
  CHECK(test::read(cfg::MOD_LIST_FILE) == original);
}
TEST(fresh_cache_skips_refresh_and_seeds_status) {
  Fixture fixture;
  cache_at(test::NOW - cfg::FEED_MAX_AGE_SECONDS + 1);
  feed::thread_main(nullptr);
  CHECK(test::requests.empty());
  CHECK(mm::status::feed_succeeded());
  CHECK(mm::status::feed_finished());
  CHECK(!mm::status::feed_updated());
  CHECK(mm::status::last_feed_time() == test::NOW - cfg::FEED_MAX_AGE_SECONDS + 1);
}
TEST(expired_future_and_forced_caches_refresh_instead_of_skipping) {
  Fixture fixture;
  for (auto mtime : {test::NOW - cfg::FEED_MAX_AGE_SECONDS, test::NOW + 1, test::NOW}) {
    cache_at(mtime);
    mm::status::begin_feed();
    const auto before = test::requests.size();
    test::replies.push_back({CURLE_OK, 500}); // CA refresh is best effort.
    queue_pipeline();
    int force_marker = 0;
    feed::thread_main(mtime == test::NOW ? &force_marker : nullptr);
    CHECK(test::requests.size() == before + 3);
    CHECK(mm::status::feed_succeeded());
    CHECK(mm::status::feed_updated());
    CHECK(mm::status::last_feed_time() == test::NOW);
  }
}
TEST(failed_refresh_retains_cached_timestamp_and_reports_failure) {
  Fixture fixture;
  cache_at(test::NOW - cfg::FEED_MAX_AGE_SECONDS);
  const auto original = test::read(cfg::MOD_LIST_FILE);
  test::replies.push_back({CURLE_OK, 500});
  test::replies.push_back({CURLE_OK, 200, R"({"_aRecords":[]})"});
  feed::thread_main(nullptr);
  CHECK(mm::status::feed_finished());
  CHECK(!mm::status::feed_succeeded());
  CHECK(!mm::status::feed_updated());
  CHECK(mm::status::last_feed_time() == test::NOW - cfg::FEED_MAX_AGE_SECONDS);
  CHECK(test::read(cfg::MOD_LIST_FILE) == original);
}
TEST(legacy_cache_without_version_cannot_suppress_refresh) {
  Fixture fixture;
  test::write(cfg::MOD_LIST_FILE, R"([{"Id":99}])");
  test::replies.push_back({CURLE_OK, 500});
  queue_pipeline();
  feed::thread_main(nullptr);
  CHECK(test::requests.size() == 3);
  CHECK(mm::status::feed_updated());
  CHECK(mm::store::mod_list_version_ok());
}
TEST(cancellation_after_index_response_prevents_persistence) {
  Fixture fixture;
  cache_at(test::NOW - 10);
  const auto previous = test::read(cfg::MOD_LIST_FILE);
  test::replies.push_back({CURLE_OK, 200, R"({"_aRecords":[]})", {}, [] { feed::quit_requested = true; }});
  CHECK(!feed::run_pipeline());
  CHECK(test::requests.size() == 1);
  CHECK(test::read(cfg::MOD_LIST_FILE) == previous);
}
