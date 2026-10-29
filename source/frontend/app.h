#pragma once
#include "core/system.h"
#include <3ds.h>
#include <citro3d.h>
#include "imgui.h"
#include "imgui_sw.h"
#include <string>
namespace mm {
namespace app {
/**
 * Load installed records and the cached list, then reset browsing and start its workers.
 * Return false if the list cannot be read or is empty; installed-record and worker
 * initialization failures do not affect the result. This does not change AppState.
 */
[[nodiscard]] bool enter_browse_state();
void run_search_dialog();
void advance_state(bool feed_done);
/**
 * Handle repeated navigation keys and newly pressed action keys while browsing.
 * About and uninstall-confirmation overlays take precedence; busy installs allow
 * queueing and cancellation, while active uninstall work disables those actions.
 */
void handle_browse_input(u32 nav_keys, u32 pressed);
class Platform {
 public:
  Platform() = default;
  Platform(const Platform &) = delete;
  Platform &operator=(const Platform &) = delete;
  ~Platform();
  [[nodiscard]] bool init();
  /**
   * Run the initialized platform's input, feed, install, and rendering loop.
   * Allow cached browsing during refresh, defer list replacement while installs or
   * confirmation are active, and redraw on changes. Return on applet exit or an
   * allowed START press; idle iterations sleep for 16 milliseconds.
   */
  void run();
 private:
  [[nodiscard]] bool init_screens();
  /** Mount ROMFS and record readiness for cleanup; return false if mounting fails. */
  [[nodiscard]] bool init_romfs();
  /**
   * Attempt audio startup, initialize network services and trust selection, then start the feed.
   * Return false on network setup or worker creation failure; audio failure is tolerated.
   * Partially initialized services remain available for shutdown to release.
   */
  [[nodiscard]] bool init_audio_and_network();
  /**
   * Cancel and join the previous feed worker, reset status, and start another.
   * force bypasses cache freshness. Return false and mark the feed failed if thread
   * creation fails; success means the worker started, not that refresh completed.
   */
  [[nodiscard]] bool start_feed(bool force);
  [[nodiscard]] float tick_clock() noexcept;
  void update_pointer(u32 held, u32 released) noexcept;
  void render(const std::string &status_line);
  void shutdown() noexcept;
  C3D_RenderTarget *top_ = nullptr;
  C3D_RenderTarget *bottom_ = nullptr;
  ImGuiIO *io_ = nullptr;
  imgui_sw::SwOptions sw_options_{};
  TickCounter frame_timer_{};
  sys::MallocArray<u32> soc_buffer_;
  sys::Thread feed_thread_;
  bool graphics_ready_ = false;
  bool imgui_ready_ = false;
  bool romfs_ready_ = false;
  bool soc_ready_ = false;
  bool sslc_ready_ = false;
  bool curl_ready_ = false;
};
}
}
