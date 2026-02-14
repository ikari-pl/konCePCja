#include "devtools_ui.h"
#include "imgui_ui_testable.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "koncepcja.h"
#include "z80.h"
#include "z80_disassembly.h"
#include "symfile.h"
#include "data_areas.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

extern t_CPC CPC;
extern t_z80regs z80;

// Reject paths containing ".." to prevent path traversal
static bool has_path_traversal(const char* path)
{
  for (const auto& comp : std::filesystem::path(path)) {
    if (comp == "..") return true;
  }
  return false;
}

DevToolsUI g_devtools_ui;

// -----------------------------------------------
// Name-to-pointer mapping helpers
// -----------------------------------------------

bool* DevToolsUI::window_ptr(const std::string& name)
{
  if (name == "registers")    return &show_registers_;
  if (name == "disassembly")  return &show_disassembly_;
  if (name == "memory_hex")   return &show_memory_hex_;
  if (name == "stack")        return &show_stack_;
  if (name == "breakpoints")  return &show_breakpoints_;
  if (name == "symbols")      return &show_symbols_;
  if (name == "data_areas")   return &show_data_areas_;
  if (name == "disasm_export") return &show_disasm_export_;
  return nullptr;
}

void DevToolsUI::toggle_window(const std::string& name)
{
  bool* p = window_ptr(name);
  if (p) *p = !*p;
}

bool DevToolsUI::is_window_open(const std::string& name) const
{
  if (name == "registers")    return show_registers_;
  if (name == "disassembly")  return show_disassembly_;
  if (name == "memory_hex")   return show_memory_hex_;
  if (name == "stack")        return show_stack_;
  if (name == "breakpoints")  return show_breakpoints_;
  if (name == "symbols")      return show_symbols_;
  if (name == "data_areas")   return show_data_areas_;
  if (name == "disasm_export") return show_disasm_export_;
  return false;
}

bool DevToolsUI::any_window_open() const
{
  return show_registers_ || show_disassembly_ || show_memory_hex_ ||
         show_stack_ || show_breakpoints_ || show_symbols_ ||
         show_data_areas_ || show_disasm_export_;
}

void DevToolsUI::navigate_disassembly(word addr)
{
  show_disassembly_ = true;
  disasm_follow_pc_ = false;
  disasm_goto_value_ = addr;
  snprintf(disasm_goto_addr_, sizeof(disasm_goto_addr_), "%04X", addr);
}

// -----------------------------------------------
// Main render dispatch
// -----------------------------------------------

void DevToolsUI::render()
{
  if (show_registers_)    render_registers();
  if (show_disassembly_)  render_disassembly();
  if (show_memory_hex_)   render_memory_hex();
  if (show_stack_)        render_stack();
  if (show_breakpoints_)  render_breakpoints();
  if (show_symbols_)      render_symbols();
  if (show_data_areas_)   render_data_areas();
  if (show_disasm_export_) render_disasm_export();
}

// -----------------------------------------------
// Debug Window 1: Registers
// -----------------------------------------------

void DevToolsUI::render_registers()
{
  ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(620, 30), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Registers", &open)) {
    if (!open) show_registers_ = false;
    ImGui::End();
    return;
  }

  bool locked = !CPC.paused;
  ImGuiInputTextFlags hex_flags = ImGuiInputTextFlags_CharsHexadecimal |
    (locked ? ImGuiInputTextFlags_ReadOnly : 0);

  auto RegField16 = [&](const char* label, reg_pair& rp) {
    unsigned short val = rp.w.l;
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputScalar(label, ImGuiDataType_U16, &val, nullptr, nullptr, "%04X", hex_flags)) {
      if (!locked) rp.w.l = val;
    }
  };

  auto RegField8 = [&](const char* label, byte& val) {
    ImGui::SetNextItemWidth(40);
    unsigned char v = val;
    if (ImGui::InputScalar(label, ImGuiDataType_U8, &v, nullptr, nullptr, "%02X", hex_flags)) {
      if (!locked) val = v;
    }
  };

  // Main registers in two columns
  ImGui::Columns(2, "regs_main", false);
  RegField16("AF", z80.AF); ImGui::NextColumn();
  RegField16("AF'", z80.AFx); ImGui::NextColumn();
  RegField16("BC", z80.BC); ImGui::NextColumn();
  RegField16("BC'", z80.BCx); ImGui::NextColumn();
  RegField16("DE", z80.DE); ImGui::NextColumn();
  RegField16("DE'", z80.DEx); ImGui::NextColumn();
  RegField16("HL", z80.HL); ImGui::NextColumn();
  RegField16("HL'", z80.HLx); ImGui::NextColumn();
  RegField16("IX", z80.IX); ImGui::NextColumn();
  RegField16("IY", z80.IY); ImGui::NextColumn();
  RegField16("SP", z80.SP); ImGui::NextColumn();
  RegField16("PC", z80.PC); ImGui::NextColumn();
  ImGui::Columns(1);

  ImGui::Spacing();
  RegField8("I", z80.I);
  ImGui::SameLine();
  RegField8("R", z80.R);

  // Interrupt state
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("IFF1: %d  IFF2: %d  IM: %d  HALT: %d",
              z80.IFF1, z80.IFF2, z80.IM, z80.HALT);
  ImGui::Text("T-states: %llu", static_cast<unsigned long long>(g_tstate_counter));

  // Flags
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("Flags");
  byte f = z80.AF.b.l;
  bool s = f & Sflag, zf = f & Zflag, h = f & Hflag, pv = f & Pflag, n = f & Nflag, cf = f & Cflag;
  ImGui::Checkbox("S", &s);   ImGui::SameLine();
  ImGui::Checkbox("Z", &zf);  ImGui::SameLine();
  ImGui::Checkbox("H", &h);   ImGui::SameLine();
  ImGui::Checkbox("P/V", &pv); ImGui::SameLine();
  ImGui::Checkbox("N", &n);   ImGui::SameLine();
  ImGui::Checkbox("C", &cf);
  if (!locked) {
    byte new_f = 0;
    if (s)  new_f |= Sflag;
    if (zf) new_f |= Zflag;
    if (h)  new_f |= Hflag;
    if (pv) new_f |= Pflag;
    if (n)  new_f |= Nflag;
    if (cf) new_f |= Cflag;
    z80.AF.b.l = new_f | (f & Xflags);
  }

  if (!open) show_registers_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 2: Disassembly
// -----------------------------------------------

void DevToolsUI::render_disassembly()
{
  ImGui::SetNextWindowSize(ImVec2(440, 500), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Disassembly", &open, ImGuiWindowFlags_MenuBar)) {
    if (!open) show_disassembly_ = false;
    ImGui::End();
    return;
  }

  // Toolbar
  if (ImGui::BeginMenuBar()) {
    ImGui::Checkbox("Follow PC", &disasm_follow_pc_);
    ImGui::Separator();
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputText("Goto", disasm_goto_addr_,
        sizeof(disasm_goto_addr_),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
      unsigned long addr;
      if (parse_hex(disasm_goto_addr_, &addr, 0xFFFF)) {
        disasm_goto_value_ = static_cast<int>(addr);
        disasm_follow_pc_ = false;
      }
    }
    ImGui::EndMenuBar();
  }

  // Determine the center address
  word center_pc = z80.PC.w.l;
  if (!disasm_follow_pc_ && disasm_goto_value_ >= 0) {
    center_pc = static_cast<word>(disasm_goto_value_);
  }

  // Disassemble ~48 instructions starting from an estimated position before center
  word start_addr = center_pc - 40;
  constexpr int NUM_LINES = 48;

  DisassembledCode dummy_dc;
  std::vector<dword> dummy_eps;
  struct DisasmEntry {
    word addr;
    std::string text;
    std::string label;
  };
  std::vector<DisasmEntry> lines;
  lines.reserve(NUM_LINES);

  word addr = start_addr;
  for (int i = 0; i < NUM_LINES; i++) {
    DisasmEntry entry;
    entry.addr = addr;

    // Check for symbol at this address
    entry.label = g_symfile.lookupAddr(addr);

    auto line = disassemble_one(addr, dummy_dc, dummy_eps);
    entry.text = line.instruction_;
    int len = line.Size();
    if (len <= 0) len = 1;
    addr = (addr + len) & 0xFFFF;
    lines.push_back(std::move(entry));
  }

  const auto& breakpoints = z80_list_breakpoints_ref();

  if (ImGui::BeginChild("##disasm_scroll", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    int scroll_to_idx = -1;

    for (int i = 0; i < static_cast<int>(lines.size()); i++) {
      const auto& entry = lines[i];
      bool is_pc = (entry.addr == z80.PC.w.l);
      bool is_bp = false;
      for (const auto& bp : breakpoints) {
        if (bp.address == entry.addr && bp.type != EPHEMERAL) { is_bp = true; break; }
      }

      // Symbol label above the instruction
      if (!entry.label.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
        ImGui::Text("  %s:", entry.label.c_str());
        ImGui::PopStyleColor();
      }

      // Build display text
      char label[160];
      snprintf(label, sizeof(label), "%s %04X  %s",
               is_bp ? "\xe2\x97\x8f" : " ",
               entry.addr, entry.text.c_str());

      // Color: green for PC, red for breakpoint
      if (is_pc) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
      } else if (is_bp) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
      }

      if (ImGui::Selectable(label, is_pc)) {
        // Left click: toggle breakpoint
        if (is_bp)
          z80_del_breakpoint(entry.addr);
        else
          z80_add_breakpoint(entry.addr);
      }

      // Right-click context menu
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Run to here")) {
          z80_add_breakpoint_ephemeral(entry.addr);
          CPC.paused = false;
        }
        if (ImGui::MenuItem("Set PC here")) {
          z80.PC.w.l = entry.addr;
        }
        if (ImGui::MenuItem("Goto this address")) {
          disasm_goto_value_ = entry.addr;
          disasm_follow_pc_ = false;
          snprintf(disasm_goto_addr_, sizeof(disasm_goto_addr_),
                   "%04X", entry.addr);
        }
        ImGui::EndPopup();
      }

      if (is_pc || is_bp) ImGui::PopStyleColor();

      // Track the PC line for auto-scroll
      if (is_pc) scroll_to_idx = i;
    }

    // Auto-scroll to PC line
    if (disasm_follow_pc_ && scroll_to_idx >= 0) {
      float item_height = ImGui::GetTextLineHeightWithSpacing();
      ImGui::SetScrollY(scroll_to_idx * item_height - ImGui::GetWindowHeight() * 0.3f);
    }
  }
  ImGui::EndChild();

  if (!open) show_disassembly_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 3: Memory Hex Dump
// -----------------------------------------------

void DevToolsUI::render_memory_hex()
{
  ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(460, 30), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Memory Hex", &open, ImGuiWindowFlags_MenuBar)) {
    if (!open) show_memory_hex_ = false;
    ImGui::End();
    return;
  }

  // Toolbar
  if (ImGui::BeginMenuBar()) {
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputText("Goto##memhex", memhex_goto_addr_,
        sizeof(memhex_goto_addr_),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
      unsigned long addr;
      if (parse_hex(memhex_goto_addr_, &addr, 0xFFFF)) {
        memhex_goto_value_ = static_cast<int>(addr);
      }
    }
    ImGui::Separator();
    ImGui::Text("W:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40);
    int bpr = memhex_bytes_per_row_;
    if (ImGui::InputInt("##bpr", &bpr, 0, 0)) {
      if (bpr >= 4 && bpr <= 32) memhex_bytes_per_row_ = bpr;
    }
    ImGui::EndMenuBar();
  }

  int bytes_per_row = memhex_bytes_per_row_;
  int total_rows = (0x10000 + bytes_per_row - 1) / bytes_per_row;

  // Collect watchpoint ranges for highlighting
  const auto& watchpoints = z80_list_watchpoints_ref();

  if (ImGui::BeginChild("##hexview", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    // Use clipper for efficient scrolling over all 64K
    ImGuiListClipper clipper;
    clipper.Begin(total_rows);

    // Handle goto
    if (memhex_goto_value_ >= 0) {
      int target_row = memhex_goto_value_ / bytes_per_row;
      float item_height = ImGui::GetTextLineHeightWithSpacing();
      ImGui::SetScrollY(target_row * item_height);
      memhex_goto_value_ = -1;
    }

    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
        unsigned int base_addr = row * bytes_per_row;

        // Address
        ImGui::Text("%04X:", base_addr & 0xFFFF);

        // Hex bytes with watchpoint highlighting
        for (int col = 0; col < bytes_per_row; col++) {
          word a = (base_addr + col) & 0xFFFF;
          byte val = z80_read_mem(a);

          ImGui::SameLine();

          // Check watchpoint highlighting
          bool wp_r = false, wp_w = false;
          for (const auto& wp : watchpoints) {
            if (wp.length > 0 && a >= wp.address && a < wp.address + wp.length) {
              if (wp.type == READ || wp.type == READWRITE) wp_r = true;
              if (wp.type == WRITE || wp.type == READWRITE) wp_w = true;
            }
          }

          bool colored = wp_r || wp_w;
          if (wp_r && wp_w) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.2f, 1.0f));
          } else if (wp_w) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
          } else if (wp_r) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
          }

          ImGui::Text("%02X", val);

          if (colored) ImGui::PopStyleColor();
        }

        // ASCII column
        ImGui::SameLine();
        char ascii[33];
        int asc_len = bytes_per_row < 32 ? bytes_per_row : 32;
        for (int col = 0; col < asc_len; col++) {
          byte b = z80_read_mem((base_addr + col) & 0xFFFF);
          ascii[col] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
        }
        ascii[asc_len] = '\0';
        ImGui::Text("|%s|", ascii);
      }
    }
    clipper.End();
  }
  ImGui::EndChild();

  if (!open) show_memory_hex_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 4: Stack
// -----------------------------------------------

void DevToolsUI::render_stack()
{
  ImGui::SetNextWindowSize(ImVec2(260, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(460, 440), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Stack", &open)) {
    if (!open) show_stack_ = false;
    ImGui::End();
    return;
  }

  word sp = z80.SP.w.l;
  ImGui::Text("SP = %04X", sp);
  ImGui::Separator();

  if (ImGui::BeginChild("##stack_entries", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    constexpr int MAX_DEPTH = 32;
    for (int i = 0; i < MAX_DEPTH; i++) {
      word addr = (sp + i * 2) & 0xFFFF;
      byte lo = z80_read_mem(addr);
      byte hi = z80_read_mem((addr + 1) & 0xFFFF);
      word value = (hi << 8) | lo;

      // Heuristic: check if the instruction before `value` is a CALL or RST
      bool is_ret_addr = false;
      if (value >= 3) {
        byte op3 = z80_read_mem((value - 3) & 0xFFFF);
        if (op3 == 0xCD || (op3 & 0xC7) == 0xC4) is_ret_addr = true;
      }
      if (value >= 1 && !is_ret_addr) {
        byte op1 = z80_read_mem((value - 1) & 0xFFFF);
        if ((op1 & 0xC7) == 0xC7) is_ret_addr = true;
      }

      // Look up symbol
      std::string sym = g_symfile.lookupAddr(value);

      if (is_ret_addr) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
      }

      std::string line;
      if (!sym.empty()) {
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "SP+%02X: %04X %s", i * 2, value,
                 is_ret_addr ? "[call] " : "");
        line = std::string(prefix) + sym;
      } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "SP+%02X: %04X%s",
                 i * 2, value, is_ret_addr ? "  [call]" : "");
        line = buf;
      }

      // Double-click navigates disassembly
      if (ImGui::Selectable(line.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(0)) {
          navigate_disassembly(value);
        }
      }

      if (is_ret_addr) ImGui::PopStyleColor();
    }
  }
  ImGui::EndChild();

  if (!open) show_stack_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 5: Breakpoint / Watchpoint List
// -----------------------------------------------

void DevToolsUI::render_breakpoints()
{
  ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(10, 540), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Breakpoints & Watchpoints", &open)) {
    if (!open) show_breakpoints_ = false;
    ImGui::End();
    return;
  }

  // Clear all buttons
  if (ImGui::Button("Clear All BPs")) z80_clear_breakpoints();
  ImGui::SameLine();
  if (ImGui::Button("Clear All WPs")) z80_clear_watchpoints();
  ImGui::SameLine();
  if (ImGui::Button("Clear All IOBPs")) z80_clear_io_breakpoints();
  ImGui::Separator();

  const auto& bps = z80_list_breakpoints_ref();
  const auto& wps = z80_list_watchpoints_ref();
  const auto& iobps = z80_list_io_breakpoints_ref();

  if (ImGui::BeginTable("bpwp_table", 5,
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("Address/Port", ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn("Condition", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 20);
    ImGui::TableHeadersRow();

    // Breakpoints
    for (size_t i = 0; i < bps.size(); i++) {
      const auto& bp = bps[i];
      if (bp.type == EPHEMERAL) continue;

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("BP");
      ImGui::TableSetColumnIndex(1);
      std::string sym = g_symfile.lookupAddr(bp.address);
      if (!sym.empty())
        ImGui::Text("%04X %s", bp.address, sym.c_str());
      else
        ImGui::Text("%04X", bp.address);
      ImGui::TableSetColumnIndex(2);
      if (!bp.condition_str.empty())
        ImGui::TextUnformatted(bp.condition_str.c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%d", bp.hit_count);
      ImGui::TableSetColumnIndex(4);
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::SmallButton("X")) {
        z80_del_breakpoint(bp.address);
      }
      ImGui::PopID();
    }

    // Watchpoints
    for (size_t i = 0; i < wps.size(); i++) {
      const auto& wp = wps[i];
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      const char* wp_type = (wp.type == READ) ? "WP/R" :
                            (wp.type == WRITE) ? "WP/W" : "WP/RW";
      ImGui::Text("%s", wp_type);
      ImGui::TableSetColumnIndex(1);
      if (wp.length > 1)
        ImGui::Text("%04X+%d", static_cast<word>(wp.address), wp.length);
      else
        ImGui::Text("%04X", static_cast<word>(wp.address));
      ImGui::TableSetColumnIndex(2);
      if (!wp.condition_str.empty())
        ImGui::TextUnformatted(wp.condition_str.c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%d", wp.hit_count);
      ImGui::TableSetColumnIndex(4);
      ImGui::PushID(1000 + static_cast<int>(i));
      if (ImGui::SmallButton("X")) {
        z80_del_watchpoint(static_cast<int>(i));
      }
      ImGui::PopID();
    }

    // IO Breakpoints
    for (size_t i = 0; i < iobps.size(); i++) {
      const auto& iobp = iobps[i];
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      const char* dir_str = (iobp.dir == IO_IN) ? "IO/IN" :
                            (iobp.dir == IO_OUT) ? "IO/OUT" : "IO/RW";
      ImGui::Text("%s", dir_str);
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%04X/%04X", iobp.port, iobp.mask);
      ImGui::TableSetColumnIndex(2);
      if (!iobp.condition_str.empty())
        ImGui::TextUnformatted(iobp.condition_str.c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("-");
      ImGui::TableSetColumnIndex(4);
      ImGui::PushID(2000 + static_cast<int>(i));
      if (ImGui::SmallButton("X")) {
        z80_del_io_breakpoint(static_cast<int>(i));
      }
      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  // Add Watchpoint form
  ImGui::Spacing();
  if (ImGui::CollapsingHeader("Add Watchpoint")) {
    ImGui::SetNextItemWidth(60);
    ImGui::InputText("Addr##wp", wp_addr_, sizeof(wp_addr_),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40);
    ImGui::InputText("Len##wp", wp_len_, sizeof(wp_len_),
                     ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    const char* wp_types[] = { "Read", "Write", "R/W" };
    ImGui::Combo("Type##wp", &wp_type_, wp_types, 3);
    ImGui::SameLine();
    if (ImGui::Button("Add WP")) {
      unsigned long addr;
      if (parse_hex(wp_addr_, &addr, 0xFFFF)) {
        long len_val = std::strtol(wp_len_, nullptr, 10);
        int len = (len_val > 0 && len_val <= 0xFFFF) ? static_cast<int>(len_val) : 1;
        WatchpointType wt = (wp_type_ == 0) ? READ :
                            (wp_type_ == 1) ? WRITE : READWRITE;
        z80_add_watchpoint(static_cast<word>(addr), static_cast<word>(len), wt);
        wp_addr_[0] = '\0';
      }
    }
  }

  // Add IO Breakpoint form
  if (ImGui::CollapsingHeader("Add IO Breakpoint")) {
    ImGui::SetNextItemWidth(60);
    ImGui::InputText("Port##iobp", iobp_port_, sizeof(iobp_port_),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputText("Mask##iobp", iobp_mask_, sizeof(iobp_mask_),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    const char* iobp_dirs[] = { "IN", "OUT", "Both" };
    ImGui::Combo("Dir##iobp", &iobp_dir_, iobp_dirs, 3);
    ImGui::SameLine();
    if (ImGui::Button("Add IOBP")) {
      unsigned long port, mask;
      if (parse_hex(iobp_port_, &port, 0xFFFF) &&
          parse_hex(iobp_mask_, &mask, 0xFFFF)) {
        IOBreakpointDir dir = (iobp_dir_ == 0) ? IO_IN :
                              (iobp_dir_ == 1) ? IO_OUT : IO_BOTH;
        z80_add_io_breakpoint(static_cast<word>(port),
                              static_cast<word>(mask), dir);
        iobp_port_[0] = '\0';
      }
    }
  }

  if (!open) show_breakpoints_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 6: Symbol Table Viewer
// -----------------------------------------------

void DevToolsUI::render_symbols()
{
  auto syms = g_symfile.listSymbols(symtable_filter_);

  char title[64];
  snprintf(title, sizeof(title), "Symbols (%d)###SymbolTable", static_cast<int>(syms.size()));

  ImGui::SetNextWindowSize(ImVec2(340, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(520, 540), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin(title, &open)) {
    if (!open) show_symbols_ = false;
    ImGui::End();
    return;
  }

  // Load / Save buttons
  if (ImGui::Button("Load .sym")) {
    if (sym_path_[0] != '\0' && !has_path_traversal(sym_path_)) {
      Symfile loaded(sym_path_);
      for (const auto& [addr, name] : loaded.Symbols()) {
        g_symfile.addSymbol(addr, name);
      }
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Save .sym")) {
    if (sym_path_[0] != '\0' && !has_path_traversal(sym_path_)) {
      g_symfile.SaveTo(sym_path_);
    }
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##sympath", "Symbol file path...", sym_path_,
                           sizeof(sym_path_));

  // Add Symbol form
  ImGui::SetNextItemWidth(60);
  ImGui::InputText("Addr##addsym", sym_addr_, sizeof(sym_addr_),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(150);
  ImGui::InputText("Name##addsym", sym_name_, sizeof(sym_name_));
  ImGui::SameLine();
  if (ImGui::Button("Add##addsym")) {
    unsigned long addr;
    if (parse_hex(sym_addr_, &addr, 0xFFFF) && sym_name_[0] != '\0') {
      g_symfile.addSymbol(static_cast<word>(addr), sym_name_);
      sym_addr_[0] = '\0';
      sym_name_[0] = '\0';
    }
  }

  ImGui::Separator();
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##symfilter", "Filter...", symtable_filter_,
                           sizeof(symtable_filter_));
  ImGui::Separator();

  if (ImGui::BeginTable("sym_table", 3,
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
      ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 20);
    ImGui::TableHeadersRow();

    for (const auto& [addr, name] : syms) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Selectable(nullptr, false, ImGuiSelectableFlags_SpanAllColumns)) {
        // Navigate disassembly to this address
        navigate_disassembly(addr);
      }
      ImGui::SameLine();
      ImGui::Text("%04X", addr);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(name.c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::PushID(static_cast<int>(addr));
      if (ImGui::SmallButton("X")) {
        g_symfile.delSymbol(name);
      }
      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  if (!open) show_symbols_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 7: Data Areas
// -----------------------------------------------

void DevToolsUI::render_data_areas()
{
  ImGui::SetNextWindowSize(ImVec2(450, 350), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Data Areas", &open)) {
    if (!open) show_data_areas_ = false;
    ImGui::End();
    return;
  }

  if (ImGui::Button("Clear All")) g_data_areas.clear_all();
  ImGui::Separator();

  // Mark form
  ImGui::SetNextItemWidth(60);
  ImGui::InputText("Start##da", da_start_, sizeof(da_start_),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(60);
  ImGui::InputText("End##da", da_end_, sizeof(da_end_),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80);
  const char* da_types[] = { "Bytes", "Words", "Text" };
  ImGui::Combo("Type##da", &da_type_, da_types, 3);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(100);
  ImGui::InputText("Label##da", da_label_, sizeof(da_label_));
  ImGui::SameLine();
  if (ImGui::Button("Mark")) {
    unsigned long s, e;
    if (parse_hex(da_start_, &s, 0xFFFF) && parse_hex(da_end_, &e, 0xFFFF) && s <= e) {
      DataType dt = (da_type_ == 0) ? DataType::BYTES :
                    (da_type_ == 1) ? DataType::WORDS : DataType::TEXT;
      g_data_areas.mark(static_cast<uint16_t>(s), static_cast<uint16_t>(e), dt,
                        da_label_[0] ? da_label_ : "");
      da_start_[0] = '\0';
      da_end_[0] = '\0';
      da_label_[0] = '\0';
    }
  }
  ImGui::Separator();

  // List data areas in a table
  auto areas = g_data_areas.list();
  if (ImGui::BeginTable("da_table", 5,
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("End", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 20);
    ImGui::TableHeadersRow();

    for (const auto& da : areas) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%04X", da.start);
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%04X", da.end);
      ImGui::TableSetColumnIndex(2);
      const char* type_str = (da.type == DataType::BYTES) ? "Bytes" :
                             (da.type == DataType::WORDS) ? "Words" : "Text";
      ImGui::Text("%s", type_str);
      ImGui::TableSetColumnIndex(3);
      if (!da.label.empty()) ImGui::TextUnformatted(da.label.c_str());
      ImGui::TableSetColumnIndex(4);
      ImGui::PushID(static_cast<int>(da.start));
      if (ImGui::SmallButton("X")) {
        g_data_areas.clear(da.start);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (!open) show_data_areas_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 8: Disassembly Export
// -----------------------------------------------

void DevToolsUI::render_disasm_export()
{
  ImGui::SetNextWindowSize(ImVec2(420, 220), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Disassembly Export", &open)) {
    if (!open) show_disasm_export_ = false;
    ImGui::End();
    return;
  }

  ImGui::SetNextItemWidth(60);
  ImGui::InputText("Start##dex", dex_start_, sizeof(dex_start_),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(60);
  ImGui::InputText("End##dex", dex_end_, sizeof(dex_end_),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::Checkbox("Symbols", &dex_symbols_);

  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##dexpath", "Output path (e.g. /tmp/out.asm)...",
                           dex_path_, sizeof(dex_path_));

  if (ImGui::Button("Export")) {
    unsigned long start_addr, end_addr;
    if (parse_hex(dex_start_, &start_addr, 0xFFFF) &&
        parse_hex(dex_end_, &end_addr, 0xFFFF) &&
        start_addr <= end_addr) {
      std::ostringstream oss;
      char hexbuf[32];
      snprintf(hexbuf, sizeof(hexbuf), "$%04X", static_cast<unsigned>(start_addr));
      oss << "; Disassembly export from konCePCja\n";
      oss << "org " << hexbuf << "\n\n";

      DisassembledCode code;
      std::vector<dword> entry_points;
      word pos = static_cast<word>(start_addr);
      word end_pos = static_cast<word>(end_addr);

      while (pos <= end_pos) {
        if (dex_symbols_) {
          std::string sym = g_symfile.lookupAddr(pos);
          if (!sym.empty()) oss << sym << ":\n";
        }

        const DataArea* da = g_data_areas.find(pos);
        if (da) {
          int remaining = static_cast<int>(da->end) - static_cast<int>(pos) + 1;
          int max_bytes = (da->type == DataType::TEXT) ? 64 : 8;
          int buf_len = std::min(remaining, max_bytes);
          if (pos + buf_len - 1 > end_pos) buf_len = end_pos - pos + 1;
          uint8_t membuf[64];
          for (int mi = 0; mi < buf_len; mi++)
            membuf[mi] = z80_read_mem(static_cast<word>(pos + mi));
          int line_bytes = 0;
          std::string formatted = g_data_areas.format_at(pos, membuf,
                                                          buf_len, &line_bytes);
          oss << "  " << formatted << "\n";
          if (line_bytes == 0) line_bytes = 1;
          unsigned int next = static_cast<unsigned int>(pos) + line_bytes;
          if (next > 0xFFFF || next > end_addr + 1u) break;
          pos = static_cast<word>(next);
        } else {
          auto line = disassemble_one(pos, code, entry_points);
          code.lines.insert(line);
          std::string instr = line.instruction_;
          if (dex_symbols_ && !line.ref_address_string_.empty()) {
            std::string sym = g_symfile.lookupAddr(line.ref_address_);
            if (!sym.empty()) {
              auto ref_pos = instr.find(line.ref_address_string_);
              if (ref_pos != std::string::npos)
                instr.replace(ref_pos, line.ref_address_string_.size(), sym);
            }
          }
          oss << "  " << instr << "\n";
          unsigned int next = static_cast<unsigned int>(pos) + line.Size();
          if (next > 0xFFFF || next > end_addr + 1u) break;
          pos = static_cast<word>(next);
        }
      }

      std::string result = oss.str();
      if (dex_path_[0] != '\0') {
        if (has_path_traversal(dex_path_)) {
          dex_status_ = "Error: path traversal not allowed";
        } else {
          std::ofstream f(dex_path_);
          if (f) {
            f << result;
            f.close();
            dex_status_ = "Exported " + std::to_string(result.size()) + " bytes to " + dex_path_;
          } else {
            dex_status_ = "Error: cannot write to " + std::string(dex_path_);
          }
        }
      } else {
        dex_status_ = "Error: no output path specified";
      }
    } else {
      dex_status_ = "Error: invalid address range";
    }
  }

  if (!dex_status_.empty()) {
    ImGui::TextWrapped("%s", dex_status_.c_str());
  }

  if (!open) show_disasm_export_ = false;
  ImGui::End();
}
