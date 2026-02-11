#pragma once

#include <functional>
#include <string>
#include <vector>

struct CommandEntry {
  std::string name;
  std::string description;
  std::string shortcut;
  std::function<void()> action;
};

class CommandPalette {
public:
  void open();
  void close();
  bool is_open() const;
  void toggle();

  // Called each frame from ImGui render loop
  void render();

  // Check for Cmd/Ctrl+K shortcut. Returns true if palette toggled.
  bool handle_key(int keycode, bool ctrl, bool cmd);

  // Register a command
  void register_command(const std::string& name, const std::string& description,
                        const std::string& shortcut, std::function<void()> action);

  // Clear all registered commands
  void clear_commands();

  // Get filtered commands based on query (for testing)
  std::vector<const CommandEntry*> filter_commands(const std::string& query) const;

  // Execute IPC command and return response (for testing)
  using IpcHandler = std::function<std::string(const std::string&)>;
  void set_ipc_handler(IpcHandler handler);
  std::string execute_ipc(const std::string& command) const;

  // Access commands (for testing)
  const std::vector<CommandEntry>& commands() const { return commands_; }

private:
  bool open_ = false;
  int mode_ = 0; // 0 = Commands, 1 = IPC
  char input_buf_[512] = {};
  int selected_index_ = 0;
  bool focus_input_ = false;

  // IPC mode state
  std::vector<std::string> ipc_history_;
  std::vector<std::string> ipc_output_lines_;
  int ipc_history_pos_ = -1;
  char ipc_input_buf_[512] = {};

  std::vector<CommandEntry> commands_;
  IpcHandler ipc_handler_;
};

// Global command palette instance
extern CommandPalette g_command_palette;
