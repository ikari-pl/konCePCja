#pragma once

// The core's side of the frame ring (kcpc/fb_ring.h): one shared-memory
// region per process, named fb_ring_name(pid), that the frame runner fills
// with the canonical frame (RGB24, subcycle::kFbWidth x kFbHeight) at every
// frame completion, whether or not anybody reads it (user decision
// 2026-10-26).
//
// Start it before the frame runner runs and stop it after the frame runner
// has ended (doCleanUp(), which the signal exit goes through as well): the
// frame runner publishes without a lock.

#include <cstdint>

// Creates the region. On failure it logs why and the core runs on without
// publishing. Idempotent.
bool fb_publish_start();

// The frame runner, at a frame completion, before the host blit: copies the
// whole frame into the next slot. A no-op when the ring is not running.
void fb_publish_frame(const std::uint8_t* rgb24);

// Unmaps the region and (on POSIX) unlinks its name. Idempotent.
void fb_publish_stop();
