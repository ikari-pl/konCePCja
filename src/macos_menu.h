#pragma once
#ifdef __APPLE__
void koncpc_setup_macos_menu();
void koncpc_disable_app_nap();
void koncpc_enable_app_nap();
void koncpc_activate_app();
// Dock icon: set the app icon from the bundled PNG, optionally with a live CPC screen inset
void koncpc_set_dock_icon(const char* png_path);
// pixels: full surface, vis_x/vis_y/vis_w/vis_h: visible CPC screen region within it
void koncpc_update_dock_icon_preview(const void* pixels, int surface_w, int surface_h,
                                     int pitch, int vis_x, int vis_y, int vis_w, int vis_h);
#else
inline void koncpc_disable_app_nap() {}
inline void koncpc_enable_app_nap() {}
inline void koncpc_activate_app() {}
inline void koncpc_set_dock_icon(const char*) {}
inline void koncpc_update_dock_icon_preview(const void*, int, int, int, int, int, int, int) {}
#endif
