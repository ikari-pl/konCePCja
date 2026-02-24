#ifndef WORKSPACE_LAYOUT_H
#define WORKSPACE_LAYOUT_H

enum class WorkspacePreset { Debug, IDE, Hardware };

// Render the fullscreen dockspace host window (call before other windows).
// Only active when CPC.workspace_layout == 1.
void workspace_render_dockspace();

// Render the CPC Screen as a dockable ImGui window.
// Only active when CPC.workspace_layout == 1.
void workspace_render_cpc_screen();

// Apply a preset layout using DockBuilder.
// Resets dockspace splits and docks windows by title.
void workspace_apply_preset(WorkspacePreset preset);

#endif // WORKSPACE_LAYOUT_H
