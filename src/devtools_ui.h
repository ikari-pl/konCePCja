#ifndef DEVTOOLS_UI_H
#define DEVTOOLS_UI_H

#include <string>
#include <vector>
#include <cstdint>
#include "types.h"
#include "disk_file_editor.h"
#include "disk_sector_editor.h"

class DevToolsUI {
public:
    void render();
    void toggle_window(const std::string& name);
    bool is_window_open(const std::string& name) const;
    bool any_window_open() const;
    bool* window_ptr(const std::string& name);
    void navigate_disassembly(word addr);

private:
    bool show_registers_ = false;
    bool show_disassembly_ = false;
    bool show_memory_hex_ = false;
    bool show_stack_ = false;
    bool show_breakpoints_ = false;
    bool show_symbols_ = false;
    bool show_silicon_disc_ = false;
    bool show_asic_ = false;
    bool show_disc_tools_ = false;

    bool disasm_follow_pc_ = true;
    char disasm_goto_addr_[8] = "";
    int disasm_goto_value_ = -1;

    char memhex_goto_addr_[8] = "";
    int memhex_goto_value_ = -1;
    int memhex_bytes_per_row_ = 16;

    char symtable_filter_[64] = "";

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

    void render_registers();
    void render_disassembly();
    void render_memory_hex();
    void render_stack();
    void render_breakpoints();
    void render_symbols();
    void render_silicon_disc();
    void render_asic();
    void render_disc_tools();
};

extern DevToolsUI g_devtools_ui;

#endif // DEVTOOLS_UI_H
