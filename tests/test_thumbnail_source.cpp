#include "support/test_support.h"
#include "backend/sd_card.h"
#include "backend/thumbnail_source.h"
#include <memory>

namespace {
std::string ca_path;
namespace thumbs = mm::thumbs;
struct Fixture {
  test::Sandbox sandbox;
  Fixture() {
    CHECK(mm::sd::init_paths());
    thumbs::quit_requested = false;
    ca_path = "old-cacert.pem";
  }
};
std::string jpeg() {
  std::vector<unsigned char> pixels(mm::cfg::THUMB_IMG_W * mm::cfg::THUMB_IMG_H * 3, 127);
  unsigned char *encoded = nullptr;
  unsigned long size = 0;
  CHECK(thumbs::jpeg_encode(pixels.data(), &encoded, &size));
  mm::sys::MallocArray<unsigned char> owner{encoded};
  return {reinterpret_cast<char *>(encoded), size};
}
}
namespace mm::net {
void configure(CURL *curl) noexcept {
  curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path.c_str());
}
}
TEST(thumbnail_download_reconfigures_reused_handle_after_ca_bundle_change) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  std::vector<u16> pixels(mm::cfg::THUMB_TEX_W * mm::cfg::THUMB_TEX_H);
  const auto image = jpeg();
  test::replies.push_back({CURLE_OK, 200, image});
  CHECK(thumbs::produce(curl.get(), 1, "https://example.test/one.jpg", pixels.data()));
  ca_path = "updated-cacert.pem";
  test::replies.push_back({CURLE_OK, 200, image});
  CHECK(thumbs::produce(curl.get(), 2, "https://example.test/two.jpg", pixels.data()));
  CHECK(test::requests.size() == 2);
  CHECK(test::requests[0].ca == "old-cacert.pem");
  CHECK(test::requests[1].ca == "updated-cacert.pem");
}
TEST(thumbnail_cancelled_request_does_not_transfer_or_write_cache) {
  Fixture fixture;
  mm::sys::CurlHandle curl;
  std::vector<u16> pixels(mm::cfg::THUMB_TEX_W * mm::cfg::THUMB_TEX_H);
  thumbs::quit_requested = true;
  CHECK(!thumbs::produce(curl.get(), 3, "https://example.test/three.jpg", pixels.data()));
  CHECK(test::requests.empty());
  CHECK(!test::exists(thumbs::cache_path(3)));
}
