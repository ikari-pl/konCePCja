#pragma once
// Present the rendered frame directly to a DRM/KMS display (LilyGO T-Display
// K230 and similar), bypassing SDL's video layer.
//
// Why this exists: the K230's rootfs has libdrm but no libEGL/libGLESv2/libgbm
// and no Mesa at all, and /dev/dri exposes card0 with no render node. SDL's
// KMSDRM driver requires GBM, so it cannot initialise there. The emulator runs
// under SDL_VIDEODRIVER=offscreen and this module scans out what the SDL
// renderer produced — including the Dear ImGui chrome, since ImGui draws
// through the same renderer.
//
// Opt-in: set KONCPC_DRM=1. Absent (or on any non-Linux host) every entry
// point is a no-op, so desktop builds are unaffected.

struct SDL_Renderer;

bool drm_present_enabled();

// Read the current frame back from `r` and scan it out. Safe to call every
// frame; initialises lazily on first use. The renderer is passed in because
// video_host.cpp keeps it in an anonymous namespace (internal linkage).
void drm_present_frame(SDL_Renderer* r);

void drm_present_shutdown();
