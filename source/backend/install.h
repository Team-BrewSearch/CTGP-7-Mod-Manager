#pragma once
#include "backend/store.h"
#include "core/system.h"
#include <curl/curl.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
namespace mm {
namespace install {
extern std::atomic<bool> quit_requested;
extern std::atomic<bool> cancel_requested;
extern std::atomic<int> percent;
extern std::atomic<int64_t> bytes_done;
extern std::atomic<int> files_written;
extern std::vector<std::string> uninstall_files;
extern std::atomic<int> uninstall_done;
extern std::atomic<bool> uninstalling;
extern std::atomic<int> uninstall_mod_id;
extern std::atomic<bool> uninstall_pending;
[[nodiscard]] bool aborting() noexcept;
struct DownloadSink {
  std::FILE *file = nullptr;
  curl_off_t written = 0;
};
std::size_t download_write(void *contents, std::size_t size, std::size_t nmemb, void *userp);
int download_progress(void *, curl_off_t download_total, curl_off_t download_now,
                      curl_off_t, curl_off_t);
void configure_download(CURL *curl) noexcept;
/**
 * Download an archive to DOWNLOAD_TMP using a valid curl handle, restarting each retry.
 * Retry eligible transport errors up to DOWNLOAD_ATTEMPTS with exponential waits.
 * Cancellation, file errors, invalid HTTP responses, and content-length failures
 * return false with a user message; a successful retry need not clear that message.
 */
[[nodiscard]] bool download(CURL *curl, const std::string &url, std::string &message);
enum class Phase {
  IDLE,
  DOWNLOADING,
  EXTRACTING,
  FINISHING
};
enum class Slot {
  EMPTY,
  REQUESTED,
  RUNNING,
  COMPLETE
};
struct Request {
  int mod_id = 0;
  int64_t file_date = 0;
  std::string url;
  std::string source_name;
};
struct Result {
  int mod_id = 0;
  int64_t file_date = 0;
  std::string source_name;
  std::vector<std::string> files;
  bool ok = false;
  std::string message;
};
extern sys::Mutex mailbox_lock;
extern sys::Event wake;
extern sys::Thread worker;
extern Slot slot;
extern Request pending;
extern Result finished;
extern std::atomic<Phase> phase;
extern bool ready;
extern std::string user_message;
[[nodiscard]] bool busy();
[[nodiscard]] bool installing(int mod_id) noexcept;
[[nodiscard]] bool is_uninstalling() noexcept;
[[nodiscard]] bool is_uninstall_pending() noexcept;
void cancel();
/**
 * Process pending and queued downloads and extraction until quit is requested.
 * Publish each result for tick to apply, removing the temporary archive afterward.
 */
void worker_main(void *);
[[nodiscard]] bool begin(const store::ModData &mod);
[[nodiscard]] bool queue_mod(const store::ModData &mod);
[[nodiscard]] bool queue_selected_mod();
/**
 * Record a completed extraction, save it, then remove obsolete files and resort the view.
 * If saving fails, restore the previous registry entry and keep obsolete files.
 * Already extracted files are not rolled back; file deletion errors are ignored.
 */
void apply(const Result &result);
/**
 * Advance uninstall work and consume a completed install result on the UI thread.
 * Apply successful extractions or expose their failure message. Return whether
 * uninstall progress changed or a result was consumed; false if not initialized.
 */
[[nodiscard]] bool tick();
[[nodiscard]] std::string progress_label();
void do_action();
void uninstall();
void request_uninstall();
/**
 * Clear confirmation and start removing the selected installed mod when idle.
 * Persist its uninstall marker before scheduling deletion; a failed save restores
 * the marker and aborts. A record with no files is erased and saved immediately;
 * a failed removal save reports a warning. Resort the view after registry changes.
 */
void confirm_uninstall();
void cancel_uninstall_pending();
void begin_uninstall(const std::vector<std::string> &files, int mod_id);
/**
 * Attempt one batch of file removals and update uninstall progress.
 * On completion, erase the registry entry and attempt to save it. Removal and save
 * errors are ignored. Return whether progress changed or uninstall completed.
 */
[[nodiscard]] bool tick_uninstall();
bool init();
void shutdown();
}
}
