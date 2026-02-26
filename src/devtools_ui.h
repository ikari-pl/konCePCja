#ifndef DEVTOOLS_UI_H
#define DEVTOOLS_UI_H

#include <string>
#include <vector>
#include <cstdint>
#include "types.h"
#include "disk_file_editor.h"
#include "disk_sector_editor.h"
#include "z80_assembler.h"
#include "TextEditor.h"
#include <memory>

enum class NavTarget { DISASM, MEMORY, GFX };

class DevToolsUI {
public:
    ~DevToolsUI();
    void render();
    void toggle_window(const std::string& name);
    bool is_window_open(const std::string& name) const;
    bool any_window_open() const;
    bool* window_ptr(const std::string& name);
    void navigate_disassembly(word addr);
    void navigate_to(word addr, NavTarget target);
    void navigate_memory(word addr);
    // IPC access: returns a shadow buffer synced from TextEditor on demand
    char* asm_source_buf();
    size_t asm_source_buf_size() const { return sizeof(asm_source_shadow_); }
    void asm_set_source(const char* text);  // IPC write path

    // Returns the array of all window key strings (16 entries).
    static const char* const* all_window_keys(int* count);

private:
    bool show_registers_ = false;
    bool show_disassembly_ = false;
    bool show_memory_hex_ = false;
    bool show_stack_ = false;
    bool show_breakpoints_ = false;
    bool show_symbols_ = false;
    bool show_session_recording_ = false;
    bool show_gfx_finder_ = false;
    bool show_silicon_disc_ = false;
    bool show_asic_ = false;
    bool show_disc_tools_ = false;
    bool show_data_areas_ = false;
    bool show_disasm_export_ = false;
    bool show_video_state_ = false;
    bool show_audio_state_ = false;
    bool show_recording_controls_ = false;
    bool show_assembler_ = false;

    bool disasm_follow_pc_ = true;
    char disasm_goto_addr_[8] = "";
    int disasm_goto_value_ = -1;
    bool disasm_scroll_pending_ = false;

    char memhex_goto_addr_[8] = "";
    int memhex_goto_value_ = -1;
    int memhex_bytes_per_row_ = 16;
    int memhex_highlight_addr_ = -1;  // address to flash-highlight (-1 = none)
    int memhex_highlight_frames_ = 0; // remaining frames of highlight

    char symtable_filter_[64] = "";

    // Session Recording state
    char sr_path_[256] = "";
    std::string sr_status_;

    // Graphics Finder state
    char gfx_addr_[8] = "C000";
    int gfx_width_ = 2;     // width in bytes
    int gfx_height_ = 16;   // height in pixel rows
    int gfx_mode_ = 1;      // CPC mode 0/1/2
    int gfx_zoom_ = 8;
    int gfx_paint_color_ = 1;
    std::vector<uint32_t> gfx_pixels_;
    int gfx_pixel_width_ = 0;
    char gfx_export_path_[256] = "";

    // Silicon Disc state
    char sd_path_[256] = "";
    float sd_bank_usage_[4] = {};
    bool sd_usage_dirty_ = true;

    // Disc Tools state
    int dt_drive_ = 0;  // 0=A, 1=B
    int dt_format_ = 0;
    int dt_track_ = 0;
    int dt_side_ = 0;
    char dt_sector_id_[4] = "C1";
    std::vector<DiskFileEntry> dt_file_cache_;
    std::string dt_file_error_;
    bool dt_files_dirty_ = true;
    std::string dt_format_combo_;
    bool dt_format_combo_dirty_ = true;
    std::vector<SectorInfo> dt_sector_cache_;
    std::string dt_sector_error_;
    std::vector<uint8_t> dt_sector_data_;
    std::string dt_sector_read_error_;

    // Add Watchpoint form state
    char wp_addr_[8] = "";
    char wp_len_[8] = "1";
    int wp_type_ = 2;  // 0=R, 1=W, 2=RW

    // Add IO Breakpoint form state
    char iobp_port_[8] = "";
    char iobp_mask_[8] = "FFFF";
    int iobp_dir_ = 2;  // 0=IN, 1=OUT, 2=BOTH

    // Add Symbol form state
    char sym_addr_[8] = "";
    char sym_name_[64] = "";
    char sym_path_[256] = "";

    // Data Areas window state
    char da_start_[8] = "";
    char da_end_[8] = "";
    char da_label_[64] = "";
    int da_type_ = 0;  // 0=BYTES, 1=WORDS, 2=TEXT

    // Disasm Export window state
    char dex_start_[8] = "0000";
    char dex_end_[8] = "FFFF";
    char dex_path_[256] = "";
    bool dex_symbols_ = true;
    bool dex_prefill_pending_ = false;
    std::string dex_status_;
    char dex_mark_start_[8] = "";
    char dex_mark_end_[8] = "";
    int dex_mark_type_ = 0;

    // Recording Controls state
    char rc_wav_path_[256] = "";
    char rc_ym_path_[256] = "";
    char rc_avi_path_[256] = "";
    int rc_avi_quality_ = 85;
    std::string rc_status_;

    // Assembler state
    std::unique_ptr<TextEditor> asm_editor_;
    bool asm_editor_initialized_ = false;
    char asm_source_shadow_[65536] = "";  // shadow buffer for IPC compatibility
    std::vector<AsmError> asm_errors_;
    std::string asm_status_;
    char asm_path_[256] = "";
    char asm_org_addr_[8] = "4000";
    bool show_asm_reference_ = false;

    void render_registers();
    void render_disassembly();
    void render_memory_hex();
    void render_stack();
    void render_breakpoints();
    void render_symbols();
    void render_session_recording();
    void render_gfx_finder();
    void render_silicon_disc();
    void render_asic();
    void render_disc_tools();
    void render_data_areas();
    void render_disasm_export();
    void render_video_state();
    void render_audio_state();
    void render_recording_controls();
    void render_assembler();
    void render_asm_reference();
};

extern DevToolsUI g_devtools_ui;

#endif // DEVTOOLS_UI_H
