#include "command_palette.h"
#include "search_engine.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>

CommandPalette g_command_palette;

void CommandPalette::open() {
  open_ = true;
  focus_input_ = true;
  selected_index_ = 0;
  std::memset(input_buf_, 0, sizeof(input_buf_));
  std::memset(ipc_input_buf_, 0, sizeof(ipc_input_buf_));
  ipc_history_pos_ = -1;
}

void CommandPalette::close() {
  open_ = false;
}

bool CommandPalette::is_open() const {
  return open_;
}

void CommandPalette::toggle() {
  if (open_)
    close();
  else
    open();
}

bool CommandPalette::handle_key(int keycode, bool ctrl, bool cmd) {
  bool modifier = false;
#ifdef __APPLE__
  modifier = cmd;
  (void)ctrl;
#else
  modifier = ctrl;
  (void)cmd;
#endif
  if (modifier && (keycode == 'k' || keycode == 'K')) {
    toggle();
    return true;
  }
  return false;
}

void CommandPalette::register_command(const std::string& name, const std::string& description,
                                      const std::string& shortcut, std::function<void()> action) {
  commands_.push_back({name, description, shortcut, std::move(action)});
}

void CommandPalette::clear_commands() {
  commands_.clear();
}

std::vector<const CommandEntry*> CommandPalette::filter_commands(const std::string& query) const {
  std::vector<std::pair<int, const CommandEntry*>> scored;
  for (const auto& cmd : commands_) {
    int name_score = search_detail::fuzzy_score(query, cmd.name);
    int desc_score = search_detail::fuzzy_score(query, cmd.description);
    int best = std::max(name_score, desc_score);
    if (best > 0) {
      scored.push_back({best, &cmd});
    }
  }
  std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  std::vector<const CommandEntry*> result;
  result.reserve(scored.size());
  for (const auto& s : scored) {
    result.push_back(s.second);
  }
  return result;
}

void CommandPalette::set_ipc_handler(IpcHandler handler) {
  ipc_handler_ = std::move(handler);
}

std::string CommandPalette::execute_ipc(const std::string& command) const {
  if (ipc_handler_) return ipc_handler_(command);
  return "ERR no IPC handler\n";
}

void CommandPalette::render() {
  if (!open_) return;

  ImGuiIO& io = ImGui::GetIO();
  ImVec2 display_size = io.DisplaySize;

  float palette_w = std::min(600.0f, display_size.x * 0.8f);
  float palette_h = std::min(400.0f, display_size.y * 0.7f);
  ImVec2 pos((display_size.x - palette_w) * 0.5f,
             display_size.y * 0.15f);
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(palette_w, palette_h), ImGuiCond_Always);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoScrollbar;

  ImGui::GetBackgroundDrawList()->AddRectFilled(
      ImVec2(0, 0), display_size,
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, 0.5f)));

  if (!ImGui::Begin("##CommandPalette", &open_, flags)) {
    ImGui::End();
    return;
  }

  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    close();
    ImGui::End();
    return;
  }

  if (ImGui::BeginTabBar("##PaletteModes")) {
    if (ImGui::BeginTabItem("Commands")) {
      mode_ = 0;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("IPC")) {
      mode_ = 1;
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  if (mode_ == 0) {
    if (focus_input_) {
      ImGui::SetKeyboardFocusHere();
      focus_input_ = false;
    }
    bool enter_pressed = ImGui::InputText("##CmdSearch", input_buf_, sizeof(input_buf_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);

    auto filtered = filter_commands(input_buf_);

    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
      selected_index_ = std::min(selected_index_ + 1,
                                  static_cast<int>(filtered.size()) - 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
      selected_index_ = std::max(selected_index_ - 1, 0);
    }

    if (enter_pressed && !filtered.empty() &&
        selected_index_ >= 0 && selected_index_ < static_cast<int>(filtered.size())) {
      auto action = filtered[static_cast<size_t>(selected_index_)]->action;
      close();
      if (action) action();
      ImGui::End();
      return;
    }

    ImGui::BeginChild("##CmdList", ImVec2(0, 0), ImGuiChildFlags_None);
    for (size_t i = 0; i < filtered.size(); i++) {
      const auto* cmd = filtered[i];
      bool is_selected = (static_cast<int>(i) == selected_index_);

      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable("##cmd", is_selected, 0, ImVec2(0, 24))) {
        auto action = cmd->action;
        close();
        if (action) action();
        ImGui::PopID();
        ImGui::EndChild();
        ImGui::End();
        return;
      }
      ImGui::SameLine();
      ImGui::Text("%s", cmd->name.c_str());
      if (!cmd->shortcut.empty()) {
        float shortcut_w = ImGui::CalcTextSize(cmd->shortcut.c_str()).x;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > shortcut_w) {
          ImGui::SameLine(ImGui::GetCursorPosX() + avail - shortcut_w);
          ImGui::TextDisabled("%s", cmd->shortcut.c_str());
        }
      }
      if (!cmd->description.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(" - %s", cmd->description.c_str());
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
  } else {
    if (focus_input_) {
      ImGui::SetKeyboardFocusHere();
      focus_input_ = false;
    }

    ImGuiInputTextFlags ipc_flags = ImGuiInputTextFlags_EnterReturnsTrue;
    bool ipc_enter = ImGui::InputText("##IpcInput", ipc_input_buf_, sizeof(ipc_input_buf_), ipc_flags);

    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !ipc_history_.empty()) {
      if (ipc_history_pos_ < 0) {
        ipc_history_pos_ = static_cast<int>(ipc_history_.size()) - 1;
      } else if (ipc_history_pos_ > 0) {
        ipc_history_pos_--;
      }
      std::strncpy(ipc_input_buf_,
                    ipc_history_[static_cast<size_t>(ipc_history_pos_)].c_str(),
                    sizeof(ipc_input_buf_) - 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !ipc_history_.empty()) {
      if (ipc_history_pos_ >= 0 &&
          ipc_history_pos_ < static_cast<int>(ipc_history_.size()) - 1) {
        ipc_history_pos_++;
        std::strncpy(ipc_input_buf_,
                      ipc_history_[static_cast<size_t>(ipc_history_pos_)].c_str(),
                      sizeof(ipc_input_buf_) - 1);
      } else {
        ipc_history_pos_ = -1;
        std::memset(ipc_input_buf_, 0, sizeof(ipc_input_buf_));
      }
    }

    if (ipc_enter && ipc_input_buf_[0] != '\0') {
      std::string cmd(ipc_input_buf_);
      ipc_history_.push_back(cmd);
      ipc_history_pos_ = -1;

      ipc_output_lines_.push_back("> " + cmd);
      std::string response = execute_ipc(cmd);
      while (!response.empty() && response.back() == '\n') response.pop_back();
      ipc_output_lines_.push_back(response);

      std::memset(ipc_input_buf_, 0, sizeof(ipc_input_buf_));
      ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::BeginChild("##IpcOutput", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const auto& line : ipc_output_lines_) {
      if (line.size() > 1 && line[0] == '>') {
        ImGui::TextColored(ImVec4(0.541f, 0.416f, 0.063f, 1.0f), "%s", line.c_str());
      } else {
        ImGui::TextWrapped("%s", line.c_str());
      }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
      ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
  }

  ImGui::End();
}
