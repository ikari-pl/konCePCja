#pragma once
#ifdef __APPLE__
void koncpc_setup_macos_menu();
void koncpc_disable_app_nap();
void koncpc_enable_app_nap();
void koncpc_activate_app();
#else
inline void koncpc_disable_app_nap() {}
inline void koncpc_enable_app_nap() {}
inline void koncpc_activate_app() {}
#endif
