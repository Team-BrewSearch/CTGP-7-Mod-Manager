#include "backend/store.h"
#include "backend/json.h"
#include "backend/sd_card.h"
#include "core/config.h"
#include "core/format.h"
#include "core/log.h"
#include "core/system.h"
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
namespace mm {
namespace store {
Registry installed;
bool write_mod_list(const char *path, const std::vector<ModData> &mods) {
  sys::JsonRef array{json_object_new_array()};
  if (!array) {
    return false;
  }
  sys::JsonRef header{json_object_new_object()};
  if (!header ||
      !js::add_field(header.get(), "CacheVersion", json_object_new_int(cfg::MOD_LIST_VERSION)) ||
      json_object_array_add(array.get(), header.get()) != 0) {
    return false;
  }
  (void)header.release();
  for (const ModData &mod : mods) {
    sys::JsonRef entry{json_object_new_object()};
    if (!entry) {
      return false;
    }
    const bool built =
        js::add_field(entry.get(), "Id", json_object_new_int(mod.id)) &&
        js::add_field(entry.get(), "Name", json_object_new_string(mod.name.c_str())) &&
        js::add_field(entry.get(), "Author", json_object_new_string(mod.author.c_str())) &&
        js::add_field(entry.get(), "ThumbnailUrl",
                      json_object_new_string(mod.thumbnail_url.c_str())) &&
        js::add_field(entry.get(), "LatestFileUrl",
                      json_object_new_string(mod.latest_file_url.c_str())) &&
        js::add_field(entry.get(), "LatestFileDate",
                      json_object_new_int64(mod.latest_file_date)) &&
        js::add_field(entry.get(), "LatestFileName",
                      json_object_new_string(mod.latest_file_name.c_str()));
    if (!built) {
      return false;
    }
    if (json_object_array_add(array.get(), entry.get()) != 0) {
      return false;
    }
    (void)entry.release();
  }
  const std::string temporary = std::string(path) + ".tmp";
  if (!js::write_file(temporary.c_str(), array.get())) {
    sd::unlink_quietly(temporary.c_str());
    log_event("Mod list: temporary write failed");
    return false;
  }
  if (!sd::replace_file(temporary.c_str(), path)) {
    sd::unlink_quietly(temporary.c_str());
    log_event("Mod list: replace failed");
    return false;
  }
  return true;
}
bool read_mod_list(const char *path, std::vector<ModData> &out) {
  out.clear();
  const sys::JsonRef root = js::read_file(path);
  if (!root || !js::is(root.get(), json_type_array)) {
    return false;
  }
  const js::ArrayView records{root.get()};
  out.reserve(records.size());
  for (json_object *record : records) {
    if (!js::is(record, json_type_object)) {
      continue;
    }
    ModData mod;
    mod.id = js::integer_field<int>(record, "Id");
    if (mod.id == 0) {
      continue;
    }
    mod.name = js::string_or_empty(record, "Name");
    mod.author = js::string_or_empty(record, "Author");
    mod.thumbnail_url = js::string_or_empty(record, "ThumbnailUrl");
    mod.latest_file_url = js::string_or_empty(record, "LatestFileUrl");
    mod.latest_file_name = js::string_or_empty(record, "LatestFileName");
    mod.latest_file_date = js::integer_field<int64_t>(record, "LatestFileDate");
    out.push_back(std::move(mod));
  }
  return true;
}
bool mod_list_version_ok() {
  sys::FileHandle file = sd::open(cfg::MOD_LIST_FILE.data(), "rb");
  if (!file) {
    return false;
  }
  std::array<char, 128> head{};
  const std::size_t read = std::fread(head.data(), 1, head.size(), file.get());
  const std::string_view text{head.data(), read};
  const std::size_t key = text.find("\"CacheVersion\"");
  if (key == std::string_view::npos) {
    return false;
  }
  std::size_t at = key + std::string_view{"\"CacheVersion\""}.size();
  const auto skip_space = [&text](std::size_t &pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                 text[pos] == '\r' || text[pos] == '\n')) {
      ++pos;
    }
  };
  skip_space(at);
  if (at >= text.size() || text[at] != ':') {
    return false;
  }
  ++at;
  skip_space(at);
  int version = 0;
  const std::size_t digits = at;
  while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
    version = version * 10 + (text[at] - '0');
    ++at;
  }
  if (at == digits) {
    return false;
  }
  skip_space(at);
  return version == cfg::MOD_LIST_VERSION && at < text.size() &&
         (text[at] == ',' || text[at] == '}');
}
bool load_installed() {
  installed.clear();
  sys::JsonRef root = js::read_file(cfg::INSTALLED_TMP.data());
  if (!root) {
    root = js::read_file(cfg::INSTALLED_FILE.data());
  }
  if (!root) {
    sd::unlink_quietly(cfg::INSTALLED_TMP.data());
    return true;
  }
  if (!js::is(root.get(), json_type_object)) {
    sd::unlink_quietly(cfg::INSTALLED_TMP.data());
    return false;
  }
  json_object_object_foreach(root.get(), key, value) {
    if (key == nullptr || !js::is(value, json_type_object)) {
      continue;
    }
    const auto id = js::parse_integer<int>(std::string_view{key});
    if (!id || *id <= 0) {
      continue;
    }
    InstallRecord record;
    record.date = js::integer_field<int64_t>(value, "Date");
    record.source_file_name = js::string_or_empty(value, "SourceFileName");
    record.uninstalling = js::boolean_field(value, "Uninstalling", false);
    for (json_object *file : js::array_field(value, "Files")) {
      if (!js::is(file, json_type_string)) {
        continue;
      }
      const char *name = json_object_get_string(file);
      if (name != nullptr && name[0] != '\0') {
        record.files.emplace_back(name);
      }
    }
    installed.insert_or_assign(*id, std::move(record));
  }
  sd::unlink_quietly(cfg::INSTALLED_TMP.data());
  std::vector<int> erased;
  for (Registry::Entry &entry : installed.entries()) {
    if (!entry.record.uninstalling) {
      continue;
    }
    for (const std::string &file : entry.record.files) {
      sd::unlink_quietly(fmt::format("{}{}", cfg::CTGP7_DIR, file).c_str());
    }
    erased.push_back(entry.id);
  }
  for (const int id : erased) {
    installed.erase(id);
  }
  if (!erased.empty() && !save_installed()) {
    return false;
  }
  return true;
}
bool save_installed() {
  sys::JsonRef root{json_object_new_object()};
  if (!root) {
    return false;
  }
  for (const Registry::Entry &entry : installed.entries()) {
    sys::JsonRef record{json_object_new_object()};
    if (!record) {
      return false;
    }
    json_object *files = json_object_new_array();
    if (!js::add_field(record.get(), "Files", files)) {
      return false;
    }
    for (const std::string &file : entry.record.files) {
      json_object *item = json_object_new_string(file.c_str());
      if (item == nullptr) {
        return false;
      }
      if (json_object_array_add(files, item) != 0) {
        json_object_put(item);
        return false;
      }
    }
    if (!js::add_field(record.get(), "Date",
                       json_object_new_int64(entry.record.date)) ||
        !js::add_field(record.get(), "SourceFileName",
                       json_object_new_string(entry.record.source_file_name.c_str())) ||
        !js::add_field(record.get(), "Uninstalling",
                       json_object_new_boolean(entry.record.uninstalling))) {
      return false;
    }
    const std::string key = fmt::format("{}", entry.id);
    if (!js::add_field(root.get(), key.c_str(), record.get())) {
      return false;
    }
    (void)record.release();
  }
  const std::string tmp = cfg::INSTALLED_TMP.data();
  const std::string permanent = cfg::INSTALLED_FILE.data();
  if (!js::write_file(tmp.c_str(), root.get())) {
    sd::unlink_quietly(tmp.c_str());
    return false;
  }
  if (!sd::replace_file(tmp.c_str(), permanent.c_str())) {
    sd::unlink_quietly(tmp.c_str());
    return false;
  }
  sd::unlink_quietly(tmp.c_str());
  return true;
}
}
}
