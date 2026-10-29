#include "support/test_support.h"
#include "backend/net.h"
#include "backend/sd_card.h"
#include "core/config.h"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <numeric>

namespace {
namespace net = mm::net;
namespace cfg = mm::cfg;
constexpr const char *URL = "https://example.test/resource";
// Synthetic PEM-shaped data, deliberately not a real trust anchor.
std::string bundle(std::size_t size = cfg::CA_BUNDLE_MIN_SIZE) {
  std::string data;
  for (int i = 0; i < 10; ++i) data += "-----BEGIN CERTIFICATE-----\nA\n-----END CERTIFICATE-----\n";
  data.insert(data.rfind("-----END CERTIFICATE-----"), size - data.size(), 'A');
  return data;
}
constexpr const char *HASH = "0fc2457e4c53f17de326bde2f756e757b46ac19c498a5a5f0085380b89c94e1d";
struct Fixture {
  test::Sandbox sandbox;
  Fixture() { net::failed_requests = 0; net::init_ca_bundle(); }
};
void queue_bundle(std::string body = bundle(), std::string hash = HASH) {
  test::replies.push_back({CURLE_OK, 200, std::move(body), {"ETag: \"bundle-v2\"\r\n"}});
  test::replies.push_back({CURLE_OK, 200, std::move(hash) + "  cacert.pem\n"});
}
void cached_bundle() {
  test::write(cfg::CA_BUNDLE_SD, bundle());
  net::init_ca_bundle();
}
}
TEST(get_accepts_only_2xx_and_discards_error_bodies) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  CHECK(bool(curl));
  net::configure(curl.get());
  for (long status : {199L, 200L, 204L, 299L, 300L, 404L, 500L}) {
    test::replies.push_back({CURLE_OK, status, "response"});
    const auto result = net::get(curl.get(), URL);
    CHECK(result.ok == (status >= 200 && status < 300));
    CHECK(result.status_code == status);
    CHECK(result.data == (result.ok ? "response" : ""));
  }
  CHECK(net::failed_requests == 4);
}
TEST(get_does_not_start_for_null_handle_or_precancelled_request) {
  Fixture fixture;
  CHECK(!net::get(nullptr, URL).ok);
  mm::sys::CurlHandle curl;
  std::atomic<bool> cancel{true};
  CHECK(!net::get(curl.get(), URL, &cancel).ok);
  CHECK(test::requests.empty());
  CHECK(net::failed_requests == 0);
}
TEST(cancellation_during_transfer_does_not_increment_failure_counter) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  net::configure(curl.get());
  std::atomic<bool> cancel{false};
  test::replies.push_back({CURLE_OK, 200, "ignored", {}, [&] { cancel = true; }});
  CHECK(!net::get(curl.get(), URL, &cancel).ok);
  CHECK(net::failed_requests == 0);
  cancel = false;
  test::replies.push_back({CURLE_RECV_ERROR, 0});
  CHECK(!net::get(curl.get(), URL, &cancel).ok);
  CHECK(net::failed_requests == 1);
}
TEST(reused_handle_can_disable_previous_cancellation_callback) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  net::configure(curl.get());
  std::atomic<bool> cancel{false};
  test::replies.push_back({CURLE_OK, 200, "one"});
  CHECK(net::get(curl.get(), URL, &cancel).ok);
  cancel = true;
  test::replies.push_back({CURLE_OK, 200, "two"});
  CHECK(net::get(curl.get(), URL).data == "two");
}
TEST(retry_uses_exponential_delays_capped_at_thirty_seconds) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  net::configure(curl.get());
  for (int i = 0; i < 9; ++i) test::replies.push_back({CURLE_COULDNT_CONNECT, 0});
  test::replies.push_back({CURLE_OK, 200, "success"});
  std::map<std::size_t, std::int64_t> delays;
  test::on_sleep = [&] { delays[test::requests.size()] += test::sleeps.back(); };
  CHECK(net::get_with_retry(curl.get(), URL, 10).data == "success");
  CHECK(test::requests.size() == 10);
  CHECK(net::failed_requests == 9);
  std::int64_t expected = cfg::RETRY_BASE_DELAY_NS;
  for (std::size_t retry = 1; retry < 10; ++retry) {
    CHECK(delays[retry] == expected);
    expected = std::min<std::int64_t>(expected * 2, 30'000'000'000LL);
  }
  CHECK(std::ranges::all_of(test::sleeps, [](auto delay) { return delay <= 250'000'000; }));
}
TEST(retry_stops_on_size_errors_and_honors_attempt_limit) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  net::configure(curl.get());
  for (long status : {400L, 413L, 414L}) {
    test::replies.push_back({CURLE_OK, status, "too big"});
    CHECK(net::get_with_retry(curl.get(), URL, 4).status_code == status);
  }
  CHECK(test::requests.size() == 3);
  CHECK(test::sleeps.empty());
  for (int i = 0; i < 3; ++i) test::replies.push_back({CURLE_OK, 500});
  CHECK(!net::get_with_retry(curl.get(), URL, 3).ok);
  CHECK(test::requests.size() == 6);
  CHECK(!net::get_with_retry(curl.get(), URL, 0).ok);
  CHECK(!net::get_with_retry(curl.get(), URL, -1).ok);
  CHECK(test::requests.size() == 6);
}
TEST(throttled_responses_add_throttle_delay_before_retry) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  net::configure(curl.get());
  for (long status : {429L, 503L}) {
    test::sleeps.clear();
    test::replies.push_back({CURLE_OK, status});
    test::replies.push_back({CURLE_OK, 200, "ok"});
    CHECK(net::get_with_retry(curl.get(), URL, 2).ok);
    CHECK(std::accumulate(test::sleeps.begin(), test::sleeps.end(), std::int64_t{0}) ==
          cfg::THROTTLE_DELAY_NS + cfg::RETRY_BASE_DELAY_NS);
  }
}
TEST(cancel_during_backoff_stops_after_first_sleep_slice) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  net::configure(curl.get());
  std::atomic<bool> cancel{false};
  test::replies.push_back({CURLE_COULDNT_CONNECT, 0});
  test::on_sleep = [&] { cancel = true; };
  CHECK(!net::get_with_retry(curl.get(), URL, 4, &cancel).ok);
  CHECK(test::requests.size() == 1);
  CHECK(test::sleeps == std::vector<std::int64_t>({50'000'000}));
}
TEST(ca_initialization_falls_back_to_romfs_then_recovers_temporary_bundle) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  net::configure(curl.get());
  CHECK(test::transfers.at(curl.get()).ca == cfg::CA_BUNDLE_ROMFS);
  test::write(cfg::CA_BUNDLE_SD, "broken");
  test::write(cfg::CA_BUNDLE_SD_TMP, bundle());
  net::init_ca_bundle();
  net::configure(curl.get());
  CHECK(test::transfers.at(curl.get()).ca == cfg::CA_BUNDLE_SD_TMP);
}
TEST(ca_refresh_verifies_known_digest_and_atomically_caches_etag) {
  Fixture fixture;
  queue_bundle();
  CHECK(net::refresh_ca_bundle());
  CHECK(test::read(cfg::CA_BUNDLE_SD) == bundle());
  CHECK(test::read(cfg::CA_BUNDLE_ETAG) == "32768\n\"bundle-v2\"\n");
  CHECK(!test::exists(cfg::CA_BUNDLE_SD_TMP));
  CHECK(test::requests.size() == 2);
  CHECK(test::requests[0].verify_peer == 0);
  CHECK(test::requests[1].url == cfg::CA_BUNDLE_HASH_URL);
  CHECK(test::requests[1].verify_peer == 1);
  CHECK(test::requests[1].verify_host == 2);
  test::replies.push_back({CURLE_OK, 304});
  CHECK(!net::refresh_ca_bundle());
  CHECK(test::requests.back().headers == std::vector<std::string>({"If-None-Match: \"bundle-v2\""}));
  CHECK(test::requests.back().verify_peer == 1);
  CHECK(test::read(cfg::CA_BUNDLE_SD) == bundle());
}
TEST(ca_rejects_malformed_hashes_and_digest_mismatch_without_touching_cache) {
  Fixture fixture;
  cached_bundle();
  for (const std::string hash : {std::string(64, '0'), std::string(64, 'z'), std::string(63, 'a')}) {
    queue_bundle(bundle(), hash);
    CHECK(!net::refresh_ca_bundle());
    CHECK(test::read(cfg::CA_BUNDLE_SD) == bundle());
    CHECK(!test::exists(cfg::CA_BUNDLE_SD_TMP));
  }
}
TEST(ca_bootstrap_requires_hash_fetch_and_keeps_romfs_on_failure) {
  Fixture fixture;
  test::replies.push_back({CURLE_OK, 200, bundle()});
  test::replies.push_back({CURLE_COULDNT_CONNECT, 0});
  CHECK(!net::refresh_ca_bundle());
  CHECK(!test::exists(cfg::CA_BUNDLE_SD));
  mm::sys::CurlHandle curl;
  net::configure(curl.get());
  CHECK(test::transfers.at(curl.get()).ca == cfg::CA_BUNDLE_ROMFS);
}
TEST(ca_certificate_failure_retries_unverified_but_still_requires_hash) {
  Fixture fixture;
  cached_bundle();
  test::replies.push_back({CURLE_PEER_FAILED_VERIFICATION, 0});
  queue_bundle();
  CHECK(net::refresh_ca_bundle());
  CHECK(test::requests.size() == 3);
  CHECK(test::requests[0].verify_peer == 1);
  CHECK(test::requests[1].verify_peer == 0);
  CHECK(test::requests[2].verify_peer == 1);
  test::replies.push_back({CURLE_SSL_CACERT_BADFILE, 0});
  test::replies.push_back({CURLE_OK, 200, bundle()});
  test::replies.push_back({CURLE_COULDNT_CONNECT, 0});
  test::replies.push_back({CURLE_COULDNT_CONNECT, 0}); // Shipped-anchor hash retry.
  CHECK(!net::refresh_ca_bundle());
  CHECK(test::read(cfg::CA_BUNDLE_SD) == bundle());
}
TEST(ca_rejects_implausible_size_markers_and_too_few_certificates) {
  Fixture fixture;
  cached_bundle();
  for (const std::string &body : std::vector<std::string>{
           bundle(cfg::CA_BUNDLE_MIN_SIZE - 1), std::string(cfg::CA_BUNDLE_MIN_SIZE, 'A'),
           "-----BEGIN CERTIFICATE-----" + std::string(cfg::CA_BUNDLE_MIN_SIZE, 'A') + "-----END CERTIFICATE-----"}) {
    queue_bundle(body);
    CHECK(!net::refresh_ca_bundle());
    CHECK(test::read(cfg::CA_BUNDLE_SD) == bundle());
  }
}
TEST(ca_does_not_send_stale_size_mismatched_or_invalid_etags) {
  Fixture fixture;
  cached_bundle();
  for (const std::string text : {"123\n\"stale\"\n", "x\n\"bad\"", "32768\n \r\n", "32768"}) {
    test::write(cfg::CA_BUNDLE_ETAG, text);
    test::replies.push_back({CURLE_OK, 304});
    CHECK(!net::refresh_ca_bundle());
    CHECK(test::requests.back().headers.empty());
  }
}
TEST(ca_cancel_before_or_during_download_preserves_existing_bundle) {
  Fixture fixture;
  cached_bundle();
  std::atomic<bool> cancel{true};
  CHECK(!net::refresh_ca_bundle(&cancel));
  CHECK(test::requests.empty());
  cancel = false;
  test::replies.push_back({CURLE_OK, 200, bundle(), {}, [&] { cancel = true; }});
  CHECK(!net::refresh_ca_bundle(&cancel));
  CHECK(test::requests.size() == 1);
  CHECK(test::read(cfg::CA_BUNDLE_SD) == bundle());
}
TEST(ca_accepts_uppercase_hash_and_case_insensitive_etag_header) {
  Fixture fixture;
  std::string hash = HASH;
  for (char &digit : hash) if (digit >= 'a' && digit <= 'f') digit -= 'a' - 'A';
  test::replies.push_back({CURLE_OK, 200, bundle(), {"eTaG: \tW/\"version\" \r\n"}});
  test::replies.push_back({CURLE_OK, 200, hash + "\n"});
  CHECK(net::refresh_ca_bundle());
  CHECK(test::read(cfg::CA_BUNDLE_ETAG) == "32768\nW/\"version\"\n");
}
TEST(ca_bootstrap_rejects_empty_hash_response_instead_of_installing_unverified_data) {
  Fixture fixture;
  test::replies.push_back({CURLE_OK, 200, bundle()});
  test::replies.push_back({CURLE_OK, 200, ""});
  CHECK(!net::refresh_ca_bundle());
  CHECK(!test::exists(cfg::CA_BUNDLE_SD));
}
TEST(ca_uses_verified_temporary_file_when_atomic_replacement_fails) {
  Fixture fixture;
  cached_bundle();
  test::rename_errors = {EACCES, EACCES};
  queue_bundle();
  CHECK(net::refresh_ca_bundle());
  CHECK(test::read(cfg::CA_BUNDLE_SD_TMP) == bundle());
  net::init_ca_bundle();
  mm::sys::CurlHandle curl;
  net::configure(curl.get());
  CHECK(test::transfers.at(curl.get()).ca == cfg::CA_BUNDLE_SD_TMP);
}
TEST(ca_hash_retry_uses_shipped_anchor_with_tls_verification_enabled) {
  Fixture fixture;
  cached_bundle();
  test::replies.push_back({CURLE_OK, 200, bundle()});
  test::replies.push_back({CURLE_PEER_FAILED_VERIFICATION, 0});
  test::replies.push_back({CURLE_OK, 200, HASH});
  CHECK(net::refresh_ca_bundle());
  CHECK(test::requests.size() == 3);
  CHECK(test::requests[1].ca == cfg::CA_BUNDLE_SD);
  CHECK(test::requests[2].url == cfg::CA_BUNDLE_HASH_URL);
  CHECK(test::requests[2].ca == cfg::CA_BUNDLE_ROMFS);
  CHECK(test::requests[2].verify_peer == 1);
  CHECK(test::requests[2].verify_host == 2);
}
TEST(ca_cancellation_during_shipped_anchor_retry_preserves_cached_bundle) {
  Fixture fixture;
  cached_bundle();
  std::atomic<bool> cancel{false};
  test::replies.push_back({CURLE_OK, 200, bundle()});
  test::replies.push_back({CURLE_COULDNT_CONNECT, 0});
  test::replies.push_back({CURLE_OK, 200, HASH, {}, [&] { cancel = true; }});
  CHECK(!net::refresh_ca_bundle(&cancel));
  CHECK(test::requests.size() == 3);
  CHECK(test::read(cfg::CA_BUNDLE_SD) == bundle());
  CHECK(!test::exists(cfg::CA_BUNDLE_SD_TMP));
}
