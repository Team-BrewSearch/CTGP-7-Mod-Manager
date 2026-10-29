#pragma once
#include "backend/store.h"
#include "frontend/model.h"
#include "imgui.h"
#include <string>
#include <string_view>
#include <vector>
namespace mm {
namespace draw {
[[nodiscard]] float measure_imgui(const char *content);
void text_centered(const char *content, const ImVec4 &color, float y);
/** Display and log content, adding retry and exit hints when failed is true. */
void bottom_status(const std::string &content, bool failed);
[[nodiscard]] std::string action_label(model::ModAction action, const store::ModData *mod);
void wrap_lines(std::string_view content, float wrap_width, int max_lines,
                std::vector<std::string> &out);
[[nodiscard]] bool wrapped_button(const char *id, const std::string &label, float y,
                                  float height, const ImVec4 &background,
                                  const ImVec4 &background_hot, const ImVec4 &foreground,
                                  float border);
void draw_sort_options();
void draw_progress_bar();
/**
 * Draw browsing controls, progress, and feed age, or the active modal overlay.
 * Handle activated touch controls, including install actions, search, and overlay changes.
 */
void bottom_browse();
}
}
