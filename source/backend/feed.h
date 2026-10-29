#pragma once
#include "backend/store.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>
namespace mm {
namespace feed {
using store::ModData;
struct PageResult {
  std::vector<ModData> mods;
  int record_count = -1;
  bool ok = false;
};
struct PageJob {
  int category_id = 0;
  int page = 1;
  bool sequential = false;
};
extern std::atomic<std::size_t> core_batch_cap;
extern std::atomic<bool> quit_requested;
void parse_index_records(json_object *records, std::vector<ModData> &out);
/**
 * Append visible records from a one-based category page to out and update its metadata.
 * Return false on cancellation, request failure, or unparseable JSON; existing
 * contents of out are retained on those failures.
 */
[[nodiscard]] bool fetch_index_page(CURL *curl, int category_id, int page, PageResult &out);
void collect_pages(const std::vector<PageJob> &jobs, std::vector<PageResult> &results);
void deduplicate(std::vector<ModData> &mods);
/**
 * Read a core-data row, accepting up to four array wrappers and a file dictionary.
 * Fill an unknown category from a wrapper and select the valid file with the
 * largest positive Unix timestamp in seconds. Return false if no object can be
 * unwrapped; true does not imply that a valid file was found. Category changes
 * made while unwrapping can remain even when false is returned.
 */
[[nodiscard]] bool parse_latest_file(json_object *raw_item, ModData &mod);
void lower_core_cap(std::size_t limit) noexcept;
/**
 * Enrich mods without download URLs with file and category data until canceled.
 * Split failed batches and honor the URL length and shared batch limits; size
 * rejections lower the shared limit. Failed lookups can leave mods unresolved.
 */
void fetch_core_range(CURL *curl, std::span<ModData> mods);
void fetch_core_data(std::vector<ModData> &mods);
void drain_pages(std::vector<PageResult> &results, std::vector<ModData> &out);
[[nodiscard]] std::vector<PageJob> plan_remaining_pages(const std::vector<PageResult> &first_pass);
/**
 * Fetch, deduplicate, enrich, and save the mod list, excluding configured categories.
 * Return false on cancellation, no initial records, or a failed save. Individual
 * request failures can still yield a successful, partially resolved list.
 */
[[nodiscard]] bool run_pipeline();
/**
 * Refresh the feed and publish its completion status; a non-null argument forces refresh.
 * Otherwise reuse a current cache younger than 24 hours. CA refresh failure does
 * not prevent fetching, and failed feed refreshes retain the cached timestamp.
 */
void thread_main(void *);
}
}
