/* subcycle_bridge.h — the sub-cycle engine inside the konCePCja app
 * (Milestone B seam, step 2).
 *
 * The Z80 thread calls subcycle_bridge_frame(): one whole 50 Hz frame of
 * the src/subcycle machine, blitted into the app's back_surface and
 * returning the frame's audio for the SDL push path. Keyboard rows come
 * from the app's published matrix, so every input path (SDL, autotype,
 * IPC, session replay) drives the core unchanged. Since Gate C Wave 1 this
 * is the ONLY engine — the host view structs (t_z80regs, t_CRTC, ...) are
 * dumb buffers the bridge publishes Device truth into. */
#ifndef KONCPC_SUBCYCLE_BRIDGE_H
#define KONCPC_SUBCYCLE_BRIDGE_H

#include <cstdint>
#include <vector>

struct SDL_Surface;

/* Build the machine from the app's configuration (CPC.rom_path + model ROM,
 * amsdos.rom, CPC.driveA.file when set). Returns false if the system ROM
 * cannot be loaded — fatal: there is no other engine. */
bool subcycle_bridge_start();

/* True after a successful start (the Z80 thread's dispatch check). */
bool subcycle_bridge_active();

// Take (read + clear) the keyboard rows the sub-cycle firmware READ this frame,
// so the host relays them to its KeyboardManager as notify_scanned() (the
// engine=1 equivalent of the legacy per-read notify — powers BufferedUntilRead).
uint16_t subcycle_bridge_scanned_key_rows();

/* Sub-cycle FDC drive activity LEDs: lit for the unit the last command selected
 * while the motor latch is on (mirrors the legacy FDC.led surface for the
 * status bar). Both false when the bridge is inactive. */
void subcycle_bridge_disk_leds(bool& drive_a, bool& drive_b);

/* Cold-boot the machine (media persists — it is wiring). */
void subcycle_bridge_reset();

void subcycle_bridge_stop();

/* Run one emulated frame: rows[16] is the published keyboard matrix (bit
 * clear = pressed); dst receives the picture (scaled blit, may be null);
 * limit paces to the 50 Hz wall clock with drift correction. Returns the
 * frame's interleaved stereo s16 44 100 Hz samples. */
const std::vector<int16_t>& subcycle_bridge_frame(const uint8_t rows[16],
                                                  SDL_Surface* dst, bool limit);

/* Re-blit the CURRENT framebuffer without running a frame (IPC "repaint"). */
void subcycle_bridge_repaint(SDL_Surface* dst);

/* Media hot-swap. Thread-safe: callable from the main/UI thread
 * while the emulation runs — the swap is deferred and applied by the Z80
 * thread at the next frame boundary (the FDC's media is live wiring; it must
 * not change mid-tick). `flux` selects SCP interpretation; bytes are moved
 * into the bridge, which keeps them alive for the machine. No-ops when the
 * engine is inactive. */
void subcycle_bridge_insert_media(std::vector<uint8_t> bytes, bool flux,
                                  uint8_t unit = 0);
void subcycle_bridge_eject_media(uint8_t unit = 0);

/* The Multiface II STOP button (deferred to the frame boundary). */
void subcycle_bridge_mf2_stop();

/* Cassette hot-insert (drive-A pattern: queued, applied at frame boundary). */
void subcycle_bridge_insert_tape(std::vector<uint8_t> bytes);

/* Cassette eject (queued like the insert; the deck is live wiring). */
void subcycle_bridge_eject_tape();

/* Deferred tape block-seek: the tape UI (render thread) requests a jump to the
 * Nth CDT block; the Z80 thread applies it at the next frame boundary (the deck
 * is live wiring). The deck walks its own cdt to the ordinal — layout-independent. */
void subcycle_bridge_request_tape_seek(uint32_t block_ordinal);

/* --- Debug view sync ---
 * The host `z80` struct is a dumb VIEW BUFFER: the bridge mirrors the
 * machine's pin-level truth into it each frame, so every debug surface
 * (IPC, DevTools, telnet status) reads Device truth, and the breakpoint/
 * watchpoint editing lists mirror INTO the bus probe. */

namespace subcycle {
class Machine;
}
/* The live machine, or null when the engine is inactive. */
subcycle::Machine* subcycle_bridge_machine();

/* The live FDC Device (drive media truth for Save-As), or null when the engine
 * is inactive. A lean handle so flux_save.cpp need not pull in machine.h. */
struct Device;
const Device* subcycle_bridge_fdc();

// --- Run-tier policy (F9, beads-3wyl; user decision 2026-07-10) -----------
// Auto = RunTier::Fast, dropping to Wake for as long as the debugger is
// engaged (any breakpoint / watchpoint / IO breakpoint set — the per-cycle
// tiers own pin-level observability), returning to Fast when the last one
// clears. Applied at frame boundaries only. A KONCPC_TIER / KONCPC_WAKE
// environment variable pins the machine's tier and disables the policy
// entirely (the bench and the §8.3 harness drive tiers by env).
enum class BridgeTierPolicy : std::uint8_t {
  Auto = 0,
  Fast = 1,
  Wake = 2,
  Soldered = 3,
  Faithful = 4,
};
void subcycle_bridge_set_tier_policy(BridgeTierPolicy policy);
BridgeTierPolicy subcycle_bridge_tier_policy();
// engine=1 CPU trace: attach/detach the per-instruction record hook that feeds
// g_trace. No-op sink when the sub-cycle engine is inactive (legacy uses its own
// z80.cpp call site). Setting the hook also suppresses the µs-chunk elision so
// every retired instruction is captured under the per-cycle tiers.
void subcycle_bridge_set_instr_trace(bool on);
// Nonzero when an env var pins the tier (the policy UI should say so).
int subcycle_bridge_tier_env_pinned();
// The tier the machine resolved for the CURRENT frame, as a display name
// ("fast", "wake", ...), after policy, env, and composition degradation.
const char* subcycle_bridge_effective_tier_name();
// The same, as the user-facing display label ("Performance", "Balanced",
// "Microscope", "Microscope, socketed chips"). IPC keeps the short tokens.
const char* subcycle_bridge_effective_tier_label();

// Async per-tier throughput benchmark (menu "up to X FPS"). request() arms
// it (idempotent while running; re-arms after completion for fresh numbers);
// the Z80 thread then runs ~12 ms slices of snapshot-wrapped frames per tier
// ahead of real frames until each tier has its sample. fps(tier 0..3 =
// fast/wake/soldered/faithful) returns -1 while unmeasured.
void subcycle_bridge_bench_request();
int subcycle_bridge_bench_fps(int tier);
// Tier index currently being sampled (0..3), or -1 when idle/done.
int subcycle_bridge_bench_running();

/* Frame-boundary sync: legacy breakpoint/watchpoint lists -> probe
 * comparators; machine registers -> the legacy view struct; a latched probe
 * hit -> legacy breakpoint/watchpoint flags + the IPC hit hook (latch acked:
 * the fetch edge is consumed, so resuming continues mid-instruction and
 * never re-trips). Returns nonzero when a hit was consumed (the caller maps
 * it to EC_BREAKPOINT). */
int subcycle_bridge_debug_sync();

// Mirror the legacy breakpoint/watchpoint/IO lists into the probe (the
// editing model -> the firing truth) and refresh the tier policy's
// debug-engaged signal. debug_sync runs it after each frame; the emulation
// loop runs it again immediately before each frame so list edits made while
// paused at a hit take effect on the resumed frame (beads-4gf9).
void subcycle_bridge_sync_probe();

/* Pull the ASIC Device's debug state into the host asic view (the DevTools
 * ASIC window and the IPC `asic` dumps read it). On-demand — sprites + the
 * 16K register-page snapshot are too heavy for the per-frame sync. */
void subcycle_bridge_refresh_asic_view();

/* Machine registers -> the legacy view struct only (used after stepping). */
void subcycle_bridge_sync_regs_view();

/* The legacy struct -> machine (after IPC/DevTools write a register). */
void subcycle_bridge_regs_to_machine();

#endif /* KONCPC_SUBCYCLE_BRIDGE_H */
