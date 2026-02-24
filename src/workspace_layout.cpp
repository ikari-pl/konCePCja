#include "workspace_layout.h"
#include "koncepcja.h"
#include "video.h"
#include "devtools_ui.h"
#include "imgui_ui.h"
#include "imgui.h"
#include "imgui_internal.h"

extern t_CPC CPC;

// Persistent dockspace ID
static const ImGuiID DOCKSPACE_ID = ImHashStr("KonCePCjaDockSpace");

// Track whether we've ever applied a preset (to auto-apply Debug on first dock)
static bool s_first_dock = true;

// ─────────────────────────────────────────────────
// Dockspace host window
// ─────────────────────────────────────────────────

void workspace_render_dockspace()
{
    if (CPC.workspace_layout != 1) return;

    // Auto-apply Debug preset on first entry into docked mode
    if (s_first_dock) {
        // Only apply if dockspace has no saved layout (no child nodes)
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(DOCKSPACE_ID);
        if (!node || !node->ChildNodes[0]) {
            workspace_apply_preset(WorkspacePreset::Debug);
        }
        s_first_dock = false;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();

    // Get topbar height to offset below it
    int topbar_h = video_get_topbar_height();
    ImVec2 pos(vp->Pos.x, vp->Pos.y + topbar_h);
    ImVec2 size(vp->Size.x, vp->Size.y - topbar_h);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("##DockSpaceHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGui::DockSpace(DOCKSPACE_ID, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();
}

// ─────────────────────────────────────────────────
// CPC Screen window (docked mode)
// ─────────────────────────────────────────────────

void workspace_render_cpc_screen()
{
    if (CPC.workspace_layout != 1) return;

    ImTextureID tex = static_cast<ImTextureID>(video_get_cpc_texture());
    int tex_w, tex_h;
    video_get_cpc_size(&tex_w, &tex_h);
    if (!tex || tex_w <= 0 || tex_h <= 0) return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    bool open = true;
    if (ImGui::Begin("CPC Screen", &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImVec2 avail = ImGui::GetContentRegionAvail();

        // CPC aspect ratio
        float src_aspect = static_cast<float>(tex_w) / static_cast<float>(tex_h);

        float draw_w, draw_h;
        if (CPC.cpc_screen_scale == 0) {
            // Fit: scale to fill available space preserving aspect ratio
            float dst_aspect = avail.x / avail.y;
            if (dst_aspect > src_aspect) {
                draw_h = avail.y;
                draw_w = draw_h * src_aspect;
            } else {
                draw_w = avail.x;
                draw_h = draw_w / src_aspect;
            }
        } else {
            // Fixed 1x/2x/3x
            draw_w = static_cast<float>(tex_w * CPC.cpc_screen_scale);
            draw_h = static_cast<float>(tex_h * CPC.cpc_screen_scale);
        }

        // Center in available space
        float offset_x = (avail.x - draw_w) * 0.5f;
        float offset_y = (avail.y - draw_h) * 0.5f;
        if (offset_x < 0.0f) offset_x = 0.0f;
        if (offset_y < 0.0f) offset_y = 0.0f;

        // Black background behind the image (for letterboxing)
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            p0, ImVec2(p0.x + avail.x, p0.y + avail.y),
            IM_COL32(0, 0, 0, 255));

        ImGui::SetCursorPos(ImVec2(
            ImGui::GetCursorPos().x + offset_x,
            ImGui::GetCursorPos().y + offset_y));
        ImGui::Image(tex, ImVec2(draw_w, draw_h));

        // Right-click context menu for scale mode
        if (ImGui::BeginPopupContextWindow("##CPCScreenCtx")) {
            ImGui::TextUnformatted("Scale Mode");
            ImGui::Separator();
            if (ImGui::RadioButton("Fit",  CPC.cpc_screen_scale == 0)) CPC.cpc_screen_scale = 0;
            if (ImGui::RadioButton("1x",   CPC.cpc_screen_scale == 1)) CPC.cpc_screen_scale = 1;
            if (ImGui::RadioButton("2x",   CPC.cpc_screen_scale == 2)) CPC.cpc_screen_scale = 2;
            if (ImGui::RadioButton("3x",   CPC.cpc_screen_scale == 3)) CPC.cpc_screen_scale = 3;
            ImGui::EndPopup();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    // If user closed the CPC Screen window, switch back to classic mode
    if (!open) {
        CPC.workspace_layout = 0;
    }
}

// ─────────────────────────────────────────────────
// Preset layouts via DockBuilder
// ─────────────────────────────────────────────────

// Helper: ensure a DevToolsUI window is open by name
static void ensure_window_open(const char* name) {
    if (!g_devtools_ui.is_window_open(name)) {
        g_devtools_ui.toggle_window(name);
    }
}

void workspace_apply_preset(WorkspacePreset preset)
{
    // Clear existing dockspace layout
    ImGui::DockBuilderRemoveNode(DOCKSPACE_ID);
    ImGui::DockBuilderAddNode(DOCKSPACE_ID, ImGuiDockNodeFlags_DockSpace);

    // Size the dockspace to match the main viewport (minus topbar)
    ImGuiViewport* vp = ImGui::GetMainViewport();
    int topbar_h = video_get_topbar_height();
    ImGui::DockBuilderSetNodeSize(DOCKSPACE_ID,
        ImVec2(vp->Size.x, vp->Size.y - topbar_h));
    ImGui::DockBuilderSetNodePos(DOCKSPACE_ID,
        ImVec2(vp->Pos.x, vp->Pos.y + topbar_h));

    ImGuiID center = DOCKSPACE_ID;
    ImGuiID left, right, bottom;

    switch (preset) {
    case WorkspacePreset::Debug:
        // Split: left 25% | center | right 25%, bottom 30%
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.25f, &left, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.33f, &right, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, &bottom, &center);

        ImGui::DockBuilderDockWindow("CPC Screen", center);
        ImGui::DockBuilderDockWindow("DevTools", center);  // tabs with CPC Screen

        ImGui::DockBuilderDockWindow("Disassembly", left);
        ImGui::DockBuilderDockWindow("Breakpoints/WP/IO", left);

        ImGui::DockBuilderDockWindow("Registers", right);
        ImGui::DockBuilderDockWindow("Stack", right);

        ImGui::DockBuilderDockWindow("Memory Hex", bottom);

        ensure_window_open("registers");
        ensure_window_open("disassembly");
        ensure_window_open("stack");
        ensure_window_open("breakpoints");
        ensure_window_open("memory_hex");
        break;

    case WorkspacePreset::IDE:
        // Split: left 20% | center | right 25%
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, &left, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, &right, &center);

        ImGui::DockBuilderDockWindow("CPC Screen", center);
        ImGui::DockBuilderDockWindow("DevTools", center);

        ImGui::DockBuilderDockWindow("Disassembly", left);

        ImGui::DockBuilderDockWindow("Symbols", right);
        ImGui::DockBuilderDockWindow("Breakpoints/WP/IO", right);

        ensure_window_open("disassembly");
        ensure_window_open("symbols");
        ensure_window_open("breakpoints");
        break;

    case WorkspacePreset::Hardware:
        // Split: center | right 30%, bottom 30%
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, &right, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, &bottom, &center);

        ImGui::DockBuilderDockWindow("CPC Screen", center);
        ImGui::DockBuilderDockWindow("DevTools", center);

        ImGui::DockBuilderDockWindow("Video State", right);
        ImGui::DockBuilderDockWindow("Audio State", right);
        ImGui::DockBuilderDockWindow("ASIC Registers", right);

        ImGui::DockBuilderDockWindow("Disc Tools", bottom);
        ImGui::DockBuilderDockWindow("Silicon Disc", bottom);

        ensure_window_open("video_state");
        ensure_window_open("audio_state");
        ensure_window_open("asic");
        ensure_window_open("disc_tools");
        ensure_window_open("silicon_disc");
        break;
    }

    // Ensure DevTools toolbar is visible so windowed panels render
    imgui_state.show_devtools = true;

    ImGui::DockBuilderFinish(DOCKSPACE_ID);
}
