// Definition of the process-wide `imgui_state` singleton.
//
// Lives in its own TU (not imgui_ui.cpp) so the symbol is present in
// MODERN_UI=ON and OFF builds alike: this file is unconditionally part
// of `koncepcja_lib`, while imgui_ui.cpp is excluded when MODERN_UI=OFF.
//
// The struct itself is a pure data container — no ImGui types, no
// rendering — so it compiles cleanly without the UI sources.  Telemetry
// writers in the core (kon_cpc_ja.cpp, z80.cpp, …) and the text IPC
// server read/write this struct directly.

#include "imgui_state.h"

ImGuiUIState imgui_state;
