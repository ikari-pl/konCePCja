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

extern t_CPC CPC;
extern t_z80regs z80;

DevToolsUI g_devtools_ui;

// -----------------------------------------------
// Name-to-pointer mapping helpers
// -----------------------------------------------

bool* DevToolsUI::window_ptr(const std::string& name)
{
  if (name == "registers")   return &show_registers_;
  if (name == "disassembly") return &show_disassembly_;
  if (name == "memory_hex")  return &show_memory_hex_;
  if (name == "stack")       return &show_stack_;
  if (name == "breakpoints") return &show_breakpoints_;
  if (name == "symbols")     return &show_symbols_;
  return nullptr;
}

void DevToolsUI::toggle_window(const std::string& name)
{
  bool* p = window_ptr(name);
  if (p) *p = !*p;
}

bool DevToolsUI::is_window_open(const std::string& name) const
{
  if (name == "registers")   return show_registers_;
  if (name == "disassembly") return show_disassembly_;
  if (name == "memory_hex")  return show_memory_hex_;
  if (name == "stack")       return show_stack_;
  if (name == "breakpoints") return show_breakpoints_;
  if (name == "symbols")     return show_symbols_;
  return false;
}

bool DevToolsUI::any_window_open() const
{
  return show_registers_ || show_disassembly_ || show_memory_hex_ ||
         show_stack_ || show_breakpoints_ || show_symbols_;
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
  if (show_registers_)   render_registers();
  if (show_disassembly_) render_disassembly();
  if (show_memory_hex_)  render_memory_hex();
  if (show_stack_)       render_stack();
  if (show_breakpoints_) render_breakpoints();
  if (show_symbols_)     render_symbols();
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
