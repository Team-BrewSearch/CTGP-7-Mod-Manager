#include "support/test_support.h"
#include "core/config.h"
#include "frontend/model.h"

namespace mm::store { Registry installed; }
namespace {
namespace model = mm::model;
using mm::store::ModData;
struct Fixture {
  Fixture() {
    mm::store::installed.clear();
    model::all_mods.clear();
    model::mods.clear();
    model::queued_mod_ids.clear();
    model::search_query.clear();
    model::window_start = model::selected = 0;
    model::sort_by_name = true;
    model::sort_reversed = false;
    model::cards_dirty = false;
  }
};
ModData mod(int id, std::string name, std::string author = {}) {
  ModData result;
  result.id = id;
  result.name = std::move(name);
  result.author = std::move(author);
  return result;
}
std::vector<int> ids() {
  std::vector<int> result;
  for (const auto &entry : model::mods) result.push_back(entry.id);
  return result;
}
}
TEST(search_matches_name_and_author_without_case_sensitivity) {
  Fixture fixture;
  model::all_mods = {mod(1, "LuIgI", "Alice"), mod(2, "Mario", "LUIGI fan"), mod(3, "Peach")};
  model::apply_search("lUiGi");
  CHECK(ids() == std::vector<int>({1, 2}));
  CHECK(model::all_mods.size() == 3);
  CHECK(model::search_query == "lUiGi");
}
TEST(search_handles_empty_long_and_nonmatching_queries) {
  Fixture fixture;
  model::all_mods = {mod(1, "Mario", "Alice")};
  for (const std::string query : {"Mario-longer", "nobody", " "}) {
    model::apply_search(query);
    CHECK(model::mods.empty());
    CHECK(model::selected_mod() == nullptr);
  }
  model::apply_search("");
  CHECK(ids() == std::vector<int>({1}));
}
TEST(replace_all_reapplies_active_search_to_new_records) {
  Fixture fixture;
  model::search_query = "ALICE";
  model::queued_mod_ids = {7};
  model::window_start = 30;
  model::selected = 5;
  model::replace_all({mod(1, "Peach", "alice"), mod(2, "Mario", "Bob")});
  CHECK(ids() == std::vector<int>({1}));
  CHECK(model::all_mods.size() == 2);
  CHECK(model::search_query == "ALICE");
  CHECK(model::queued_mod_ids == std::vector<int>({7}));
  CHECK(model::window_start == 0);
  CHECK(model::selected == 0);
  CHECK(model::cards_dirty);
}
TEST(replace_all_respects_sort_mode_and_resets_selection) {
  Fixture fixture;
  model::sort_reversed = true;
  model::window_start = model::selected = 3;
  model::replace_all({mod(1, "Alpha"), mod(2, "Zulu")});
  CHECK(ids() == std::vector<int>({2, 1}));
  CHECK(model::window_start == 0);
  CHECK(model::selected == 0);
  model::replace_all({});
  CHECK(model::mods.empty());
  CHECK(model::all_mods.empty());
  CHECK(model::selected_mod() == nullptr);
}
TEST(select_mod_id_keeps_each_target_visible_at_grid_boundaries) {
  Fixture fixture;
  for (int id = 1; id <= 14; ++id) model::mods.push_back(mod(id, "name"));
  for (int id : {1, 3, 4, 6, 7, 12, 14}) {
    model::select_mod_id(id);
    CHECK(model::selected_mod() != nullptr);
    CHECK(model::selected_mod()->id == id);
    CHECK(model::window_start % mm::cfg::GRID_COLS == 0);
    CHECK(model::window_start <= model::max_window_start());
    CHECK(model::selected < model::visible_count());
  }
}
TEST(select_missing_id_clamps_invalid_view_instead_of_leaving_stale_selection) {
  Fixture fixture;
  model::mods = {mod(1, "only")};
  for (int id : {0, 999}) {
    model::window_start = 99;
    model::selected = 99;
    model::cards_dirty = false;
    model::select_mod_id(id);
    CHECK(model::window_start == 0);
    CHECK(model::selected == 0);
    CHECK(model::cards_dirty);
  }
  model::mods.clear();
  model::select_mod_id(1);
  CHECK(model::selected_mod() == nullptr);
}
