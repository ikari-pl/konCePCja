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
#include <string>

// Creates the region. On failure it logs why and the core runs on without
// publishing. Idempotent.
bool fb_publish_start();

// The frame runner, at a frame completion, before the host blit: copies the
// whole frame into the next slot. A no-op when the ring is not running.
void fb_publish_frame(const std::uint8_t* rgb24);

// Unmaps the region and (on POSIX) unlinks its name. Idempotent.
void fb_publish_stop();

// Any thread. The sequence number of the latest frame published (1, 2, ...),
// 0 before the first or when the ring is not running: what FB_INFO's fb_seq
// and EVT_FRAME report (beads-0ir.3).
std::uint64_t fb_publish_seq();

// Any thread. The OS name of the region (shm::os_name()), what FB_INFO's
// shm_name reports; empty when the ring is not running.
std::string fb_publish_shm_name();
