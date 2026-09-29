#include "ipc_mru.h"

#include <mutex>
#include <utility>

#include "imgui_state.h"
#include "imgui_ui.h"
#include "imgui_ui_testable.h"

extern t_CPC CPC;

namespace {
std::mutex g_mru_mutex;
std::vector<std::pair<CpcMruList, std::string>> g_mru_entries;
}  // namespace

void ipc_mru_stage(CpcMruList list, const std::string& path) {
  std::scoped_lock const lock(g_mru_mutex);
  g_mru_entries.emplace_back(list, path);
}

void ipc_mru_apply_staged(bool save_config) {
  if (imgui_state.show_options) return;
  std::vector<std::pair<CpcMruList, std::string>> entries;
  {
    std::scoped_lock const lock(g_mru_mutex);
    entries.swap(g_mru_entries);
  }
  for (const auto& [list, path] : entries) {
    if (save_config) {
      imgui_mru_push(CPC.*list, path);
    } else {
      mru_list_push(CPC.*list, path, t_CPC::MRU_MAX);
    }
  }
}
