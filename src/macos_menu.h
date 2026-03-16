#pragma once
#ifdef __APPLE__
void koncpc_setup_macos_menu();
void koncpc_disable_app_nap();
void koncpc_enable_app_nap();
void koncpc_activate_app();
void koncpc_order_viewports_above_main();
// Dock icon: set the app icon from the bundled PNG, optionally with a live CPC screen inset
void koncpc_set_dock_icon(const char* png_path);
void koncpc_update_dock_icon_preview(const void* pixels, int w, int h, int pitch);
#else
inline void koncpc_disable_app_nap() {}
inline void koncpc_enable_app_nap() {}
inline void koncpc_activate_app() {}
inline void koncpc_order_viewports_above_main() {}
inline void koncpc_set_dock_icon(const char*) {}
inline void koncpc_update_dock_icon_preview(const void*, int, int, int) {}
#endif
