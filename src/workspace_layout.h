#ifndef WORKSPACE_LAYOUT_H
#define WORKSPACE_LAYOUT_H

#include <string>
#include <vector>

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

// Save current window arrangement as a named layout.
bool workspace_save_layout(const std::string& name);

// Load a previously saved layout by name.
bool workspace_load_layout(const std::string& name);

// Delete a saved layout by name.
bool workspace_delete_layout(const std::string& name);

// Return sorted list of saved layout names (stems only, no extension).
std::vector<std::string> workspace_list_layouts();

#endif // WORKSPACE_LAYOUT_H
