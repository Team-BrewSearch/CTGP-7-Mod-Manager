#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
namespace mm {
namespace store {
struct ModData {
  int id = 0;
  int category_id = 0;
  std::string name;
  std::string author;
  std::string thumbnail_url;
  std::string latest_file_url;
  int64_t latest_file_date = 0;
  std::string latest_file_name;
};
struct InstallRecord {
  int64_t date = 0;
  std::vector<std::string> files;
  std::string source_file_name;
  bool uninstalling = false;
};
class Registry {
 public:
  struct Entry {
    int id = 0;
    InstallRecord record;
  };
  [[nodiscard]] const InstallRecord *find(int id) const noexcept {
    std::scoped_lock guard{mutex_};
    const auto it = lower_bound(id);
    if (it == entries_.end() || it->id != id) {
      return nullptr;
    }
    return &it->record;
  }
  /**
   * Return a borrowed mutable record, or null if id is absent.
   * The lock is released before return; callers must coordinate access and avoid
   * retaining the pointer across registry mutations that invalidate it.
   */
  [[nodiscard]] InstallRecord *find(int id) noexcept {
    std::scoped_lock guard{mutex_};
    const auto it = lower_bound(id);
    if (it == entries_.end() || it->id != id) {
      return nullptr;
    }
    return &it->record;
  }
  [[nodiscard]] bool contains(int id) const noexcept {
    return find(id) != nullptr;
  }
  void insert_or_assign(int id, InstallRecord record) {
    std::scoped_lock guard{mutex_};
    const auto it = lower_bound(id);
    if (it != entries_.end() && it->id == id) {
      it->record = std::move(record);
      return;
    }
    entries_.insert(it, Entry{id, std::move(record)});
  }
  bool erase(int id) {
    std::scoped_lock guard{mutex_};
    const auto it = lower_bound(id);
    if (it == entries_.end() || it->id != id) {
      return false;
    }
    entries_.erase(it);
    return true;
  }
  void clear() noexcept {
    std::scoped_lock guard{mutex_};
    entries_.clear();
  }
  [[nodiscard]] std::size_t size() const noexcept {
    std::scoped_lock guard{mutex_};
    return entries_.size();
  }
   [[nodiscard]] const std::vector<Entry> &entries() const noexcept {
     std::scoped_lock guard{mutex_};
     return entries_;
   }
   /**
    * Return the registry storage without holding its lock after return.
    * Callers must coordinate access and preserve ascending, unique IDs.
    */
   [[nodiscard]] std::vector<Entry> &entries() noexcept {
     std::scoped_lock guard{mutex_};
     return entries_;
   }
 private:
  [[nodiscard]] std::vector<Entry>::iterator lower_bound(int id) noexcept {
    return std::lower_bound(entries_.begin(), entries_.end(), id,
                            [](const Entry &entry, int key) {
                              return entry.id < key;
                            });
  }
  [[nodiscard]] std::vector<Entry>::const_iterator lower_bound(int id) const noexcept {
    return std::lower_bound(entries_.begin(), entries_.end(), id,
                            [](const Entry &entry, int key) {
                              return entry.id < key;
                            });
  }
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
};
extern Registry installed;
/**
 * Save mods with a cache-version header through path.tmp, then replace path.
 * Return false on JSON construction, write, or replacement failure. category_id
 * is not persisted; failed writes or replacements attempt to remove the temp file.
 */
[[nodiscard]] bool write_mod_list(const char *path, const std::vector<ModData> &mods);
[[nodiscard]] bool read_mod_list(const char *path, std::vector<ModData> &out);
/**
 * Check for the expected CacheVersion in the first 128 bytes of the configured list.
 * Return false if missing, unreadable, or mismatched; this does not validate the full list.
 */
[[nodiscard]] bool mod_list_version_ok();
/**
 * Clear and load the registry, preferring parseable temporary JSON over the main file.
 * Attempt deletion of files marked uninstalling, erase their records, and save the
 * cleanup; deletion and cleanup-save errors are ignored. Remove the temporary file.
 * Return true with an empty registry when neither file parses; return false when
 * the chosen JSON is not an object.
 */
[[nodiscard]] bool load_installed();
/**
 * Save installed records, including uninstall markers, through a temporary file.
 * Return false on JSON construction, write, or replacement failure; failed writes
 * and replacements attempt to remove the temporary file.
 */
[[nodiscard]] bool save_installed();
}
}
