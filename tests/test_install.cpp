#include "support/test_support.h"
#include "backend/install.h"
#include "backend/sd_card.h"
#include "core/config.h"
#include "frontend/model.h"

// Transport configuration is outside these unit tests; install supplies its own
// write and progress callbacks, and every perform call is intercepted.
namespace mm::net { void configure(CURL *) noexcept {} }
namespace {
namespace install = mm::install;
namespace store = mm::store;
namespace cfg = mm::cfg;
struct Fixture {
  test::Sandbox sandbox;
  Fixture() {
    CHECK(mm::sd::init_paths());
    store::installed.clear();
    mm::model::mods.clear();
    mm::model::all_mods.clear();
    mm::model::search_query.clear();
    mm::model::queued_mod_ids.clear();
    mm::model::window_start = mm::model::selected = 0;
    install::quit_requested = false;
    install::cancel_requested = false;
    install::uninstalling = false;
    install::uninstall_pending = false;
    install::uninstall_mod_id = 0;
    install::uninstall_done = 0;
    install::percent = -1;
    install::uninstall_files.clear();
    install::slot = install::Slot::EMPTY;
    install::ready = true;
  }
};
void original_install() {
  store::installed.insert_or_assign(7, {100, {"old.chpack", "Shared.chpack"}, "old.zip", false});
  test::write(std::string(cfg::CTGP7_DIR) + "old.chpack", "old content");
  test::write(std::string(cfg::CTGP7_DIR) + "Shared.chpack", "shared content");
  CHECK(store::save_installed());
}
install::Result update() {
  install::Result result;
  result.mod_id = 7;
  result.file_date = 200;
  result.source_name = "new.zip";
  result.files = {"new.chpack", "shared.chpack"};
  result.ok = true;
  return result;
}
void select_installed() {
  store::ModData mod;
  mod.id = 7;
  mm::model::mods = {mod};
}
}
TEST(download_retries_transient_failures_with_fresh_file_and_exponential_delay) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  test::write(cfg::DOWNLOAD_TMP, "stale partial download");
  test::replies.push_back({CURLE_COULDNT_CONNECT, 0});
  test::replies.push_back({CURLE_RECV_ERROR, 0});
  test::replies.push_back({CURLE_OK, 200, "archive"});
  std::string message;
  CHECK(install::download(curl.get(), "https://example.test/mod.zip", message));
  CHECK(test::requests.size() == 3);
  CHECK(test::read(cfg::DOWNLOAD_TMP) == "archive");
  CHECK(test::sleeps == std::vector<std::int64_t>({cfg::RETRY_BASE_DELAY_NS, cfg::RETRY_BASE_DELAY_NS * 2}));
}
TEST(download_bounds_retries_for_each_transient_error) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  for (const auto error : {CURLE_COULDNT_RESOLVE_HOST, CURLE_COULDNT_CONNECT,
           CURLE_OPERATION_TIMEDOUT, CURLE_SSL_CONNECT_ERROR, CURLE_PEER_FAILED_VERIFICATION,
           CURLE_SSL_CACERT_BADFILE, CURLE_PARTIAL_FILE, CURLE_RECV_ERROR,
           CURLE_SEND_ERROR, CURLE_GOT_NOTHING}) {
    const auto before = test::requests.size();
    for (int i = 0; i < cfg::DOWNLOAD_ATTEMPTS; ++i) test::replies.push_back({error, 0});
    std::string message;
    CHECK(!install::download(curl.get(), "https://example.test/mod.zip", message));
    CHECK(test::requests.size() == before + cfg::DOWNLOAD_ATTEMPTS);
    CHECK(!message.empty());
  }
}
TEST(download_does_not_retry_permanent_transfer_http_or_empty_file_failures) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  for (const test::Reply &reply : std::vector<test::Reply>{
           {CURLE_WRITE_ERROR, 0}, {CURLE_UNSUPPORTED_PROTOCOL, 0},
           {CURLE_OK, 404, "not found"}, {CURLE_OK, 200, ""}}) {
    const auto before = test::requests.size();
    test::replies.push_back(reply);
    std::string message;
    CHECK(!install::download(curl.get(), "https://example.test/mod.zip", message));
    CHECK(test::requests.size() == before + 1);
    CHECK(!message.empty());
  }
  CHECK(test::sleeps.empty());
}
TEST(download_cancellation_prevents_new_attempts) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  install::cancel_requested = true;
  std::string message;
  CHECK(!install::download(curl.get(), "https://example.test/mod.zip", message));
  CHECK(message == "Cancelled.");
  CHECK(test::requests.empty());
  install::cancel_requested = false;
  test::replies.push_back({CURLE_RECV_ERROR, 0});
  test::on_sleep = [] { install::cancel_requested = true; };
  CHECK(!install::download(curl.get(), "https://example.test/mod.zip", message));
  CHECK(test::requests.size() == 1);
  CHECK(message == "Cancelled.");
}
TEST(download_rejects_reported_length_mismatch) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  test::Reply reply{CURLE_OK, 200, "archive"};
  reply.reported_bytes = 100;
  test::replies.push_back(reply);
  std::string message;
  CHECK(!install::download(curl.get(), "https://example.test/mod.zip", message));
  CHECK(message == "Download was interrupted.");
  CHECK(test::requests.size() == 1);
}
TEST(apply_persists_new_install_before_removing_obsolete_files) {
  Fixture fixture;
  original_install();
  install::apply(update());
  const auto *record = store::installed.find(7);
  CHECK(record != nullptr);
  CHECK(record->date == 200);
  CHECK(record->source_file_name == "new.zip");
  CHECK(record->files == update().files);
  CHECK(!test::exists(std::string(cfg::CTGP7_DIR) + "old.chpack"));
  // Case changes in archive names must not delete a file still supplied.
  CHECK(test::exists(std::string(cfg::CTGP7_DIR) + "Shared.chpack"));
  store::installed.clear();
  CHECK(store::load_installed());
  CHECK(store::installed.find(7)->date == 200);
}
TEST(apply_save_failure_restores_old_record_and_preserves_all_existing_files) {
  Fixture fixture;
  original_install();
  const auto disk = test::read(cfg::INSTALLED_FILE);
  test::fail_json_write = true;
  install::apply(update());
  const auto *record = store::installed.find(7);
  CHECK(record != nullptr);
  CHECK(record->date == 100);
  CHECK(record->source_file_name == "old.zip");
  CHECK(record->files == std::vector<std::string>({"old.chpack", "Shared.chpack"}));
  CHECK(test::read(std::string(cfg::CTGP7_DIR) + "old.chpack") == "old content");
  CHECK(test::read(std::string(cfg::CTGP7_DIR) + "Shared.chpack") == "shared content");
  CHECK(test::read(cfg::INSTALLED_FILE) == disk);
}
TEST(apply_new_install_save_failure_does_not_leave_phantom_registry_entry) {
  Fixture fixture;
  test::fail_json_write = true;
  install::apply(update());
  CHECK(!store::installed.contains(7));
  CHECK(!test::exists(cfg::INSTALLED_TMP));
}
TEST(uninstall_save_failure_rolls_back_marker_without_deleting_files) {
  Fixture fixture;
  original_install();
  select_installed();
  test::fail_json_write = true;
  install::confirm_uninstall();
  CHECK(!install::is_uninstalling());
  CHECK(!store::installed.find(7)->uninstalling);
  CHECK(test::exists(std::string(cfg::CTGP7_DIR) + "old.chpack"));
  CHECK(test::exists(std::string(cfg::CTGP7_DIR) + "Shared.chpack"));
}
TEST(uninstall_persists_marker_before_deletion_and_erases_record_on_completion) {
  Fixture fixture;
  original_install();
  select_installed();
  install::confirm_uninstall();
  CHECK(install::is_uninstalling());
  CHECK(store::installed.find(7)->uninstalling);
  CHECK(test::read(cfg::INSTALLED_FILE).find("\"Uninstalling\":true") != std::string::npos);
  CHECK(test::exists(std::string(cfg::CTGP7_DIR) + "old.chpack"));
  CHECK(install::tick_uninstall());
  CHECK(install::uninstall_done == 1);
  CHECK(install::percent == 50);
  CHECK(store::installed.contains(7));
  CHECK(install::tick_uninstall());
  CHECK(!store::installed.contains(7));
  CHECK(!install::is_uninstalling());
  CHECK(install::percent == -1);
  CHECK(!install::tick_uninstall());
  CHECK(store::load_installed());
  CHECK(!store::installed.contains(7));
}
TEST(tick_reports_completed_failure_and_then_no_change) {
  Fixture fixture;
  mm::model::queued_mod_ids = {7};
  install::finished = update();
  install::finished.ok = false;
  install::finished.message = "network failed";
  install::slot = install::Slot::COMPLETE;
  CHECK(install::tick());
  CHECK(install::user_message == "network failed");
  CHECK(install::slot == install::Slot::EMPTY);
  CHECK(mm::model::queued_mod_ids.empty());
  CHECK(!install::tick());
}
