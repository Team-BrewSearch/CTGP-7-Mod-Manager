#include "support/test_support.h"
#include "backend/json.h"
#include "backend/sd_card.h"
#include "backend/store.h"
#include "core/config.h"
#include <cerrno>
#include <sys/stat.h>
#include <utime.h>

namespace {
namespace store = mm::store;
namespace cfg = mm::cfg;
struct Fixture {
  test::Sandbox sandbox;
  Fixture() { store::installed.clear(); CHECK(mm::sd::init_paths()); }
};
}
TEST(replace_file_atomically_replaces_existing_contents) {
  Fixture fixture;
  test::write("old", "original");
  test::write("new", "replacement");
  CHECK(mm::sd::replace_file("new", "old"));
  CHECK(test::read("old") == "replacement");
  CHECK(!test::exists("new"));
  CHECK(!test::exists("old.backup"));
}
TEST(replace_file_supports_fat_style_rename_and_removes_backup) {
  Fixture fixture;
  test::write("old", "original");
  test::write("new", "replacement");
  test::rename_errors = {EEXIST, 0, 0};
  CHECK(mm::sd::replace_file("new", "old"));
  CHECK(test::read("old") == "replacement");
  CHECK(!test::exists("old.backup"));
  CHECK(test::renames.size() == 3);
}
TEST(replace_failure_restores_original_and_keeps_source) {
  Fixture fixture;
  test::write("old", "original");
  test::write("new", "replacement");
  test::rename_errors = {EEXIST, 0, EIO, 0};
  CHECK(!mm::sd::replace_file("new", "old"));
  CHECK(test::read("old") == "original");
  CHECK(test::read("new") == "replacement");
  CHECK(!test::exists("old.backup"));
}
TEST(replace_failure_to_back_up_never_discards_original) {
  Fixture fixture;
  test::write("old", "original");
  test::write("new", "replacement");
  test::rename_errors = {EEXIST, EACCES};
  CHECK(!mm::sd::replace_file("new", "old"));
  CHECK(test::read("old") == "original");
  CHECK(test::read("new") == "replacement");
}
TEST(file_mtime_rejects_missing_and_epoch_files) {
  Fixture fixture;
  CHECK(!mm::sd::file_mtime("missing"));
  test::write("dated", "data");
  utimbuf times{1234, 1234};
  CHECK(::utime("dated", &times) == 0);
  CHECK(mm::sd::file_mtime("dated") == 1234);
  times = {0, 0};
  CHECK(::utime("dated", &times) == 0);
  CHECK(!mm::sd::file_mtime("dated"));
}
TEST(mod_list_has_version_header_and_round_trips_public_fields) {
  Fixture fixture;
  store::ModData mod;
  mod.id = 7;
  mod.name = "Mario \"Deluxe\"";
  mod.author = "作者";
  mod.thumbnail_url = "https://example.test/thumb.jpg";
  mod.latest_file_url = "https://example.test/7";
  mod.latest_file_name = "archive.zip";
  mod.latest_file_date = 5'000'000'000LL;
  CHECK(store::write_mod_list(cfg::MOD_LIST_FILE.data(), {mod}));
  CHECK(store::mod_list_version_ok());
  const auto root = mm::js::read_file(cfg::MOD_LIST_FILE.data());
  CHECK(json_object_array_length(root.get()) == 2);
  CHECK(mm::js::integer_field<int>(json_object_array_get_idx(root.get(), 0), "CacheVersion") == cfg::MOD_LIST_VERSION);
  std::vector<store::ModData> loaded;
  CHECK(store::read_mod_list(cfg::MOD_LIST_FILE.data(), loaded));
  CHECK(loaded.size() == 1);
  CHECK(loaded[0].id == mod.id);
  CHECK(loaded[0].name == mod.name);
  CHECK(loaded[0].author == mod.author);
  CHECK(loaded[0].thumbnail_url == mod.thumbnail_url);
  CHECK(loaded[0].latest_file_url == mod.latest_file_url);
  CHECK(loaded[0].latest_file_name == mod.latest_file_name);
  CHECK(loaded[0].latest_file_date == mod.latest_file_date);
  CHECK(!test::exists(std::string(cfg::MOD_LIST_FILE) + ".tmp"));
}
TEST(empty_mod_list_still_has_a_valid_cache_header) {
  Fixture fixture;
  CHECK(store::write_mod_list(cfg::MOD_LIST_FILE.data(), {}));
  CHECK(store::mod_list_version_ok());
  std::vector<store::ModData> loaded(1);
  CHECK(store::read_mod_list(cfg::MOD_LIST_FILE.data(), loaded));
  CHECK(loaded.empty());
}
TEST(cache_version_rejects_old_missing_malformed_and_prefix_values) {
  Fixture fixture;
  CHECK(!store::mod_list_version_ok());
  for (const std::string text : {"[]", "[{\"Id\":7}]", "[{\"CacheVersion\":0}]",
           "[{\"CacheVersion\":10}]", "[{\"CacheVersion\":1oops}]",
           "[{\"CacheVersion\":\"1\"}]", "[{\"CacheVersion\" 1}]", "[{\"CacheVersion\":"}) {
    test::write(cfg::MOD_LIST_FILE, text);
    CHECK(!store::mod_list_version_ok());
  }
  test::write(cfg::MOD_LIST_FILE, "[ { \"CacheVersion\" \t:\r\n 1 } ]");
  CHECK(store::mod_list_version_ok());
}
TEST(mod_list_write_failures_preserve_prior_bytes_and_remove_partial_temporary) {
  Fixture fixture;
  for (bool write_failure : {true, false}) {
    test::write(cfg::MOD_LIST_FILE, "previous complete list");
    test::fail_json_write = write_failure;
    if (!write_failure) test::rename_errors = {EACCES, EACCES};
    CHECK(!store::write_mod_list(cfg::MOD_LIST_FILE.data(), {}));
    CHECK(test::read(cfg::MOD_LIST_FILE) == "previous complete list");
    CHECK(!test::exists(std::string(cfg::MOD_LIST_FILE) + ".tmp"));
  }
}
TEST(installed_save_round_trips_uninstall_marker_and_legacy_default) {
  Fixture fixture;
  store::installed.insert_or_assign(3, {123, {"a.chpack"}, "source.zip", false});
  CHECK(store::save_installed());
  store::installed.clear();
  CHECK(store::load_installed());
  CHECK(store::installed.find(3)->date == 123);
  CHECK(store::installed.find(3)->files == std::vector<std::string>({"a.chpack"}));
  CHECK(store::installed.find(3)->source_file_name == "source.zip");
  CHECK(!store::installed.find(3)->uninstalling);
  test::write(cfg::INSTALLED_FILE, R"({"3":{"Date":123,"Files":[]}})");
  CHECK(store::load_installed());
  CHECK(!store::installed.find(3)->uninstalling);
}
TEST(load_installed_recovers_interrupted_uninstall_and_preserves_other_mods) {
  Fixture fixture;
  test::write(std::string(cfg::CTGP7_DIR) + "delete.chpack", "remove");
  test::write(std::string(cfg::CTGP7_DIR) + "keep.chpack", "keep");
  test::write(cfg::INSTALLED_TMP, R"({"1":{"Files":["delete.chpack","already-missing.chpack"],"Uninstalling":true},"2":{"Files":["keep.chpack"],"Uninstalling":false}})");
  CHECK(store::load_installed());
  CHECK(!store::installed.contains(1));
  CHECK(store::installed.contains(2));
  CHECK(!test::exists(std::string(cfg::CTGP7_DIR) + "delete.chpack"));
  CHECK(test::read(std::string(cfg::CTGP7_DIR) + "keep.chpack") == "keep");
  CHECK(!test::exists(cfg::INSTALLED_TMP));
  store::installed.clear();
  CHECK(store::load_installed());
  CHECK(store::installed.size() == 1);
  CHECK(store::installed.contains(2));
}
TEST(installed_recovery_prefers_valid_temporary_and_falls_back_from_malformed_json) {
  Fixture fixture;
  test::write(cfg::INSTALLED_FILE, R"({"1":{"Date":1}})");
  test::write(cfg::INSTALLED_TMP, R"({"2":{"Date":2}})");
  CHECK(store::load_installed());
  CHECK(store::installed.contains(2));
  CHECK(!store::installed.contains(1));
  test::write(cfg::INSTALLED_TMP, "{broken");
  CHECK(store::load_installed());
  CHECK(store::installed.contains(1));
  CHECK(!test::exists(cfg::INSTALLED_TMP));
}
TEST(installed_save_failure_preserves_previous_registry_file) {
  Fixture fixture;
  test::write(cfg::INSTALLED_FILE, "original");
  store::installed.insert_or_assign(1, {});
  for (bool write_failure : {true, false}) {
    test::fail_json_write = write_failure;
    if (!write_failure) test::rename_errors = {EACCES, EACCES};
    CHECK(!store::save_installed());
    CHECK(test::read(cfg::INSTALLED_FILE) == "original");
    CHECK(!test::exists(cfg::INSTALLED_TMP));
  }
}
TEST(json_boolean_field_is_strict_and_uses_the_requested_default) {
  Fixture fixture;
  mm::sys::JsonRef root{json_tokener_parse(R"({"yes":true,"no":false,"integer":1,"string":"true","null":null})")};
  CHECK(bool(root));
  CHECK(mm::js::boolean_field(root.get(), "yes", false));
  CHECK(!mm::js::boolean_field(root.get(), "no", true));
  for (const char *key : {"missing", "integer", "string", "null"}) {
    CHECK(mm::js::boolean_field(root.get(), key, true));
    CHECK(!mm::js::boolean_field(root.get(), key, false));
  }
  CHECK(mm::js::boolean_field(nullptr, "missing", true));
}
