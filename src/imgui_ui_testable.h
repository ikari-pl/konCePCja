// imgui_ui_testable.h - Extracted pure-logic functions for unit testing
// These functions have no ImGui dependencies and can be tested independently.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "types.h"

// ─────────────────────────────────────────────────
// Hex parsing
// ─────────────────────────────────────────────────

// Parse hex string with validation. Returns true if valid, false otherwise.
// On success, *out contains the parsed value. On failure, *out is unchanged.
inline bool parse_hex(const char* str, unsigned long* out,
                      unsigned long max_val) {
  if (!str || !str[0]) return false;
  char* end;
  unsigned long const val = strtoul(str, &end, 16);
  if (*end != '\0' || val > max_val) return false;
  *out = val;
  return true;
}

// ─────────────────────────────────────────────────
// Safe memory read helpers
// ─────────────────────────────────────────────────

// Safe read for unaligned TZX block parsing - returns false if read would
// overflow
inline bool safe_read_word(byte* p, const byte* end, size_t offset, word& out) {
  if (p + offset + sizeof(word) > end) return false;
  memcpy(&out, p + offset, sizeof(word));
  return true;
}

inline bool safe_read_dword(byte* p, const byte* end, size_t offset,
                            dword& out) {
  if (p + offset + sizeof(dword) > end) return false;
  memcpy(&out, p + offset, sizeof(dword));
  return true;
}

// ─────────────────────────────────────────────────
// Configuration lookup helpers
// ─────────────────────────────────────────────────

// RAM size options in KB
constexpr unsigned int RAM_SIZES[] = {64, 128, 192, 256, 320, 512, 576, 4160};
constexpr int RAM_SIZE_COUNT = sizeof(RAM_SIZES) / sizeof(RAM_SIZES[0]);

// Sample rate options in Hz
constexpr unsigned int SAMPLE_RATES[] = {11025, 22050, 44100, 48000, 96000};
constexpr int SAMPLE_RATE_COUNT =
    sizeof(SAMPLE_RATES) / sizeof(SAMPLE_RATES[0]);

// Exact-token membership in a dotted extension list (".dsk.ipf.raw"): true
// iff `ext` (".dsk", lowercase, leading dot) appears as a WHOLE token — i.e.
// followed by another '.' or the end of the list. Plain substring find() is
// wrong here: ".hf" would match inside ".hfe". Pure so the drag-&-drop
// routing can be unit-tested against the list slotshandler exports.
inline bool extension_in_dotted_list(const std::string& list,
                                     const std::string& ext) {
  if (ext.size() < 2 || ext[0] != '.') return false;
  size_t pos = 0;
  while ((pos = list.find(ext, pos)) != std::string::npos) {
    size_t const end = pos + ext.size();
    if (end == list.size() || list[end] == '.') return true;
    pos += 1;
  }
  return false;
}

// Check if a RAM size is in the allowed set
inline bool is_valid_ram_size(unsigned int ram) {
  for (unsigned int const i : RAM_SIZES) {
    if (i == ram) return true;
  }
  return false;
}

// Find index of RAM size in options array, returns 2 (192 KB default) if not
// found
inline int find_ram_index(unsigned int ram) {
  for (int i = 0; i < RAM_SIZE_COUNT; i++) {
    if (RAM_SIZES[i] == ram) return i;
  }
  return 2;  // default to 192 KB
}

// Find index of sample rate in options array, returns 2 (44100 Hz default) if
// not found
inline int find_sample_rate_index(unsigned int rate) {
  for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
    if (SAMPLE_RATES[i] == rate) return i;
  }
  return 2;  // default to 44100 Hz
}

// ─────────────────────────────────────────────────
// Memory display formatting
// ─────────────────────────────────────────────────

// Format memory line into stack buffer - zero heap allocations
// format: 0 = hex only, 1 = hex + ASCII, 2 = hex + decimal
// Returns number of characters written (excluding null terminator)
// Requires: pbRAM pointer to be valid 64KB memory
inline int format_memory_line(char* buf, size_t buf_size,
                              unsigned int base_addr, int bytes_per_line,
                              int format, const byte* ram) {
  if (buf_size == 0 || !ram) return 0;

  int offset = 0;
  int remaining = static_cast<int>(buf_size);

  // Helper to safely advance after snprintf
  auto advance = [&](int written) {
    if (written < 0) return false;
    int const actual = (written < remaining) ? written : remaining - 1;
    offset += actual;
    remaining -= actual;
    return remaining > 1;
  };

  // Address
  if (!advance(
          snprintf(buf + offset, remaining, "%04X : ", base_addr & 0xFFFF))) {
    buf[offset] = '\0';
    return offset;
  }

  // Hex bytes
  for (int j = 0; j < bytes_per_line && remaining > 1; j++) {
    if (!advance(snprintf(buf + offset, remaining, "%02X ",
                          ram[(base_addr + j) & 0xFFFF])))
      break;
  }

  // Extended formats
  if (format == 1 && remaining > 1) {  // Hex & char
    if (advance(snprintf(buf + offset, remaining, " | "))) {
      for (int j = 0; j < bytes_per_line && remaining > 1; j++) {
        byte const b = ram[(base_addr + j) & 0xFFFF];
        buf[offset++] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
        remaining--;
      }
    }
  } else if (format == 2 && remaining > 1) {  // Hex & u8
    if (advance(snprintf(buf + offset, remaining, " | "))) {
      for (int j = 0; j < bytes_per_line && remaining > 1; j++) {
        if (!advance(snprintf(buf + offset, remaining, "%3u ",
                              ram[(base_addr + j) & 0xFFFF])))
          break;
      }
    }
  }

  buf[offset] = '\0';
  return offset;
}

// ─────────────────────────────────────────────────
// MRU (recent files) list management
// ─────────────────────────────────────────────────

#include <algorithm>
#include <string>
#include <vector>

// Push a path to the front of an MRU list, deduplicate, cap at max_size.
inline void mru_list_push(std::vector<std::string>& list,
                          const std::string& path, int max_size = 10) {
  list.erase(std::remove(list.begin(), list.end(), path), list.end());
  list.insert(list.begin(), path);
  if (static_cast<int>(list.size()) > max_size) list.resize(max_size);
}

// ─────────────────────────────────────────────────
// Options dialog: restart decision + peripheral-toggle capture/restore
// ─────────────────────────────────────────────────

// True when the settings staged in Options require a full machine rebuild
// (emulator_init() wipes RAM and cold-boots the CPC) rather than a live
// apply. Model/RAM/keyboard swap the ROM map underneath the running Z80;
// an M4 enable/disable or any serial config change re-touches expansion ROM
// slots and the RS232 card the same way. Computed once so Save and Apply
// can't carry their own, divergent copies of this condition.
inline bool options_needs_restart(
    unsigned int old_model, unsigned int new_model, unsigned int old_ram_size,
    unsigned int new_ram_size, unsigned int old_keyboard,
    unsigned int new_keyboard, bool old_m4_enabled, bool new_m4_enabled,
    bool serial_config_changed) {
  return old_model != new_model || old_ram_size != new_ram_size ||
         old_keyboard != new_keyboard || old_m4_enabled != new_m4_enabled ||
         serial_config_changed;
}

// Snapshot each pointed-to bool into old_values[0..count), same order as
// toggles. Used by Options' first_open capture and revert_options()'s
// restore over one shared table instead of a repeated per-flag triplet.
inline void capture_toggle_values(bool* const* toggles, bool* old_values,
                                  size_t count) {
  for (size_t i = 0; i < count; ++i) old_values[i] = *toggles[i];
}

// Write old_values[0..count) back through toggles, same order as capture.
inline void restore_toggle_values(bool* const* toggles, const bool* old_values,
                                  size_t count) {
  for (size_t i = 0; i < count; ++i) *toggles[i] = old_values[i];
}

// ── Pause hub: the media row ────────────────────────────────────────────────
// The hub's Eject buttons, in the order they are drawn: Disk A, Disk B, Tape.
// A button is enabled only while something is in that drive/deck — the same
// presence the Media menu's Eject items key on. Pure so the enable logic is
// testable without rendering.
struct HubMediaButton {
  const char* label;
  bool enabled;
};
struct HubMediaButtons {
  HubMediaButton button[3];
};
inline HubMediaButtons hub_media_buttons(bool disk_a_present,
                                         bool disk_b_present,
                                         bool tape_present) {
  return {{{"Eject A", disk_a_present},
           {"Eject B", disk_b_present},
           {"Eject Tape", tape_present}}};
}

// ── Debugger step controls ──────────────────────────────────────────────────
// The In / Over / Out / Run-Pause group is drawn in two places: the DevTools
// main toolbar and the Disassembly window's menu bar, where the user's eyes
// already are while stepping (beads-4i4). Both call the same dbg_step_*
// helpers; this decides, for both, what is enabled and what the buttons say,
// so the two groups cannot disagree about when a step is allowed.
struct DebugStepControls {
  bool step_enabled;            // In / Over / Out
  const char* step_in_label;    // labels: full in the toolbar, short in the
  const char* step_over_label;  // Disassembly menu bar
  const char* step_out_label;   // says so while a Step Out walk is in flight
  const char* run_pause_label;
  // Hover text. Deliberately NOT compact-dependent: the two surfaces describe
  // the same action, so they get the same sentence from the same place.
  const char* step_in_tooltip;
  const char* step_over_tooltip;
  const char* step_out_tooltip;
  const char* run_pause_tooltip;
};
inline DebugStepControls debug_step_controls(bool paused, bool walk_running,
                                             bool compact) {
  DebugStepControls c{};
  // A walk (Step Out, Step Over across a CALL, Run to here) resumes and
  // re-pauses the machine on its own worker; a step issued meanwhile would
  // race it over the same ephemeral breakpoint.
  c.step_enabled = paused && !walk_running;
  c.step_in_label = compact ? "In" : "Step In";
  c.step_over_label = compact ? "Over" : "Step Over";
  if (walk_running) {
    c.step_out_label = compact ? "Out..." : "Stepping out...";
  } else {
    c.step_out_label = compact ? "Out" : "Step Out";
  }
  if (compact) {
    c.run_pause_label = paused ? "Run" : "Pause";
  } else {
    c.run_pause_label = paused ? "Resume" : "Pause";
  }
  c.step_in_tooltip = "Step In: one instruction, entering CALLs (F7)";
  c.step_over_tooltip =
      "Step Over: one instruction, over CALLs/RSTs (Shift+F7)";
  c.step_out_tooltip =
      "Step Out: run until this subroutine returns (Shift+F11)";
  c.run_pause_tooltip = "Run / halt the CPU (F5)";
  return c;
}

// ── Toast placement ─────────────────────────────────────────────────────────
// Toasts stack upward from the bottom-right of the viewport the user is
// looking at (beads-ar4). The work rect is that viewport's usable area; the
// toast is clamped into it, so a small floating DevTools viewport still shows
// the toast's left/top edge instead of pushing it off its window.
struct ToastPos {
  float x;
  float y;
};
inline ToastPos toast_pos(float work_x, float work_y, float work_w,
                          float work_h, float box_w, float box_h,
                          float bottom_offset, float right_margin) {
  float x = work_x + work_w - box_w - right_margin;
  float y = work_y + work_h - bottom_offset - box_h;
  // Right/bottom anchoring first, then the left/top edge wins: the start of
  // the message is the part worth keeping visible.
  x = std::max(x, work_x);
  y = std::max(y, work_y);
  return {x, y};
}

// Which viewport shows the toasts: the first of `count` the platform reports
// as focused, else the main viewport (index 0 in ImGuiPlatformIO::Viewports).
// A predicate rather than a container, so the per-frame caller scans the live
// viewport list without building one.
template <typename IsFocused>
int toast_viewport_index(int count, IsFocused is_focused) {
  for (int i = 0; i < count; ++i) {
    if (is_focused(i)) return i;
  }
  return 0;
}

// ── Options dialog: the button row ─────────────────────────────────────────
// Mac order (beads-3v8): Cancel on the left, the default commit on the right.
// Apply commits for this session; Save commits and writes the config file.
enum class OptionsButton : std::uint8_t { Cancel, Apply, Save };
struct OptionsButtonSpec {
  OptionsButton id;
  const char* label;
  const char* tooltip;  // nullptr: no tooltip
  bool is_default;      // takes the default focus (Enter)
};
inline const std::vector<OptionsButtonSpec>& options_button_row() {
  static const std::vector<OptionsButtonSpec> row = {
      {OptionsButton::Cancel, "Cancel", "Discard changes", false},
      {OptionsButton::Apply, "Apply",
       "Apply changes now without saving them\n(this session only)", false},
      {OptionsButton::Save, "Save",
       "Apply changes and write them to the config file", true},
  };
  return row;
}

// What Cancel must undo beyond copying the old settings back. Copying CPC
// restores the values, but the live side effects of a previewed change do
// not follow the struct: the window keeps a previewed size, the renderer a
// previewed plugin, the host keymap a previewed layout.
struct OptionsRevertPlan {
  bool rescale_window;  // resize the window back to old (fixed) scr_scale
  // Old scale was Fit: it has no derived size, so the window goes back to
  // the size it had when Options opened.
  bool restore_window_size;
  bool reinit_video;  // rebuild the video plugin for old scr_style
  bool reload_host_keymap;
};
inline OptionsRevertPlan options_revert_plan(
    unsigned int live_scr_scale, unsigned int old_scr_scale,
    unsigned int live_scr_style, unsigned int old_scr_style,
    const std::string& live_kbd_layout, const std::string& old_kbd_layout) {
  bool const scale_changed = live_scr_scale != old_scr_scale;
  return {scale_changed && old_scr_scale != 0,
          scale_changed && old_scr_scale == 0, live_scr_style != old_scr_style,
          live_kbd_layout != old_kbd_layout};
}

// ── Disc Tools: when the Files listing is stale ────────────────────────────
// The listing is cached (it walks the whole directory); it must be rebuilt
// when the user looks at a different drive or the drive holds a different
// medium than when it was built (beads-p5t).
struct DiscToolsMediaKey {
  int drive;            // 0 = A, 1 = B
  uint64_t generation;  // dsk_media_generation() of that drive
  bool operator==(const DiscToolsMediaKey& o) const {
    return drive == o.drive && generation == o.generation;
  }
  bool operator!=(const DiscToolsMediaKey& o) const { return !(*this == o); }
};
inline bool disc_tools_listing_stale(const DiscToolsMediaKey& listed,
                                     const DiscToolsMediaKey& live) {
  return listed != live;
}
