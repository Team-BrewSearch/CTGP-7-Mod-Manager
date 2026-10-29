#include "backend/sd_card.h"
#include "core/config.h"
#include "core/status.h"
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <utility>
namespace mm {
namespace sd {
sys::RecursiveMutex path_lock;
std::string join(std::string_view directory, std::string_view name) {
  std::string out;
  out.reserve(directory.size() + name.size() + 1);
  out.assign(directory);
  if (!out.empty() && out.back() != '/') {
    out.push_back('/');
  }
  out.append(name);
  return out;
}
bool make_directories(std::string_view path) {
  const PathGuard guard{path_lock};
  std::string full{path};
  constexpr std::string_view DEVICE = "sdmc:/";
  const std::size_t start = full.starts_with(DEVICE) ? DEVICE.size() : 0;
  for (std::size_t i = start; i < full.size(); ++i) {
    if (full[i] != '/') {
      continue;
    }
    const std::string component = full.substr(0, i);
    if (::mkdir(component.c_str(), 0777) != 0 && errno != EEXIST) {
      status::print("mkdir failed:{}", component);
      return false;
    }
  }
  if (::mkdir(full.c_str(), 0777) != 0 && errno != EEXIST) {
    status::print("mkdir failed:{}", full);
    return false;
  }
  return true;
}
void unlink_quietly(const char *path) noexcept {
  const PathGuard guard{path_lock};
  ::unlink(path);
}
bool replace_file(const char *source, const char *destination) noexcept {
  const PathGuard guard{path_lock};
  if (::rename(source, destination) == 0) {
    return true;
  }
  const std::string temp = std::string(destination) + ".replacing";
  sys::FileHandle input{std::fopen(source, "rb")};
  if (!input) {
    return false;
  }
  sys::FileHandle output{std::fopen(temp.c_str(), "wb")};
  if (!output) {
    return false;
  }
  std::array<char, 4096> buffer{};
  bool ok = true;
  for (;;) {
    const std::size_t bytes_read = std::fread(buffer.data(), 1, buffer.size(), input.get());
    if (bytes_read > 0 && std::fwrite(buffer.data(), 1, bytes_read, output.get()) != bytes_read) {
      ok = false;
      break;
    }
    if (bytes_read < buffer.size()) {
      if (std::ferror(input.get()) != 0) {
        ok = false;
      }
      break;
    }
  }
  if (ok && output.get() != nullptr) {
    (void)std::fflush(output.get());
    (void)fsync(fileno(output.get()));
  }
  const bool written = output.close();
  (void)input.close();
  if (!ok || !written) {
    ::unlink(temp.c_str());
    return false;
  }
  if (::rename(temp.c_str(), destination) == 0) {
    ::unlink(source);
    return true;
  }
  if (::unlink(destination) == 0 || errno == ENOENT) {
    if (::rename(temp.c_str(), destination) == 0) {
      ::unlink(source);
      return true;
    }
  }
  ::unlink(temp.c_str());
  return false;
}
std::optional<int64_t> file_size(const char *path) noexcept {
  const PathGuard guard{path_lock};
  struct stat info{};
  if (::stat(path, &info) != 0) {
    return std::nullopt;
  }
  if (info.st_size < 0) {
    return std::nullopt;
  }
  return static_cast<int64_t>(info.st_size);
}
std::optional<int64_t> file_mtime(const char *path) noexcept {
  const PathGuard guard{path_lock};
  struct stat info{};
  if (::stat(path, &info) != 0 || info.st_mtime <= 0) {
    return std::nullopt;
  }
  return static_cast<int64_t>(info.st_mtime);
}
sys::FileHandle open(const char *path, const char *mode) noexcept {
  const PathGuard guard{path_lock};
  return sys::FileHandle{std::fopen(path, mode)};
}
bool init_paths() {
  return make_directories(cfg::APP_DIR) &&
         make_directories(cfg::LISTS_DIR) &&
         make_directories(cfg::THUMB_DIR) &&
         make_directories(cfg::CTGP7_DIR);
}
}
}
