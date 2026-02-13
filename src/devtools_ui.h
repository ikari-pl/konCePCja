#ifndef DEVTOOLS_UI_H
#define DEVTOOLS_UI_H

#include <string>
#include <vector>
#include <cstdint>
#include "types.h"

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
    bool show_session_recording_ = false;
    bool show_gfx_finder_ = false;

    bool disasm_follow_pc_ = true;
    char disasm_goto_addr_[8] = "";
    int disasm_goto_value_ = -1;

    char memhex_goto_addr_[8] = "";
    int memhex_goto_value_ = -1;
    int memhex_bytes_per_row_ = 16;

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

    void render_registers();
    void render_disassembly();
    void render_memory_hex();
    void render_stack();
    void render_breakpoints();
    void render_symbols();
    void render_session_recording();
    void render_gfx_finder();
};

extern DevToolsUI g_devtools_ui;

#endif // DEVTOOLS_UI_H
