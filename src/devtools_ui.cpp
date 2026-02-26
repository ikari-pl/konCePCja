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
#include "session_recording.h"
#include "gfx_finder.h"
#include "silicon_disc.h"
#include "asic.h"
#include "disk.h"
#include "disk_file_editor.h"
#include "disk_sector_editor.h"
#include "disk_format.h"
#include "data_areas.h"
#include "z80_assembler.h"
#include "wav_recorder.h"
#include "ym_recorder.h"
#include "avi_recorder.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

extern t_CPC CPC;
extern t_z80regs z80;
extern byte* pbRAM;
extern t_drive driveA;
extern t_drive driveB;
extern double colours_rgb[32][3];
extern t_GateArray GateArray;
extern t_CRTC CRTC;
extern t_PSG PSG;

// Reject paths containing ".." to prevent path traversal
static bool has_path_traversal(const char* path)
{
  for (const auto& comp : std::filesystem::path(path)) {
    if (comp == "..") return true;
  }
  return false;
}

// Consistent link color for all navigable addresses
static constexpr ImVec4 kLinkColor(0.4f, 0.8f, 1.0f, 1.0f);

DevToolsUI g_devtools_ui;

// -----------------------------------------------
// Name-to-pointer mapping helpers
// -----------------------------------------------

bool* DevToolsUI::window_ptr(const std::string& name)
{
  if (name == "registers")          return &show_registers_;
  if (name == "disassembly")        return &show_disassembly_;
  if (name == "memory_hex")         return &show_memory_hex_;
  if (name == "stack")              return &show_stack_;
  if (name == "breakpoints")        return &show_breakpoints_;
  if (name == "symbols")            return &show_symbols_;
  if (name == "session_recording")  return &show_session_recording_;
  if (name == "gfx_finder")         return &show_gfx_finder_;
  if (name == "silicon_disc")       return &show_silicon_disc_;
  if (name == "asic")               return &show_asic_;
  if (name == "disc_tools")         return &show_disc_tools_;
  if (name == "data_areas")         return &show_data_areas_;
  if (name == "disasm_export")      return &show_disasm_export_;
  if (name == "video_state")        return &show_video_state_;
  if (name == "audio_state")        return &show_audio_state_;
  if (name == "recording_controls") return &show_recording_controls_;
  if (name == "assembler")          return &show_assembler_;
  return nullptr;
}

void DevToolsUI::toggle_window(const std::string& name)
{
  bool* p = window_ptr(name);
  if (!p) return;
  *p = !*p;
  // Pre-fill disasm export address range from current disassembly view
  if (name == "disasm_export" && *p)
    dex_prefill_pending_ = true;
}

bool DevToolsUI::is_window_open(const std::string& name) const
{
  if (name == "registers")          return show_registers_;
  if (name == "disassembly")        return show_disassembly_;
  if (name == "memory_hex")         return show_memory_hex_;
  if (name == "stack")              return show_stack_;
  if (name == "breakpoints")        return show_breakpoints_;
  if (name == "symbols")            return show_symbols_;
  if (name == "session_recording")  return show_session_recording_;
  if (name == "gfx_finder")         return show_gfx_finder_;
  if (name == "silicon_disc")       return show_silicon_disc_;
  if (name == "asic")               return show_asic_;
  if (name == "disc_tools")         return show_disc_tools_;
  if (name == "data_areas")         return show_data_areas_;
  if (name == "disasm_export")      return show_disasm_export_;
  if (name == "video_state")        return show_video_state_;
  if (name == "audio_state")        return show_audio_state_;
  if (name == "recording_controls") return show_recording_controls_;
  if (name == "assembler")          return show_assembler_;
  return false;
}

bool DevToolsUI::any_window_open() const
{
  return show_registers_ || show_disassembly_ || show_memory_hex_ ||
         show_stack_ || show_breakpoints_ || show_symbols_ ||
         show_session_recording_ || show_gfx_finder_ ||
         show_silicon_disc_ || show_asic_ || show_disc_tools_ ||
         show_data_areas_ || show_disasm_export_ ||
         show_video_state_ || show_audio_state_ ||
         show_recording_controls_ || show_assembler_;
}

const char* const* DevToolsUI::all_window_keys(int* count)
{
  static const char* keys[] = {
    "registers", "disassembly", "memory_hex", "stack", "breakpoints",
    "symbols", "session_recording", "gfx_finder", "silicon_disc",
    "asic", "disc_tools", "data_areas", "disasm_export",
    "video_state", "audio_state", "recording_controls", "assembler"
  };
  *count = 17;
  return keys;
}

void DevToolsUI::navigate_disassembly(word addr)
{
  show_disassembly_ = true;
  disasm_follow_pc_ = false;
  disasm_goto_value_ = addr;
  disasm_scroll_pending_ = true;
  snprintf(disasm_goto_addr_, sizeof(disasm_goto_addr_), "%04X", addr);
}

void DevToolsUI::navigate_to(word addr, NavTarget target)
{
  switch (target) {
    case NavTarget::DISASM:
      navigate_disassembly(addr);
      break;
    case NavTarget::MEMORY:
      navigate_memory(addr);
      break;
    case NavTarget::GFX:
      show_gfx_finder_ = true;
      snprintf(gfx_addr_, sizeof(gfx_addr_), "%04X", addr);
      break;
  }
}

void DevToolsUI::navigate_memory(word addr)
{
  show_memory_hex_ = true;
  memhex_goto_value_ = addr;
  snprintf(memhex_goto_addr_, sizeof(memhex_goto_addr_), "%04X", addr);
  memhex_highlight_addr_ = addr;
  memhex_highlight_frames_ = 90;  // ~1.5 seconds at 60fps
}

// -----------------------------------------------
// Main render dispatch
// -----------------------------------------------

void DevToolsUI::render()
{
  // Detect disasm_export opening via MenuItem (bypasses toggle_window)
  static bool prev_disasm_export = false;
  if (show_disasm_export_ && !prev_disasm_export)
    dex_prefill_pending_ = true;
  prev_disasm_export = show_disasm_export_;

  if (show_registers_)         render_registers();
  if (show_disassembly_)       render_disassembly();
  if (show_memory_hex_)        render_memory_hex();
  if (show_stack_)             render_stack();
  if (show_breakpoints_)       render_breakpoints();
  if (show_symbols_)           render_symbols();
  if (show_silicon_disc_)      render_silicon_disc();
  if (show_asic_)              render_asic();
  if (show_disc_tools_)        render_disc_tools();
  if (show_data_areas_)        render_data_areas();
  if (show_disasm_export_)     render_disasm_export();
  if (show_session_recording_) render_session_recording();
  if (show_gfx_finder_)       render_gfx_finder();
  if (show_video_state_)       render_video_state();
  if (show_audio_state_)       render_audio_state();
  if (show_recording_controls_) render_recording_controls();
  if (show_assembler_)          render_assembler();
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
        disasm_scroll_pending_ = true;
      }
    }
    ImGui::Separator();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Click lines to toggle breakpoints. Right-click for more options.");
    ImGui::EndMenuBar();
  }

  // Determine the center address
  word center_pc = z80.PC.w.l;
  if (!disasm_follow_pc_ && disasm_goto_value_ >= 0) {
    center_pc = static_cast<word>(disasm_goto_value_);
  }

  // Disassemble ~48 instructions starting a few lines before center
  // Use a small offset so the target appears near the top 1/3 of the view
  word start_addr = center_pc - 16;
  constexpr int NUM_LINES = 48;

  DisassembledCode dummy_dc;
  std::vector<dword> dummy_eps;
  struct DisasmEntry {
    word addr;
    std::string text;
    std::string label;
    bool is_data_area = false;
  };
  std::vector<DisasmEntry> lines;
  lines.reserve(NUM_LINES);

  word addr = start_addr;
  for (int i = 0; i < NUM_LINES; i++) {
    DisasmEntry entry;
    entry.addr = addr;

    // Check for symbol at this address
    entry.label = g_symfile.lookupAddr(addr);

    // Check if this address is in a data area
    const DataArea* da = g_data_areas.find(addr);
    if (da) {
      entry.is_data_area = true;
      int remaining = static_cast<int>(da->end) - static_cast<int>(addr) + 1;
      int max_bytes = (da->type == DataType::TEXT) ? 64 : 8;
      int buf_len = std::min(remaining, max_bytes);
      uint8_t membuf[64];
      for (int mi = 0; mi < buf_len; mi++)
        membuf[mi] = z80_read_mem(static_cast<word>(addr + mi));
      int line_bytes = 0;
      entry.text = g_data_areas.format_at(addr, membuf, buf_len, &line_bytes);
      if (line_bytes <= 0) line_bytes = 1;
      addr = (addr + line_bytes) & 0xFFFF;
    } else {
      auto line = disassemble_one(addr, dummy_dc, dummy_eps);
      entry.text = line.instruction_;
      int len = line.Size();
      if (len <= 0) len = 1;
      addr = (addr + len) & 0xFFFF;
    }
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
      static constexpr const char* kBreakpointMarker = "\xe2\x97\x8f"; // Unicode ●
      char label[256];
      snprintf(label, sizeof(label), "%s %04X  %s",
               is_bp ? kBreakpointMarker : " ",
               entry.addr, entry.text.c_str());

      // Color: green for PC, red for breakpoint, amber for data area
      int style_colors_pushed = 0;
      if (entry.is_data_area && !is_pc && !is_bp) {
        // Subtle amber background for data area lines
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.20f, 0.05f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.45f, 1.0f));
        style_colors_pushed = 2;
      } else if (is_pc) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
        style_colors_pushed = 1;
      } else if (is_bp) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        style_colors_pushed = 1;
      }

      if (ImGui::Selectable(label, is_pc || entry.is_data_area)) {
        if (!entry.is_data_area) {
          // Left click: toggle breakpoint (not meaningful for data areas)
          if (is_bp)
            z80_del_breakpoint(entry.addr);
          else
            z80_add_breakpoint(entry.addr);
        }
      }
      if (ImGui::IsItemHovered()) {
        if (entry.is_data_area)
          ImGui::SetTooltip("Data area | Right-click: edit/remove");
        else
          ImGui::SetTooltip("Click: toggle breakpoint | Right-click: more options");
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }

      // Right-click context menu
      if (ImGui::BeginPopupContextItem()) {
        const DataArea* ctx_da = g_data_areas.find(entry.addr);
        if (!entry.is_data_area) {
          if (ImGui::MenuItem("Run to here")) {
            z80_add_breakpoint_ephemeral(entry.addr);
            CPC.paused = false;
          }
          if (ImGui::MenuItem("Set PC here")) {
            z80.PC.w.l = entry.addr;
          }
        }
        if (ImGui::MenuItem("Goto this address")) {
          disasm_goto_value_ = entry.addr;
          disasm_follow_pc_ = false;
          snprintf(disasm_goto_addr_, sizeof(disasm_goto_addr_),
                   "%04X", entry.addr);
        }
        if (ImGui::MenuItem("View in Memory")) {
          navigate_to(entry.addr, NavTarget::MEMORY);
        }
        ImGui::Separator();
        if (ctx_da) {
          if (ImGui::MenuItem("Edit data area")) {
            // Pre-fill the Data Areas mark form and open the window
            snprintf(da_start_, sizeof(da_start_), "%04X", ctx_da->start);
            snprintf(da_end_, sizeof(da_end_), "%04X", ctx_da->end);
            da_type_ = (ctx_da->type == DataType::BYTES) ? 0 :
                       (ctx_da->type == DataType::WORDS) ? 1 : 2;
            if (!ctx_da->label.empty())
              snprintf(da_label_, sizeof(da_label_), "%s", ctx_da->label.c_str());
            else
              da_label_[0] = '\0';
            show_data_areas_ = true;
          }
          if (ImGui::MenuItem("Remove data area")) {
            g_data_areas.clear(ctx_da->start);
          }
        } else {
          if (ImGui::MenuItem("Mark as bytes")) {
            g_data_areas.mark(entry.addr, entry.addr, DataType::BYTES);
          }
          if (ImGui::MenuItem("Mark as words")) {
            g_data_areas.mark(entry.addr, entry.addr + 1, DataType::WORDS);
          }
          if (ImGui::MenuItem("Mark as text")) {
            g_data_areas.mark(entry.addr, entry.addr, DataType::TEXT);
          }
        }
        ImGui::EndPopup();
      }

      if (style_colors_pushed > 0) ImGui::PopStyleColor(style_colors_pushed);

      // Follow PC: scroll to keep PC visible (using SetScrollHereY in-place)
      if (is_pc && disasm_follow_pc_) {
        ImGui::SetScrollHereY(0.3f);
      }

      // Track goto target for one-shot scroll
      if (disasm_scroll_pending_ && disasm_goto_value_ >= 0 &&
          entry.addr == static_cast<word>(disasm_goto_value_)) {
        scroll_to_idx = i;
      }
    }

    // One-shot scroll to goto target
    if (disasm_scroll_pending_ && scroll_to_idx >= 0) {
      float item_height = ImGui::GetTextLineHeightWithSpacing();
      ImGui::SetScrollY(scroll_to_idx * item_height - ImGui::GetWindowHeight() * 0.3f);
      disasm_scroll_pending_ = false;
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
    ImGui::Separator();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Right-click for navigation options.");
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

          // Flash-highlight the navigation target byte
          if (memhex_highlight_frames_ > 0 && a == static_cast<word>(memhex_highlight_addr_)) {
            float alpha = static_cast<float>(memhex_highlight_frames_) / 90.0f;
            ImVec2 rmin = ImGui::GetItemRectMin();
            ImVec2 rmax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(
              rmin, rmax,
              ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.8f, 0.0f, 0.5f * alpha)));
          }

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

    // Right-click context menu for the whole hex view
    if (ImGui::BeginPopupContextWindow("##memhex_ctx")) {
      // Calculate which row was right-clicked
      float item_height = ImGui::GetTextLineHeightWithSpacing();
      ImVec2 click_pos = ImGui::GetMousePosOnOpeningCurrentPopup();
      ImVec2 win_pos = ImGui::GetWindowPos();
      ImVec2 content_min = ImGui::GetWindowContentRegionMin();
      float scroll = ImGui::GetScrollY();
      int row = static_cast<int>((click_pos.y - win_pos.y - content_min.y + scroll) / item_height);
      if (row < 0) row = 0;
      if (row >= total_rows) row = total_rows - 1;
      word ctx_addr = static_cast<word>((row * bytes_per_row) & 0xFFFF);

      ImGui::TextDisabled("%04X", ctx_addr);
      ImGui::Separator();
      if (ImGui::MenuItem("Disassemble here")) {
        navigate_to(ctx_addr, NavTarget::DISASM);
      }
      if (ImGui::MenuItem("View as graphics")) {
        navigate_to(ctx_addr, NavTarget::GFX);
      }
      ImGui::EndPopup();
    }
  }
  ImGui::EndChild();

  if (memhex_highlight_frames_ > 0) memhex_highlight_frames_--;

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
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Double-click to view in Disassembly");
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
  if (!ImGui::Begin("Breakpoints & Watchpoints & IO###BPWindow", &open)) {
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

  const auto& bps = z80_list_breakpoints_ref();
  const auto& wps = z80_list_watchpoints_ref();
  const auto& iobps = z80_list_io_breakpoints_ref();

  // Count visible (non-ephemeral) breakpoints
  int bp_visible = 0;
  for (const auto& bp : bps) {
    if (bp.type != EPHEMERAL) bp_visible++;
  }
  ImGui::Text("BP: %d  |  WP: %zu  |  IO: %zu", bp_visible, wps.size(), iobps.size());
  ImGui::Separator();

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
      ImGui::PushID(static_cast<int>(i));
      {
        std::string sym = g_symfile.lookupAddr(bp.address);
        ImGui::PushStyleColor(ImGuiCol_Text, kLinkColor);
        char bp_label[80];
        if (!sym.empty())
          snprintf(bp_label, sizeof(bp_label), "%04X %s", bp.address, sym.c_str());
        else
          snprintf(bp_label, sizeof(bp_label), "%04X", bp.address);
        if (ImGui::Selectable(bp_label, false, ImGuiSelectableFlags_DontClosePopups))
          navigate_to(bp.address, NavTarget::DISASM);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Click to show in Disassembly");
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::PopStyleColor();
      }
      ImGui::TableSetColumnIndex(2);
      if (!bp.condition_str.empty())
        ImGui::TextUnformatted(bp.condition_str.c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%d", bp.hit_count);
      ImGui::TableSetColumnIndex(4);
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
      ImGui::PushID(1000 + static_cast<int>(i));
      {
        ImGui::PushStyleColor(ImGuiCol_Text, kLinkColor);
        char wp_label[32];
        if (wp.length > 1)
          snprintf(wp_label, sizeof(wp_label), "%04X+%d", static_cast<word>(wp.address), wp.length);
        else
          snprintf(wp_label, sizeof(wp_label), "%04X", static_cast<word>(wp.address));
        if (ImGui::Selectable(wp_label, false, ImGuiSelectableFlags_DontClosePopups))
          navigate_to(static_cast<word>(wp.address), NavTarget::MEMORY);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Click to show in Memory Hex");
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::PopStyleColor();
      }
      ImGui::TableSetColumnIndex(2);
      if (!wp.condition_str.empty())
        ImGui::TextUnformatted(wp.condition_str.c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%d", wp.hit_count);
      ImGui::TableSetColumnIndex(4);
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
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click to view in Disassembly");
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
// Debug Window 7: Silicon Disc
// -----------------------------------------------

void DevToolsUI::render_silicon_disc()
{
  ImGui::SetNextWindowSize(ImVec2(380, 280), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Silicon Disc", &open)) {
    if (!open) show_silicon_disc_ = false;
    ImGui::End();
    return;
  }

  ImGui::Checkbox("Enabled", &g_silicon_disc.enabled);
  ImGui::Separator();

  // Bank usage bars (% non-zero bytes per 64K bank) — cached to avoid 256K scan every frame
  if (g_silicon_disc.data) {
    if (sd_usage_dirty_) {
      for (int bank = 0; bank < SILICON_DISC_BANKS; bank++) {
        const uint8_t* ptr = g_silicon_disc.bank_ptr(bank);
        int used = 0;
        for (size_t i = 0; i < SILICON_DISC_BANK_SIZE; i++) {
          if (ptr[i] != 0) used++;
        }
        sd_bank_usage_[bank] = static_cast<float>(used) / SILICON_DISC_BANK_SIZE;
      }
      sd_usage_dirty_ = false;
    }
    for (int bank = 0; bank < SILICON_DISC_BANKS; bank++) {
      char overlay[32];
      snprintf(overlay, sizeof(overlay), "Bank %d: %d%% used", bank + SILICON_DISC_FIRST_BANK,
               static_cast<int>(sd_bank_usage_[bank] * 100));
      ImGui::ProgressBar(sd_bank_usage_[bank], ImVec2(-1, 0), overlay);
    }
    if (ImGui::SmallButton("Refresh Usage")) sd_usage_dirty_ = true;
  } else {
    ImGui::TextDisabled("Not initialized (enable and reset to allocate)");
  }

  ImGui::Separator();
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##sdpath", "File path for save/load...",
                           sd_path_, sizeof(sd_path_));

  if (ImGui::Button("Save")) {
    if (sd_path_[0] != '\0')
      silicon_disc_save(g_silicon_disc, sd_path_);
  }
  ImGui::SameLine();
  if (ImGui::Button("Load")) {
    if (sd_path_[0] != '\0') {
      silicon_disc_load(g_silicon_disc, sd_path_);
      sd_usage_dirty_ = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    silicon_disc_clear(g_silicon_disc);
    sd_usage_dirty_ = true;
  }

  if (!open) show_silicon_disc_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 8: ASIC Register Viewer
// -----------------------------------------------

void DevToolsUI::render_asic()
{
  ImGui::SetNextWindowSize(ImVec2(520, 500), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("ASIC Registers", &open)) {
    if (!open) show_asic_ = false;
    ImGui::End();
    return;
  }

  ImGui::Text("Lock state: %s (pos %d)", asic.locked ? "Locked" : "Unlocked", asic.lockSeqPos);
  ImGui::Separator();

  // Sprites
  if (ImGui::CollapsingHeader("Sprites", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::BeginTable("asic_sprites", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
      ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 20);
      ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 50);
      ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 50);
      ImGui::TableSetupColumn("Mag", ImGuiTableColumnFlags_WidthFixed, 60);
      ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed, 50);
      ImGui::TableHeadersRow();

      for (int i = 0; i < 16; i++) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%d", i);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%d", asic.sprites_x[i]);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d", asic.sprites_y[i]);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%dx%d", asic.sprites_mag_x[i], asic.sprites_mag_y[i]);
        ImGui::TableSetColumnIndex(4);
        bool visible = (asic.sprites_x[i] != 0 || asic.sprites_y[i] != 0);
        ImGui::Text("%s", visible ? "Yes" : "No");
      }
      ImGui::EndTable();
    }
  }

  // DMA Channels
  if (ImGui::CollapsingHeader("DMA Channels", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (int ch = 0; ch < NB_DMA_CHANNELS; ch++) {
      const auto& dma_ch = asic.dma.ch[ch];
      ImGui::PushID(ch);
      ImGui::Text("Channel %d:", ch);
      ImGui::SameLine();
      ImGui::TextColored(dma_ch.enabled ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                                        : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                         "%s", dma_ch.enabled ? "ENABLED" : "disabled");
      ImGui::Text("  Addr: %04X  Loop: %04X  Prescaler: %d  Pause: %d  Loops: %d",
                  dma_ch.source_address, dma_ch.loop_address,
                  dma_ch.prescaler, dma_ch.pause_ticks, dma_ch.loops);
      ImGui::Text("  IRQ: %s  Tick cycles: %d",
                  dma_ch.interrupt ? "yes" : "no", dma_ch.tick_cycles);
      ImGui::PopID();
    }
  }

  // Palette
  if (ImGui::CollapsingHeader("Palette")) {
    float sz = 20.0f;
    for (int i = 0; i < 17; i++) {
      int hw_color = GateArray.ink_values[i];
      float r = static_cast<float>(colours_rgb[hw_color][0]);
      float g = static_cast<float>(colours_rgb[hw_color][1]);
      float b = static_cast<float>(colours_rgb[hw_color][2]);
      ImVec4 col(r, g, b, 1.0f);
      ImGui::PushID(i);
      ImGui::ColorButton("##ink", col, ImGuiColorEditFlags_NoTooltip, ImVec2(sz, sz));
      ImGui::PopID();
      if (i < 16) ImGui::SameLine();
    }
    ImGui::Text("Ink 0-15 + Border");
  }

  // Interrupts & scroll
  if (ImGui::CollapsingHeader("Interrupts & Scroll")) {
    ImGui::Text("Raster interrupt: %s", asic.raster_interrupt ? "enabled" : "disabled");
    ImGui::Text("Interrupt vector: %02X", asic.interrupt_vector);
    ImGui::Text("H-Scroll: %d  V-Scroll: %d", asic.hscroll, asic.vscroll);
    ImGui::Text("Extend border: %s", asic.extend_border ? "yes" : "no");

    ImGui::Text("DMA IRQ flags:");
    for (int ch = 0; ch < NB_DMA_CHANNELS; ch++) {
      ImGui::SameLine();
      ImGui::Text("CH%d:%s", ch, asic.dma.ch[ch].interrupt ? "IRQ" : "---");
    }
  }

  if (!open) show_asic_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 9: Disc Tools
// -----------------------------------------------

void DevToolsUI::render_disc_tools()
{
  ImGui::SetNextWindowSize(ImVec2(520, 500), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Disc Tools", &open)) {
    if (!open) show_disc_tools_ = false;
    ImGui::End();
    return;
  }

  // Drive selector
  const char* drives[] = { "Drive A", "Drive B" };
  ImGui::SetNextItemWidth(100);
  if (ImGui::Combo("Drive", &dt_drive_, drives, 2)) {
    dt_files_dirty_ = true;
  }
  t_drive* drv = (dt_drive_ == 0) ? &driveA : &driveB;

  ImGui::Separator();

  // Format section
  if (ImGui::CollapsingHeader("Format Disc")) {
    // Cache format names and combo string (they don't change at runtime)
    if (dt_format_combo_dirty_) {
      auto formats = disk_format_names();
      dt_format_combo_.clear();
      for (const auto& f : formats) {
        dt_format_combo_ += f;
        dt_format_combo_ += '\0';
      }
      dt_format_combo_ += '\0';
      dt_format_combo_dirty_ = false;
    }
    if (!dt_format_combo_.empty()) {
      ImGui::SetNextItemWidth(120);
      ImGui::Combo("Format##dt", &dt_format_, dt_format_combo_.c_str());
      ImGui::SameLine();
      if (ImGui::Button("Format")) {
        auto formats = disk_format_names();
        char letter = (dt_drive_ == 0) ? 'A' : 'B';
        if (dt_format_ >= 0 && dt_format_ < static_cast<int>(formats.size())) {
          disk_format_drive(letter, formats[dt_format_]);
          dt_files_dirty_ = true;
        }
      }
    }
  }

  // File browser
  if (ImGui::CollapsingHeader("Files", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (dt_files_dirty_) {
      dt_file_cache_ = disk_list_files(drv, dt_file_error_);
      dt_files_dirty_ = false;
    }
    if (ImGui::Button("Refresh##files")) dt_files_dirty_ = true;

    if (!dt_file_error_.empty()) {
      ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "%s", dt_file_error_.c_str());
    }

    if (ImGui::BeginTable("dt_files", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
        ImVec2(0, 200))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 60);
      ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthFixed, 30);
      ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 20);
      ImGui::TableHeadersRow();

      for (size_t i = 0; i < dt_file_cache_.size(); i++) {
        const auto& fe = dt_file_cache_[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(fe.display_name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u", fe.size_bytes);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d", fe.user);
        ImGui::TableSetColumnIndex(3);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("X")) {
          disk_delete_file(drv, fe.filename);
          dt_files_dirty_ = true;
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }

  // Sector browser
  if (ImGui::CollapsingHeader("Sector Browser")) {
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("Track##sec", &dt_track_, 1, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40);
    ImGui::InputInt("Side##sec", &dt_side_, 1, 1);
    if (dt_track_ < 0) dt_track_ = 0;
    if (dt_side_ < 0) dt_side_ = 0;

    ImGui::SameLine();
    if (ImGui::Button("List Sectors")) {
      dt_sector_cache_ = disk_sector_info(drv, static_cast<unsigned>(dt_track_),
                                           static_cast<unsigned>(dt_side_),
                                           dt_sector_error_);
    }

    if (!dt_sector_error_.empty()) {
      ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "%s", dt_sector_error_.c_str());
    }

    if (!dt_sector_cache_.empty()) {
      if (ImGui::BeginTable("dt_sectors", 5,
          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("C", ImGuiTableColumnFlags_WidthFixed, 25);
        ImGui::TableSetupColumn("H", ImGuiTableColumnFlags_WidthFixed, 25);
        ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("N", ImGuiTableColumnFlags_WidthFixed, 25);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        for (const auto& si : dt_sector_cache_) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0); ImGui::Text("%02X", si.C);
          ImGui::TableSetColumnIndex(1); ImGui::Text("%02X", si.H);
          ImGui::TableSetColumnIndex(2); ImGui::Text("%02X", si.R);
          ImGui::TableSetColumnIndex(3); ImGui::Text("%02X", si.N);
          ImGui::TableSetColumnIndex(4); ImGui::Text("%u", si.size);
        }
        ImGui::EndTable();
      }
    }

    // Read a specific sector
    ImGui::SetNextItemWidth(40);
    ImGui::InputText("Sector ID##read", dt_sector_id_, sizeof(dt_sector_id_),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    if (ImGui::Button("Read Sector")) {
      unsigned long sid;
      if (parse_hex(dt_sector_id_, &sid, 0xFF)) {
        dt_sector_data_ = disk_sector_read(drv, static_cast<unsigned>(dt_track_),
                                            static_cast<unsigned>(dt_side_),
                                            static_cast<uint8_t>(sid),
                                            dt_sector_read_error_);
      }
    }

    if (!dt_sector_read_error_.empty()) {
      ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "%s", dt_sector_read_error_.c_str());
    }

    // Hex dump of read sector
    if (!dt_sector_data_.empty()) {
      ImGui::Text("Sector data (%zu bytes):", dt_sector_data_.size());
      if (ImGui::BeginChild("##secdata", ImVec2(0, 150), ImGuiChildFlags_Borders)) {
        for (size_t off = 0; off < dt_sector_data_.size(); off += 16) {
          ImGui::Text("%04X:", static_cast<unsigned>(off));
          ImGui::SameLine();
          for (size_t col = 0; col < 16 && off + col < dt_sector_data_.size(); col++) {
            ImGui::SameLine();
            ImGui::Text("%02X", dt_sector_data_[off + col]);
          }
          // ASCII
          ImGui::SameLine();
          char ascii[17] = {};
          for (size_t col = 0; col < 16 && off + col < dt_sector_data_.size(); col++) {
            uint8_t b = dt_sector_data_[off + col];
            ascii[col] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
          }
          ImGui::Text("|%s|", ascii);
        }
      }
      ImGui::EndChild();
    }
  }

  if (!open) show_disc_tools_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 10: Data Areas
// -----------------------------------------------

void DevToolsUI::render_data_areas()
{
  ImGui::SetNextWindowSize(ImVec2(560, 350), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Data Areas", &open)) {
    if (!open) show_data_areas_ = false;
    ImGui::End();
    return;
  }

  if (ImGui::Button("Clear All")) g_data_areas.clear_all();
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Marked regions appear as db/dw/text in Disasm Export");
  ImGui::SameLine();
  if (ImGui::SmallButton("Open Disasm Export"))
    show_disasm_export_ = true;
  ImGui::SameLine();
  {
    auto areas_tmp = g_data_areas.list();
    bool has_areas = !areas_tmp.empty();
    if (!has_areas) ImGui::BeginDisabled();
    if (ImGui::SmallButton("Export Marked Range")) {
      uint16_t lo = 0xFFFF, hi = 0x0000;
      for (const auto& a : areas_tmp) {
        if (a.start < lo) lo = a.start;
        if (a.end > hi) hi = a.end;
      }
      snprintf(dex_start_, sizeof(dex_start_), "%04X", lo);
      snprintf(dex_end_, sizeof(dex_end_), "%04X", hi);
      dex_prefill_pending_ = false;
      show_disasm_export_ = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Open Disasm Export pre-filled with the bounding range of all data areas");
    if (!has_areas) ImGui::EndDisabled();
  }
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
      ImGui::PushID(static_cast<int>(da.start));
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      {
        ImGui::PushStyleColor(ImGuiCol_Text, kLinkColor);
        char da_label_id[8];
        snprintf(da_label_id, sizeof(da_label_id), "%04X", da.start);
        if (ImGui::Selectable(da_label_id, false, ImGuiSelectableFlags_DontClosePopups))
          navigate_to(da.start, NavTarget::DISASM);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Click to show in Disassembly");
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::PopStyleColor();
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%04X", da.end);
      ImGui::TableSetColumnIndex(2);
      const char* type_str = (da.type == DataType::BYTES) ? "Bytes" :
                             (da.type == DataType::WORDS) ? "Words" : "Text";
      ImGui::Text("%s", type_str);
      ImGui::TableSetColumnIndex(3);
      if (!da.label.empty()) ImGui::TextUnformatted(da.label.c_str());
      ImGui::TableSetColumnIndex(4);
      if (ImGui::SmallButton("X")) {
        g_data_areas.clear(da.start);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
  ImGui::TextWrapped("These markers affect Disasm Export output (db/dw/text directives) "
                     "and appear with distinct highlighting in the Disassembly view.");
  ImGui::PopStyleColor();

  if (!open) show_data_areas_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 11: Disassembly Export
// -----------------------------------------------

void DevToolsUI::render_disasm_export()
{
  // Pre-fill address range from current disassembly view on first open
  if (dex_prefill_pending_) {
    word start = disasm_follow_pc_ ? z80.PC.w.l
               : (disasm_goto_value_ >= 0 ? static_cast<word>(disasm_goto_value_) : z80.PC.w.l);
    word end = start + 0xFF;
    snprintf(dex_start_, sizeof(dex_start_), "%04X", start);
    snprintf(dex_end_, sizeof(dex_end_), "%04X", end);
    dex_prefill_pending_ = false;
  }

  ImGui::SetNextWindowSize(ImVec2(420, 450), ImGuiCond_FirstUseEver);

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

  // Data areas summary within selected range
  {
    unsigned long s_addr, e_addr;
    if (parse_hex(dex_start_, &s_addr, 0xFFFF) && parse_hex(dex_end_, &e_addr, 0xFFFF) && s_addr <= e_addr) {
      auto all_areas = g_data_areas.list();
      int byte_count = 0, word_count = 0, text_count = 0;
      for (const auto& a : all_areas) {
        if (a.end >= s_addr && a.start <= e_addr) {
          switch (a.type) {
            case DataType::BYTES: byte_count++; break;
            case DataType::WORDS: word_count++; break;
            case DataType::TEXT:  text_count++; break;
          }
        }
      }
      int total = byte_count + word_count + text_count;
      if (total > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
        std::string summary = std::to_string(total) + " data area" + (total > 1 ? "s" : "") + " in range (";
        bool first = true;
        if (byte_count > 0) { summary += std::to_string(byte_count) + " bytes"; first = false; }
        if (word_count > 0) { if (!first) summary += ", "; summary += std::to_string(word_count) + " words"; first = false; }
        if (text_count > 0) { if (!first) summary += ", "; summary += std::to_string(text_count) + " text"; }
        summary += ")";
        ImGui::TextUnformatted(summary.c_str());
        ImGui::PopStyleColor();
      }
    }
  }

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

  ImGui::Separator();

  // Quick "Mark as data" form
  if (ImGui::TreeNode("Mark as data")) {
    ImGui::SetNextItemWidth(60);
    ImGui::InputText("Start##dexmark", dex_mark_start_, sizeof(dex_mark_start_),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputText("End##dexmark", dex_mark_end_, sizeof(dex_mark_end_),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    const char* mark_types[] = { "Bytes", "Words", "Text" };
    ImGui::Combo("##dexmarktype", &dex_mark_type_, mark_types, 3);
    ImGui::SameLine();
    if (ImGui::Button("Mark##dex")) {
      unsigned long ms, me;
      if (parse_hex(dex_mark_start_, &ms, 0xFFFF) && parse_hex(dex_mark_end_, &me, 0xFFFF) && ms <= me) {
        DataType dt = (dex_mark_type_ == 0) ? DataType::BYTES :
                      (dex_mark_type_ == 1) ? DataType::WORDS : DataType::TEXT;
        g_data_areas.mark(static_cast<uint16_t>(ms), static_cast<uint16_t>(me), dt);
        dex_mark_start_[0] = '\0';
        dex_mark_end_[0] = '\0';
      }
    }
    ImGui::TreePop();
  }

  // Inline preview
  {
    unsigned long s_addr, e_addr;
    if (parse_hex(dex_start_, &s_addr, 0xFFFF) && parse_hex(dex_end_, &e_addr, 0xFFFF) && s_addr <= e_addr) {
      ImGui::Separator();
      ImGui::Text("Preview:");
      ImGui::BeginChild("##dex_preview", ImVec2(0, 0), ImGuiChildFlags_Borders);
      DisassembledCode preview_code;
      std::vector<dword> preview_eps;
      word pos = static_cast<word>(s_addr);
      word end_pos = static_cast<word>(e_addr);
      int line_count = 0;
      constexpr int MAX_PREVIEW_LINES = 200;

      while (pos <= end_pos && line_count < MAX_PREVIEW_LINES) {
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
          std::string formatted = g_data_areas.format_at(pos, membuf, buf_len, &line_bytes);
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f));
          ImGui::Text("%04X  %s", pos, formatted.c_str());
          ImGui::PopStyleColor();
          if (line_bytes == 0) line_bytes = 1;
          unsigned int next = static_cast<unsigned int>(pos) + line_bytes;
          if (next > 0xFFFF || next > e_addr + 1u) break;
          pos = static_cast<word>(next);
        } else {
          auto line = disassemble_one(pos, preview_code, preview_eps);
          preview_code.lines.insert(line);
          ImGui::Text("%04X  %s", pos, line.instruction_.c_str());
          unsigned int next = static_cast<unsigned int>(pos) + line.Size();
          if (next > 0xFFFF || next > e_addr + 1u) break;
          pos = static_cast<word>(next);
        }
        line_count++;
      }
      if (line_count >= MAX_PREVIEW_LINES) {
        ImGui::TextDisabled("... (truncated at %d lines)", MAX_PREVIEW_LINES);
      }
      ImGui::EndChild();
    }
  }

  if (!open) show_disasm_export_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 12: Session Recording
// -----------------------------------------------

void DevToolsUI::render_session_recording()
{
  ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Session Recording", &open)) {
    if (!open) show_session_recording_ = false;
    ImGui::End();
    return;
  }

  // Help icon
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Records keyboard input for replay.");
  ImGui::Separator();

  SessionState state = g_session.state();

  // State indicator
  if (state == SessionState::IDLE) {
    ImGui::Text("Status: Idle");
  } else if (state == SessionState::RECORDING) {
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                       "Status: Recording (%u frames, %u events)",
                       g_session.frame_count(), g_session.event_count());
  } else if (state == SessionState::PLAYING) {
    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                       "Status: Playing (%u / %u frames)",
                       g_session.frame_count(), g_session.total_frames());
    float progress = g_session.total_frames() > 0
        ? static_cast<float>(g_session.frame_count()) / g_session.total_frames()
        : 0.0f;
    ImGui::ProgressBar(progress, ImVec2(-1, 0));
  }

  ImGui::Separator();
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##srpath", "Recording file path (.ksr)...",
                           sr_path_, sizeof(sr_path_));

  // Buttons
  if (state == SessionState::IDLE) {
    if (ImGui::Button("Record")) {
      if (sr_path_[0] != '\0') {
        std::string snap_path = std::string(sr_path_) + ".sna";
        extern int snapshot_save(const std::string&);
        if (snapshot_save(snap_path) == 0) {
          if (g_session.start_recording(sr_path_, snap_path))
            sr_status_ = "Recording started";
          else
            sr_status_ = "Error: failed to start recording";
        } else {
          sr_status_ = "Error: failed to save snapshot";
        }
      } else {
        sr_status_ = "Error: no path specified";
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Play")) {
      if (sr_path_[0] != '\0') {
        std::string snap_path;
        if (g_session.start_playback(sr_path_, snap_path)) {
          extern int snapshot_load(const std::string&);
          snapshot_load(snap_path);
          sr_status_ = "Playback started";
        } else {
          sr_status_ = "Error: failed to start playback";
        }
      } else {
        sr_status_ = "Error: no path specified";
      }
    }
  } else {
    if (ImGui::Button("Stop")) {
      if (state == SessionState::RECORDING) {
        g_session.stop_recording();
        sr_status_ = "Recording stopped";
      } else {
        g_session.stop_playback();
        sr_status_ = "Playback stopped";
      }
    }
  }

  if (!sr_status_.empty()) {
    ImGui::TextWrapped("%s", sr_status_.c_str());
  }

  if (!g_session.path().empty()) {
    ImGui::TextDisabled("File: %s", g_session.path().c_str());
  }

  if (!open) show_session_recording_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 13: Graphics Finder
// -----------------------------------------------

void DevToolsUI::render_gfx_finder()
{
  ImGui::SetNextWindowSize(ImVec2(500, 500), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Graphics Finder", &open)) {
    if (!open) show_gfx_finder_ = false;
    ImGui::End();
    return;
  }

  // Help icon
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Click on the canvas to paint pixels.");
  ImGui::SameLine();

  // Parameters
  ImGui::SetNextItemWidth(60);
  ImGui::InputText("Addr##gfx", gfx_addr_, sizeof(gfx_addr_),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(60);
  ImGui::InputInt("W (bytes)##gfx", &gfx_width_);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(60);
  ImGui::InputInt("H##gfx", &gfx_height_);

  if (gfx_width_ < 1) gfx_width_ = 1;
  if (gfx_width_ > 80) gfx_width_ = 80;
  if (gfx_height_ < 1) gfx_height_ = 1;
  if (gfx_height_ > 256) gfx_height_ = 256;

  ImGui::SetNextItemWidth(80);
  const char* modes[] = { "Mode 0", "Mode 1", "Mode 2" };
  ImGui::Combo("Mode##gfx", &gfx_mode_, modes, 3);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80);
  ImGui::SliderInt("Zoom##gfx", &gfx_zoom_, 1, 16);

  // Decode the graphics
  unsigned long base_addr = 0;
  parse_hex(gfx_addr_, &base_addr, 0xFFFF);

  // Read CPC memory into a buffer starting from base_addr
  size_t mem_size = static_cast<size_t>(gfx_width_) * gfx_height_;
  std::vector<uint8_t> mem_buf(mem_size);
  for (size_t i = 0; i < mem_size; i++) {
    mem_buf[i] = z80_read_mem(static_cast<word>((base_addr + i) & 0xFFFF));
  }

  // Use address=0 since mem_buf is already relative to base_addr
  GfxViewParams params;
  params.address = 0;
  params.width = gfx_width_;
  params.height = gfx_height_;
  params.mode = gfx_mode_;

  // Get current palette
  uint32_t palette[27];
  gfx_get_palette_rgba(palette, 27);

  // Decode
  gfx_pixels_.clear();
  gfx_pixel_width_ = gfx_decode(mem_buf.data(), mem_buf.size(), params,
                                  palette, gfx_pixels_);

  ImGui::Separator();

  // Palette selector for paint mode
  ImGui::Text("Paint color:");
  ImGui::SameLine();
  for (int i = 0; i < 16; i++) {
    uint32_t rgba = palette[i];
    float r = ((rgba >> 0) & 0xFF) / 255.0f;
    float g = ((rgba >> 8) & 0xFF) / 255.0f;
    float b = ((rgba >> 16) & 0xFF) / 255.0f;
    ImVec4 col(r, g, b, 1.0f);
    ImGui::PushID(i);
    if (ImGui::ColorButton("##pal", col, ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16))) {
      gfx_paint_color_ = i;
    }
    ImGui::PopID();
    if (i < 15) ImGui::SameLine();
  }

  ImGui::Separator();

  // Render using DrawList rectangles
  if (gfx_pixel_width_ > 0 && !gfx_pixels_.empty()) {
    int pixel_h = gfx_height_;
    int pixel_w = gfx_pixel_width_;
    float zoom = static_cast<float>(gfx_zoom_);

    ImVec2 canvas_size(pixel_w * zoom, pixel_h * zoom);
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();

    // Invisible button for interaction
    ImGui::InvisibleButton("##gfxcanvas", canvas_size);
    bool hovered = ImGui::IsItemHovered();

    // Handle paint on click — only write back the single modified byte
    if (hovered && ImGui::IsMouseDown(0)) {
      ImVec2 mouse = ImGui::GetMousePos();
      int mx = static_cast<int>((mouse.x - canvas_pos.x) / zoom);
      int my = static_cast<int>((mouse.y - canvas_pos.y) / zoom);
      if (mx >= 0 && mx < pixel_w && my >= 0 && my < pixel_h) {
        int ppb = (gfx_mode_ == 0) ? 2 : (gfx_mode_ == 1) ? 4 : 8;
        int byte_col = mx / ppb;
        size_t byte_offset = static_cast<size_t>(my) * gfx_width_ + byte_col;
        if (gfx_paint(mem_buf.data(), mem_buf.size(), params,
                      mx, my, static_cast<uint8_t>(gfx_paint_color_))) {
          z80_write_mem(static_cast<word>((base_addr + byte_offset) & 0xFFFF),
                        mem_buf[byte_offset]);
        }
      }
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    for (int y = 0; y < pixel_h; y++) {
      for (int x = 0; x < pixel_w; x++) {
        uint32_t rgba = gfx_pixels_[y * pixel_w + x];
        ImU32 col = IM_COL32((rgba >> 0) & 0xFF, (rgba >> 8) & 0xFF,
                             (rgba >> 16) & 0xFF, 255);
        ImVec2 p0(canvas_pos.x + x * zoom, canvas_pos.y + y * zoom);
        ImVec2 p1(p0.x + zoom, p0.y + zoom);
        draw_list->AddRectFilled(p0, p1, col);
      }
    }

    // Show mouse coordinates
    if (hovered) {
      ImVec2 mouse = ImGui::GetMousePos();
      int mx = static_cast<int>((mouse.x - canvas_pos.x) / zoom);
      int my = static_cast<int>((mouse.y - canvas_pos.y) / zoom);
      ImGui::Text("Pixel: (%d, %d)", mx, my);
    }
  } else {
    ImGui::TextDisabled("No graphics to display");
  }

  // Export
  ImGui::Separator();
  ImGui::SetNextItemWidth(-80);
  ImGui::InputTextWithHint("##gfxexport", "Export path (.bmp)...",
                           gfx_export_path_, sizeof(gfx_export_path_));
  ImGui::SameLine();
  if (ImGui::Button("Export BMP") && gfx_pixel_width_ > 0 && !gfx_pixels_.empty()) {
    if (gfx_export_path_[0] != '\0') {
      gfx_export_bmp(gfx_export_path_, gfx_pixels_.data(),
                     gfx_pixel_width_, gfx_height_);
    }
  }

  if (!open) show_gfx_finder_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 14: Video State
// -----------------------------------------------

void DevToolsUI::render_video_state()
{
  ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Video State", &open)) {
    if (!open) show_video_state_ = false;
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("Live  |  Read-only");
  ImGui::Spacing();
  ImGui::Text("CRTC Registers");
  ImGui::Separator();
  const char* crtc_names[] = {
    "R0: H Total", "R1: H Displayed", "R2: H Sync Pos", "R3: Sync Widths",
    "R4: V Total", "R5: V Total Adj", "R6: V Displayed", "R7: V Sync Pos",
    "R8: Interlace", "R9: Max Raster", "R10: Cursor Start", "R11: Cursor End",
    "R12: Start Addr H", "R13: Start Addr L", "R14: Cursor H", "R15: Cursor L",
    "R16: LPEN H", "R17: LPEN L"
  };

  for (int i = 0; i < 18; i++) {
    unsigned char val = CRTC.registers[i];
    ImGui::SetNextItemWidth(50);
    ImGui::InputScalar(crtc_names[i], ImGuiDataType_U8, &val, nullptr, nullptr, "%02X",
                       ImGuiInputTextFlags_ReadOnly);
  }

  ImGui::Spacing();
  ImGui::Text("Gate Array");
  ImGui::Separator();
  ImGui::Text("Screen Mode: %d", GateArray.scr_mode);
  ImGui::Text("ROM Config: %02X", GateArray.ROM_config);
  ImGui::Text("RAM Config: %02X", GateArray.RAM_config);
  ImGui::Text("Pen: %d", GateArray.pen);

  if (!open) show_video_state_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 15: Audio State
// -----------------------------------------------

// Draw a single oscilloscope channel strip
static void draw_scope_strip(const char* label, ImU32 color, const PsgScopeCapture& scope,
                             int chan_idx, float width, float height)
{
  ImGui::Text("%s", label);
  ImGui::SameLine(20.0f);

  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImVec2 p1(p0.x + width, p0.y + height);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(0x10, 0x10, 0x10, 0xFF));
  dl->AddRect(p0, p1, IM_COL32(0x30, 0x30, 0x30, 0xFF));

  // Find the peak value across all samples for normalization
  int peak = 1;
  for (int i = 0; i < PsgScopeCapture::SIZE; i++) {
    int v;
    switch (chan_idx) {
      case 0: v = scope.buf[i].chan_a; break;
      case 1: v = scope.buf[i].chan_b; break;
      default: v = scope.buf[i].chan_c; break;
    }
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }

  constexpr int N = PsgScopeCapture::SIZE;
  float stepX = width / (float)(N - 1);
  float midY = p0.y + height - 2.0f;  // baseline 2px above bottom border
  float hRange = height - 4.0f;       // leave 2px margin top/bottom

  // Build step-waveform polyline (square wave style)
  ImVec2 points[N * 2 + 2];
  int nPoints = 0;

  auto sample = [&](int i) -> float {
    int idx = (scope.head + i) % N;
    int v;
    switch (chan_idx) {
      case 0: v = scope.buf[idx].chan_a; break;
      case 1: v = scope.buf[idx].chan_b; break;
      default: v = scope.buf[idx].chan_c; break;
    }
    return (float)v / (float)peak;
  };

  float prevY = midY - sample(0) * hRange;
  points[nPoints++] = ImVec2(p0.x, prevY);

  for (int i = 1; i < N; i++) {
    float curX = p0.x + i * stepX;
    float curY = midY - sample(i) * hRange;
    if (curY != prevY) {
      points[nPoints++] = ImVec2(curX, prevY);
      points[nPoints++] = ImVec2(curX, curY);
      prevY = curY;
    }
  }
  points[nPoints++] = ImVec2(p1.x, prevY);

  dl->AddPolyline(points, nPoints, color, 0, 1.0f);

  // Reserve space in ImGui layout
  ImGui::Dummy(ImVec2(width, height));
}

void DevToolsUI::render_audio_state()
{
  ImGui::SetNextWindowSize(ImVec2(420, 600), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Audio State", &open)) {
    if (!open) show_audio_state_ = false;
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("Live  |  Read-only");
  ImGui::Spacing();
  ImGui::Text("PSG (AY-3-8912) Registers");
  ImGui::Separator();

  if (ImGui::BeginTable("##psg_state", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Channel");
    ImGui::TableSetupColumn("Tone Freq");
    ImGui::TableSetupColumn("Volume");
    ImGui::TableSetupColumn("Tone/Noise");
    ImGui::TableHeadersRow();

    auto row = [](const char* ch, unsigned short tone, unsigned char amp, bool tone_on, bool noise_on) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%s", ch);
      ImGui::TableNextColumn(); ImGui::Text("%d", tone & 0xFFF);
      ImGui::TableNextColumn(); ImGui::Text("%d", amp & 0x1F);
      ImGui::TableNextColumn(); ImGui::Text("%s/%s", tone_on ? "ON" : "off", noise_on ? "ON" : "off");
    };

    byte mixer = PSG.RegisterAY.Mixer;
    row("A", PSG.RegisterAY.TonA, PSG.RegisterAY.AmplitudeA, !(mixer & 1), !(mixer & 8));
    row("B", PSG.RegisterAY.TonB, PSG.RegisterAY.AmplitudeB, !(mixer & 2), !(mixer & 16));
    row("C", PSG.RegisterAY.TonC, PSG.RegisterAY.AmplitudeC, !(mixer & 4), !(mixer & 32));
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::Text("Noise Freq: %d", PSG.RegisterAY.Noise & 0x1F);
  ImGui::Text("Envelope: %d (Type: %d)", PSG.RegisterAY.Envelope, PSG.RegisterAY.EnvType);

  // ── Waveforms (per-channel oscilloscope) ──
  ImGui::Spacing();
  if (ImGui::CollapsingHeader("Waveforms", ImGuiTreeNodeFlags_DefaultOpen)) {
    float waveW = ImGui::GetContentRegionAvail().x - 24.0f;
    if (waveW < 100.0f) waveW = 100.0f;
    float stripH = 40.0f;

    draw_scope_strip("A", IM_COL32(0xFF, 0x40, 0x40, 0xFF), g_psg_scope, 0, waveW, stripH);
    ImGui::Spacing();
    draw_scope_strip("B", IM_COL32(0x40, 0xFF, 0x40, 0xFF), g_psg_scope, 1, waveW, stripH);
    ImGui::Spacing();
    draw_scope_strip("C", IM_COL32(0x40, 0x80, 0xFF, 0xFF), g_psg_scope, 2, waveW, stripH);
  }

  // ── Envelope visualization ──
  if (ImGui::CollapsingHeader("Envelope", ImGuiTreeNodeFlags_DefaultOpen)) {
    float waveW = ImGui::GetContentRegionAvail().x - 4.0f;
    if (waveW < 100.0f) waveW = 100.0f;
    float stripH = 40.0f;

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + waveW, p0.y + stripH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(0x10, 0x10, 0x10, 0xFF));
    dl->AddRect(p0, p1, IM_COL32(0x30, 0x30, 0x30, 0xFF));

    constexpr int N = PsgScopeCapture::SIZE;
    float stepX = waveW / (float)(N - 1);
    float botY = p0.y + stripH - 2.0f;
    float topY = p0.y + 2.0f;
    float hRange = botY - topY;

    ImVec2 points[N];
    for (int i = 0; i < N; i++) {
      int idx = (g_psg_scope.head + i) % N;
      float val = g_psg_scope.buf[idx].envelope / 31.0f;  // normalize 0..1
      points[i] = ImVec2(p0.x + i * stepX, botY - val * hRange);
    }
    dl->AddPolyline(points, N, IM_COL32(0xFF, 0xD0, 0x40, 0xFF), 0, 1.0f);

    ImGui::Dummy(ImVec2(waveW, stripH));
  }

  if (!open) show_audio_state_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 16: Recording Controls
// -----------------------------------------------

static std::string format_size(uint64_t bytes)
{
  char buf[32];
  if (bytes >= 1048576) {
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buf;
}

void DevToolsUI::render_recording_controls()
{
  ImGui::SetNextWindowSize(ImVec2(420, 400), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Recording Controls", &open)) {
    if (!open) show_recording_controls_ = false;
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Records audio/video output to files.\nWAV = raw PCM audio, YM = PSG music, AVI = MJPEG video+audio.");
  ImGui::Separator();

  // Audio config from CPC state
  unsigned int sample_rate = SAMPLE_RATES[CPC.snd_playback_rate < SAMPLE_RATE_COUNT ? CPC.snd_playback_rate : 2];
  uint16_t bits = CPC.snd_bits ? 16 : 8;
  uint16_t channels = CPC.snd_stereo ? 2 : 1;

  // --- WAV ---
  if (ImGui::CollapsingHeader("WAV Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SetNextItemWidth(-80);
    ImGui::InputTextWithHint("##wavpath", "WAV file path...", rc_wav_path_, sizeof(rc_wav_path_));
    ImGui::SameLine();
    if (g_wav_recorder.is_recording()) {
      if (ImGui::Button("Stop##wav")) {
        uint32_t written = g_wav_recorder.stop();
        rc_status_ = "WAV stopped (" + format_size(written) + " written)";
      }
      ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                         "Recording (%uHz, %d-bit %s)",
                         sample_rate, bits, channels == 2 ? "stereo" : "mono");
      ImGui::Text("Written: %s", format_size(g_wav_recorder.bytes_written()).c_str());
    } else {
      if (ImGui::Button("Record##wav")) {
        if (rc_wav_path_[0] != '\0') {
          std::string err = g_wav_recorder.start(rc_wav_path_, sample_rate, bits, channels);
          rc_status_ = err.empty() ? "WAV recording started" : err;
        } else {
          rc_status_ = "Error: no WAV path specified";
        }
      }
      ImGui::TextDisabled("Idle");
    }
  }

  // --- YM ---
  if (ImGui::CollapsingHeader("YM Music", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SetNextItemWidth(-80);
    ImGui::InputTextWithHint("##ympath", "YM file path...", rc_ym_path_, sizeof(rc_ym_path_));
    ImGui::SameLine();
    if (g_ym_recorder.is_recording()) {
      if (ImGui::Button("Stop##ym")) {
        uint32_t frames = g_ym_recorder.stop();
        rc_status_ = "YM stopped (" + std::to_string(frames) + " frames)";
      }
      uint32_t fc = g_ym_recorder.frame_count();
      ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                         "Recording (%u frames, %.1fs)",
                         fc, fc / 50.0f);
    } else {
      if (ImGui::Button("Record##ym")) {
        if (rc_ym_path_[0] != '\0') {
          std::string err = g_ym_recorder.start(rc_ym_path_);
          rc_status_ = err.empty() ? "YM recording started" : err;
        } else {
          rc_status_ = "Error: no YM path specified";
        }
      }
      ImGui::TextDisabled("Idle");
    }
  }

  // --- AVI ---
  if (ImGui::CollapsingHeader("AVI Video", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SetNextItemWidth(-80);
    ImGui::InputTextWithHint("##avipath", "AVI file path...", rc_avi_path_, sizeof(rc_avi_path_));
    ImGui::SameLine();
    if (g_avi_recorder.is_recording()) {
      if (ImGui::Button("Stop##avi")) {
        uint32_t frames = g_avi_recorder.stop();
        rc_status_ = "AVI stopped (" + std::to_string(frames) + " frames)";
      }
      uint32_t fc = g_avi_recorder.frame_count();
      ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                         "Recording (%u frames, %s)",
                         fc, format_size(g_avi_recorder.bytes_written()).c_str());
    } else {
      ImGui::SetNextItemWidth(120);
      ImGui::SliderInt("Quality##avi", &rc_avi_quality_, 1, 100);
      if (ImGui::Button("Record##avi")) {
        if (rc_avi_path_[0] != '\0') {
          std::string err = g_avi_recorder.start(rc_avi_path_, rc_avi_quality_,
                                                  sample_rate, channels, bits);
          rc_status_ = err.empty() ? "AVI recording started" : err;
        } else {
          rc_status_ = "Error: no AVI path specified";
        }
      }
      ImGui::TextDisabled("Idle");
    }
  }

  // --- Stop All ---
  ImGui::Separator();
  bool any_recording = g_wav_recorder.is_recording() ||
                       g_ym_recorder.is_recording() ||
                       g_avi_recorder.is_recording();
  if (!any_recording) ImGui::BeginDisabled();
  if (ImGui::Button("Stop All")) {
    if (g_wav_recorder.is_recording()) g_wav_recorder.stop();
    if (g_ym_recorder.is_recording()) g_ym_recorder.stop();
    if (g_avi_recorder.is_recording()) g_avi_recorder.stop();
    rc_status_ = "All recordings stopped";
  }
  if (!any_recording) ImGui::EndDisabled();

  if (!rc_status_.empty()) {
    ImGui::TextWrapped("%s", rc_status_.c_str());
  }

  if (!open) show_recording_controls_ = false;
  ImGui::End();
}

// -----------------------------------------------
// Debug Window 17: Assembler
// -----------------------------------------------

void DevToolsUI::render_assembler()
{
  ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
  bool open = true;
  if (!ImGui::Begin("Assembler##devtools", &open)) {
    ImGui::End();
    return;
  }

  // Toolbar
  if (ImGui::Button("Assemble")) {
    AsmResult r = g_assembler.assemble(asm_source_);
    asm_errors_ = r.errors;
    if (r.success) {
      char buf[128];
      snprintf(buf, sizeof(buf), "OK: %d bytes at $%04X-$%04X",
               r.bytes_written, r.start_addr, r.end_addr);
      asm_status_ = buf;
      // Export symbols to debugger
      for (auto& [name, addr] : r.symbols) {
        g_symfile.addSymbol(addr, name);
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d error(s)", static_cast<int>(r.errors.size()));
      asm_status_ = buf;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Check")) {
    AsmResult r = g_assembler.check(asm_source_);
    asm_errors_ = r.errors;
    if (r.success) {
      char buf[128];
      snprintf(buf, sizeof(buf), "OK: %d bytes at $%04X-$%04X (dry run)",
               r.bytes_written, r.start_addr, r.end_addr);
      asm_status_ = buf;
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d error(s)", static_cast<int>(r.errors.size()));
      asm_status_ = buf;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    asm_source_[0] = '\0';
    asm_errors_.clear();
    asm_status_.clear();
  }

  // Load/Save
  ImGui::SameLine(0, 16.0f);
  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputText("##asm_path", asm_path_, sizeof(asm_path_));
  ImGui::SameLine();
  if (ImGui::Button("Load")) {
    if (has_path_traversal(asm_path_)) {
      asm_status_ = "Error: path traversal not allowed";
    } else {
      std::ifstream f(asm_path_);
      if (f.good()) {
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        size_t max_len = sizeof(asm_source_) - 1;
        if (content.size() > max_len) content.resize(max_len);
        memcpy(asm_source_, content.c_str(), content.size() + 1);
        asm_status_ = "Loaded " + std::to_string(content.size()) + " bytes";
        asm_errors_.clear();
      } else {
        asm_status_ = "Error: cannot open file";
      }
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Save")) {
    if (has_path_traversal(asm_path_)) {
      asm_status_ = "Error: path traversal not allowed";
    } else {
      std::ofstream f(asm_path_);
      if (f.good()) {
        f << asm_source_;
        asm_status_ = "Saved";
      } else {
        asm_status_ = "Error: cannot write file";
      }
    }
  }

  // Status line
  if (!asm_status_.empty()) {
    bool is_error = asm_status_.find("error") != std::string::npos ||
                    asm_status_.find("Error") != std::string::npos;
    if (is_error)
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", asm_status_.c_str());
    else
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", asm_status_.c_str());
  }

  // Source editor — use remaining space minus error list area
  float avail = ImGui::GetContentRegionAvail().y;
  float editor_height = asm_errors_.empty() ? avail : avail * 0.7f;
  ImGui::InputTextMultiline("##asm_source", asm_source_, sizeof(asm_source_),
                            ImVec2(-1.0f, editor_height),
                            ImGuiInputTextFlags_AllowTabInput);

  // Error list
  if (!asm_errors_.empty()) {
    ImGui::Separator();
    ImGui::Text("Errors:");
    ImGui::BeginChild("##asm_errors", ImVec2(0, 0), ImGuiChildFlags_None);
    for (auto& err : asm_errors_) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "Line %d: %s", err.line, err.message.c_str());
    }
    ImGui::EndChild();
  }

  if (!open) show_assembler_ = false;
  ImGui::End();
}
