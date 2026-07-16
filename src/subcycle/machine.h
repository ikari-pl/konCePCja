/* machine.h — the sub-cycle CPC as one embeddable object (Milestone B seam).
 *
 * Assembles the src/hw Devices (Gate Array, CRTC, PPI, PSG, memory, video,
 * Z80, FDC) into a complete 128K CPC 6128 and exposes the host-facing surface
 * an application loop needs: run one video frame, take keyboard rows, attach
 * media, pull stereo audio, drain the drive's mechanical events. This is the
 * sub-cycle side of the engine seam — the standalone sim (sim/koncepcja_sim)
 * and the konCePCja app both drive the machine through this class.
 *
 * Layering: src/hw stays pure, heap-free Devices; THIS layer is host C++ and
 * owns the Device storage. Contracts it inherits from the hw layer:
 *  - media buffers (ROMs, DSK, SCP) are CALLER-OWNED and must outlive their
 *    attachment (live wiring, docs/hardware/memory-device.md §2b et al.);
 *  - the framebuffer is caller-owned RGB24, w*h*3 (the full 768x272 monitor
 *    window unless the caller chooses otherwise);
 *  - audio is interleaved stereo s16 at 44 100 Hz, produced per frame. */
#ifndef KONCPC_SUBCYCLE_MACHINE_H
#define KONCPC_SUBCYCLE_MACHINE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "hw/amdrum.h"
#include "hw/amx.h"
#include "hw/asic.h"
#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/fdc.h"
#include "hw/light_gun.h"
#include "hw/m4.h"
#include "hw/mf2.h"
#include "hw/plotter_hp7470a.h"
#include "hw/printer.h"
#include "hw/probe.h"
#include "hw/rs232.h"
#include "hw/smartwatch.h"
#include "hw/symbiface.h"
#include "hw/tape.h"
#include "hw/video.h"
#include "hw/z80.h"

namespace subcycle {

// The native monitor window (48 chars x 16 px, 272 visible lines) and the
// audio contract. 16 MHz master / 50 Hz frames; PSG sampled at 1 MHz and
// box-filter-decimated to the host rate.
constexpr int kFbWidth = 768;
constexpr int kFbHeight = 272;
constexpr long kMasterPerFrame = 16000000L / 50;
// Char = 16 master cycles (1 MHz char clock). The Fast batch runs one frame,
// cut by a VSYNC edge; if none arrives (e.g. the post-reset window before the
// firmware reprograms the CRTC — a HALT with interrupts off then free-runs with
// no frame boundary) the batch must not run away. Cap it at the SAME span the
// per-cycle path allows a single run_frame (kMasterPerFrame*2 masters ÷ 16),
// then bail to per-cycle. A real frame VSYNC-cuts well under this (~20k chars).
constexpr uint64_t kMaxFastChars = (kMasterPerFrame * 2) / 16;
constexpr long kPsgHz = 1000000;
constexpr long kAudioHz = 44100;
constexpr int kAudioGain = 280;  // psg level 0..93 -> int16 with headroom

// Optional host audio overlay (drive sounds): the machine feeds it the FDC's
// mechanical events (motor/seek) so a host layer can react. It is events-only —
// the cosmetic audio is rendered on its own host SDL stream and mixed at the
// device, NEVER summed into the emulated AY output (which stays pure).
struct AudioOverlay {
  virtual ~AudioOverlay() = default;
  virtual void events(const FdcEvent* ev, int n) = 0;
};

class Machine {
 public:
  // Build the board from a 32K system ROM (OS + BASIC). Returns false if the
  // ROM is too short. Optional afterwards: attach_amsdos, insert_disk/flux.
  bool build(const uint8_t* rom, size_t rom_len);

  // Caller-owned RGB24 framebuffer (w*h*3). Re-attachable at any time.
  void attach_framebuffer(uint8_t* fb, int w, int h);

  // AMSDOS (or any 16K ROM) into upper-ROM slot 7. Caller-owned.
  void attach_amsdos(const uint8_t* rom16k, size_t len);
  // Any caller-owned 16K ROM into an arbitrary upper-ROM slot (e.g. the M4).
  void attach_rom(int slot, const uint8_t* rom16k);

  // Plus (6128+) cartridge: the parsed CPR image (caller-owned, 16K banks)
  // drives the low/high ROM banking (RMR2 + ROM-select). No-op on models 0-2.
  void attach_cartridge(const uint8_t* image, size_t bytes);

  // Drive media — caller-owned buffers, parsed in place (see fdc.h). `unit`
  // picks the drive: 0 = A (default), 1 = B. MUTABLE: FDC writes/FORMAT edit
  // the buffer in place (fdc-device.md §10); disk_dirty()/mark_disk_clean()
  // carry the host's persistence decision.
  bool insert_disk(uint8_t* dsk, size_t len, uint8_t unit = 0);
  bool disk_dirty() const;
  void mark_disk_clean();
  bool insert_flux(const uint8_t* scp, size_t len);
  void eject_disk(uint8_t unit = 0);

  // The cassette deck (caller-owned CDT; live wiring). The firmware owns the
  // motor relay through the PPI; PLAY is the user's button. Line-in mode
  // follows set_tape_line() — the host's Schmitt stage over mic samples.
  bool insert_tape(const uint8_t* cdt, size_t len);
  void eject_tape();
  void tape_play_button(bool on);
  void tape_rewind_deck();
  // Seek the deck to the Nth CDT block (walks the deck's cdt by block size).
  void tape_seek(uint32_t block_ordinal);
  // Drain decoded data bits for the host tape scope's BITS view (0/1, oldest
  // first); returns the count copied (<= max).
  int tape_drain_bits(uint8_t* out, int max);
  void tape_line_in(bool on);
  void set_tape_line(bool level);

  // Rate-clocked line-in: append one POST-SCHMITT level per audio sample; the
  // machine consumes them at sample_rate against the 16 MHz master clock
  // during run_frame, so transitions land with sub-frame (one-sample) timing.
  // Keeps ~2 frames of backlog; excess is dropped (live source, no rewind).
  void feed_line_levels(const uint8_t* levels, int count, int sample_rate);

  // Tape OUTPUT capture: while enabled, run_frame samples the cassette wires
  // at the audio rate — bit0 = data level (rdata to re-record a playing CDT,
  // or wdata to record the CPC's own SAVE), bit1 = motor relay. The host
  // renders these to the jack (data square + motor carrier), tape_line_in.h.
  void tape_out_capture(bool on, bool source_rdata);
  const std::vector<uint8_t>& tape_out_samples() const { return out_q_; }

  // Live cassette wires, for the host-side status scope + drive-sound overlay.
  // The sub-cycle bridge mirrors these back into the legacy bTapeLevel /
  // CPC.tape_motor that the tape UI reads (engine=1 bypasses the legacy
  // z80_OUT_handler + tape.cpp chain that used to drive them). Committed bus
  // state — read-only, no effect on the two-phase tick.
  bool tape_motor() const;       // motor-relay line (PPI bit 4)
  bool tape_read_level() const;  // RDATA: the level a playing CDT presents

  // Which 6845 variant is soldered in (0..3, docs/hardware/crtc-device.md §7).
  void set_crtc_type(uint8_t type);

  // Full device-state snapshot (length-prefixed per-device blobs + the
  // committed bus + master clock) — for DISPOSABLE measurement runs (the
  // tier benchmark): save, run frames, load, and the machine is byte-
  // identical to never having run them. Wiring/media pointers are untouched
  // (each device's load keeps them, per the Device contract).
  std::vector<uint8_t> save_devices() const;
  void load_devices(const std::vector<uint8_t>& blob);

  // Cold-boot the whole board (media and ROMs persist — they are wiring).
  void reset();

  // Keyboard: CPC matrix row (0..9) column byte, bit CLEAR = pressed; or one
  // key as the packed (row << 4 | bit) code used across the codebase.
  void set_key_row(uint8_t row, uint8_t columns);
  // Take (read + clear) the keyboard rows the firmware READ this frame — the
  // host relays these to its KeyboardManager as notify_scanned() so a held key
  // releases only once its row was scanned (BufferedUntilRead under engine=1).
  uint16_t take_key_scanned_rows();
  void key(uint8_t code, bool down);

  // Advance emulation until the video device completes one frame (bounded at
  // two frames' worth of master cycles). Fills the framebuffer and replaces
  // audio() with this frame's samples; feeds the overlay if attached.
  // Stops EARLY (mid-frame) when the bus probe latches a hit while armed —
  // check probe_hit() afterwards; ack + run again to continue the frame.
  void run_frame();

  // --- Runtime speed tiers (Gate B5/B6; plan: docs/plans/
  // 2026-07-09-001-feat-runtier-fast-plan.md) ------------------------------
  // All tiers run the SAME devices over the SAME state and master clock and
  // are observation-identical at frame boundaries — the harness (Gate B3)
  // proves each pairing. They differ only in dispatch:
  //   Faithful — the pluggable per-master-cycle fn-pointer loop (board_tick):
  //              composition-general, the home of cycle-stepping and pin-level
  //              observability.
  //   Soldered — per-cycle direct dispatch of the canonical 18 devices
  //              (tick_soldered): the devirtualization experiment, kept as a
  //              switchable mode and a harness comparator. No throughput edge
  //              over Faithful under LTO (Gate B4 finding).
  //   Wake     — THE DEFAULT: per-cycle, clock-wake scheduled — each chip runs
  //              only on its contract cycles, sleeping chips' bus lines held
  //              (~2.4x Faithful; Gate B6, beads-708f).
  //   Fast     — RESERVED (epic beads-kmzn): instruction-granularity catch-up
  //              scheduling targeting legacy-class-or-better speed. Until its
  //              driver lands (F2+), requesting Fast degrades to Wake.
  enum class RunTier : uint8_t { Faithful, Soldered, Wake, Fast };

  // Request a tier. Takes effect at the NEXT frame boundary (run_frame latches
  // it on entry — never mid-frame).
  void set_run_tier(RunTier tier) { tier_ = tier; }
  RunTier run_tier() const { return tier_; }
  // The tier that will actually run — composition-aware degradation: Soldered
  // needs the canonical 18-device board (soldered_available); Wake needs the
  // canonical core (wake_valid_, from recompose_active); Fast is not yet
  // implemented and degrades to Wake's resolution. Everything bottoms out at
  // Faithful, which runs any composition.
  RunTier effective_run_tier() const {
    switch (tier_) {
      case RunTier::Soldered:
        return soldered_available() ? RunTier::Soldered : RunTier::Faithful;
      case RunTier::Fast:
        // Fast v1 (F6): the canonical composition WITHOUT the ASIC (the Plus
        // compositor batches in F7); off-composition falls to Wake's ladder.
        if (fast_valid_) return RunTier::Fast;
        [[fallthrough]];
      case RunTier::Wake:
        return wake_valid_ ? RunTier::Wake : RunTier::Faithful;
      case RunTier::Faithful:
      default:
        return RunTier::Faithful;
    }
  }
  // Is the soldered path valid for the current board? True when the count
  // matches its frozen call list and the active set was not manually
  // overridden (KONCPC_ACTIVE). Automatic dormancy (recompose_active) does NOT
  // disqualify it: tick_soldered still lists all 18 and a dormant device's
  // tick early-returns, staying byte-identical to Faithful.
  bool soldered_available() const {
    return board_.count == kSolderedDevices && !active_override_;
  }

  // Devices actually dispatched per master cycle after dormancy exclusion
  // (recompose_active). < device count when unplugged peripherals are skipped.
  // Refreshed at each run_frame(); introspection for tests/UI, not a hot path.
  int active_tick_count() const { return board_.active_count; }
  // Frames the Fast batch driver actually completed — the engagement signal
  // (a Fast request can degrade per frame: gates, mid-frame bails).
  uint32_t fast_frames_run() const { return fast_frames_run_; }

  // Will the wake tier actually run (requested or defaulted, and the active
  // composition is the canonical core)?
  bool wake_active() const { return effective_run_tier() == RunTier::Wake; }

  // Wake-tier control (tests, GUI). `enabled` is a convenience over
  // set_run_tier(Wake / Faithful); `mask` arms per-device wake predicates
  // (bit clear = that device stays awake every cycle — the bisect hook):
  // 1=z80 2=crtc 4=psg 8=ppi 16=mem 32=video 64=tape 128=printer
  // 256=gate-array (synthesized pure-clock cycles) 512=fdc (quiet-skip).
  void set_wake(bool enabled, uint32_t mask = 0x3FF) {
    tier_ = enabled ? RunTier::Wake : RunTier::Faithful;
    wake_mask_ = mask;
  }

  // Cumulative 16 MHz master cycles since power-on — the deterministic clock a
  // record/replay driver tags input events against (record_replay.h, Gate B2).
  uint64_t master_cycle() const { return board_.master_cycles; }

  // Deterministic input-replay seam (record_replay.h): fn(ctx, master_cycle)
  // fires at the top of every run_frame master-cycle iteration, BEFORE the
  // tick, so a driver can apply queued input events at an exact cycle. At most
  // one is installed; pass nullptr to clear. Kept a bare fn-pointer (no
  // allocation, no per-cycle indirection when unset) to stay off the hot path.
  // Per-instruction observation seam (debug tracing). Fired once at each
  // instruction boundary, on BOTH the batch (Fast) and per-cycle tier paths,
  // with the CPU's architectural state. Null by default → zero overhead; the
  // host (subcycle_bridge) sets it only while `trace` is active, so the
  // per-instruction z80_peek is paid only when a trace is being recorded.
  using InstrHook = void (*)(void* ctx, const Z80Regs* regs);
  void set_instr_hook(InstrHook fn, void* ctx) {
    instr_hook_ = fn;
    instr_hook_ctx_ = ctx;
  }

  using CycleHook = void (*)(void* ctx, uint64_t master_cycle);
  void set_cycle_hook(CycleHook fn, void* ctx) {
    cycle_hook_ = fn;
    cycle_hook_ctx_ = ctx;
  }

  // --- Debug surface (Wave 1: DevTools/IPC read pin-level truth) ---

  // The ICE-style bus probe on the board (breakpoints/watchpoints/IO traps:
  // use the probe_* C API on this handle; docs/hardware/probe-device.md).
  const Device* probe() const { return &prdev_; }

  // Nonzero when the probe has latched; fills *out. Ack to resume matching.
  bool probe_hit(ProbeHit* out) const {
    return probe_pending(&prdev_, out) != 0;
  }
  void probe_resume() { probe_ack(&prdev_); }

  // Architectural Z80 state (pin-level truth via z80_peek/z80_poke).
  Z80Regs regs() const;
  void set_regs(const Z80Regs& regs);

  // The CPU-visible memory view (active ROM overlays + RAM banking); writes
  // land in the banked RAM byte, never ROM — like a real mreq cycle.
  uint8_t peek_mem(uint16_t addr) const;
  void poke_mem(uint16_t addr, uint8_t val);

  // Run until exactly one more instruction completes (bounded; ignores the
  // probe — stepping past a breakpoint must not immediately re-trip it).
  void step_instruction();

  // Firmware-vector taps: fn(ctx, addr) runs synchronously the master cycle
  // an M1 fetch lands on addr — registers still hold the caller's values
  // (telnet TXT_OUTPUT/BDOS hooks ride here). Up to 4; never halts.
  using TapFn = void (*)(void* ctx, uint16_t addr);
  bool add_tap(uint16_t addr, TapFn fn, void* ctx);
  void clear_taps();

  // DMA-style bus mastering: drive ONE I/O write cycle with the CPU off the
  // bus (snapshot restore replays chip state the way a loader would). Only
  // the passive chips see the cycle; their clocks do not advance.
  void io_write(uint16_t port, uint8_t val) const;

  // PSG register file / selection, directly (snapshot restore).
  void psg_poke(uint8_t reg, uint8_t val);
  void psg_select(uint8_t sel);

  // PHYSICAL RAM (base 64K then expansion), independent of banking — the
  // memory layout snapshots dump and restore.
  size_t ram_size() const;
  uint8_t ram_read(size_t addr) const;
  void ram_write(size_t addr, uint8_t val);

  // DK'Tronics Silicon Disc (silicon-disc-device.md): battery-backed RAM at
  // expansion banks 4-7 — a sizing + persistence policy, NOT a bus Device.
  // The region is [256K, 512K) of the expansion; the host owns the file.
  static constexpr size_t kSiliconSize = 256u * 1024;
  static constexpr size_t kSiliconStart = 256u * 1024;                 // bank 4
  static constexpr size_t kSiliconEnd = kSiliconStart + kSiliconSize;  // 512K
  void enable_silicon_disc(bool on);
  void silicon_disc_load(const uint8_t* src, size_t len);
  void silicon_disc_save(uint8_t* dst, size_t len) const;

  // This frame's interleaved stereo s16 samples (valid until the next run).
  const std::vector<int16_t>& audio() const { return audio_; }

  // Host audio overlay (drive sounds). May be null. Not owned.
  void set_overlay(AudioOverlay* overlay) { overlay_ = overlay; }

  // Drain the FDC's mechanical event ring directly (when no overlay is set).
  int drain_fdc_events(FdcEvent* out, int max);

  // Device handles for peeks / DevTools / tests.
  const Device* gate_array() const { return &gdev_; }
  const Device* mem() const { return &mdev_; }
  const Device* crtc() const { return &cdev_; }
  const Device* ppi() const { return &pdev_; }
  const Device* psg() const { return &sdev_; }
  const Device* memory() const { return &mdev_; }
  const Device* video() const { return &vdev_; }
  const Device* z80() const { return &zdev_; }
  const Device* fdc() const { return &fdev_; }
  const Device* tape() const { return &tdev_; }
  const Device* printer() const { return &prtdev_; }
  const Device* amdrum() const { return &addev_; }

  // Analog-domain DACs (printer-device.md §3, amdrum-device.md §2): the
  // host decides whether a Digiblaster hangs off the printer connector and
  // whether an AmDrum sits on the expansion port.
  void set_digiblaster(bool on) { digiblaster_ = on; }
  void set_amdrum(bool plugged) { amdrum_set_plugged(&addev_, plugged); }

  // Multiface II (multiface-device.md): 8K ROM is caller-owned live wiring.
  const Device* mf2() const { return &mfdev_; }
  void attach_mf2_rom(const uint8_t* rom8k, size_t len) {
    mf2_attach_rom(&mfdev_, rom8k, len);
  }
  void set_mf2(bool plugged) { mf2_set_plugged(&mfdev_, plugged); }
  void mf2_stop_button() { mf2_stop(&mfdev_); }

  // M4 Board (m4-device.md): 16K ROM is caller-owned wiring; commands defer
  // to the host between frames (m4_pending_command / m4_complete_response).
  const Device* m4() const { return &m4dev_; }

  // Plus/6128+ ASIC (asic-device.md): live only on model=3. Locked until a
  // Plus program sends the unlock knock; totally inert while unplugged.
  const Device* asic() const { return &adev_; }
  void set_asic(bool plugged) { asic_set_plugged(&adev_, plugged); }
  void set_m4(bool plugged) { m4_set_plugged(&m4dev_, plugged); }
  void set_m4_slot(int slot) { m4_set_slot(&m4dev_, slot); }
  void attach_m4_rom(const uint8_t* rom16k, size_t len) {
    m4_attach_rom(&m4dev_, rom16k, len);
  }
  int m4_pending(M4Pending* out) { return m4_pending_command(&m4dev_, out); }
  void m4_respond(const uint8_t* buf, uint16_t len) {
    m4_complete_response(&m4dev_, buf, len);
  }
  void m4_config(const uint8_t* buf, uint16_t len) {
    m4_write_config(&m4dev_, buf, len);
  }

  // AMX mouse (amx-mouse-device.md): whole mickeys + buttons from the host.
  const Device* amx() const { return &axdev_; }
  void set_amx_mouse(bool plugged) { amx_set_plugged(&axdev_, plugged); }
  void amx_mouse_feed(int dx, int dy, uint8_t buttons) {
    amx_feed(&axdev_, dx, dy, buttons);
  }

  // Dobbertin SmartWatch (smartwatch-device.md): host feeds DS1216 BCD time.
  const Device* smartwatch() const { return &swdev_; }
  void set_smartwatch(bool plugged) {
    smartwatch_set_plugged(&swdev_, plugged);
  }
  void set_smartwatch_time(const uint8_t bcd[8]) {
    smartwatch_set_time(&swdev_, bcd);
  }

  // The RS232 card + HP 7470A plotter pair (rs232-device.md,
  // plotter-device.md): plugged as a unit — the card is the CPC end of the
  // wire, the plotter the far end. Plugging them degrades the effective tier
  // to Faithful (recompose_active: no wake contract yet), the Symbiface
  // precedent. The plotter's DIP-fixed rate derives from the configured baud
  // (divisor = 2 MHz / baud / 16, floor 1); the card's rate is programmed by
  // the CPC through its own 8253.
  const Device* rs232_card() const { return &rsdev_; }
  const Device* plotter() const { return &pldev_; }
  void set_serial_plotter(bool plugged, uint32_t baud) {
    rs232_set_plugged(&rsdev_, plugged);
    plotter_hp7470a_set_plugged(&pldev_, plugged);
    if (plugged) {
      uint32_t divisor = baud ? 2000000u / (baud * 16u) : 13u;
      if (divisor == 0) divisor = 1;
      plotter_hp7470a_set_baud_divisor(&pldev_, static_cast<uint16_t>(divisor));
    }
  }
  void set_serial_card(bool plugged) { rs232_set_plugged(&rsdev_, plugged); }
  void serial_host_rx(uint8_t byte) { rs232_host_rx(&rsdev_, byte); }
  void set_serial_host_tx(void (*fn)(uint8_t, void*), void* ctx) {
    rs232_set_host_tx(&rsdev_, fn, ctx);
  }

  // The light gun (Magnum Phaser / Trojan — light-gun-device.md). Its LPEN
  // strobe drives the CRTC light-pen latch; the aim is in CRTC beam space
  // (scanline + active char column), converted from the host mouse by the
  // bridge. type 0 unplugs it; plugging degrades the effective tier to
  // Faithful (recompose_active: no wake contract — the serial precedent).
  const Device* light_gun() const { return &lgdev_; }
  void set_light_gun(int type, uint16_t aim_line, uint16_t aim_col,
                     bool pressed) {
    light_gun_set_type(&lgdev_, type);
    light_gun_set_aim(&lgdev_, aim_line, aim_col);
    light_gun_set_trigger(&lgdev_, pressed);
  }

  // Symbiface II (symbiface-device.md): IDE images are caller-owned mutable
  // buffers; the RTC clock and mouse packets are host-fed.
  const Device* symbiface() const { return &sfdev_; }
  void set_symbiface(bool plugged) { sf2_set_plugged(&sfdev_, plugged); }
  // NOLINTNEXTLINE(readability-non-const-parameter): pointer written through a
  // cast or passed to a non-const callee
  void symbiface_attach_ide(int drive, uint8_t* img, size_t len) {
    sf2_ide_attach(&sfdev_, drive, img, len);
  }
  void symbiface_detach_ide(int drive) { sf2_ide_detach(&sfdev_, drive); }
  bool symbiface_dirty() const { return sf2_media_dirty(&sfdev_) != 0; }
  void symbiface_mark_clean() { sf2_media_mark_clean(&sfdev_); }
  void symbiface_rtc_time(const uint8_t regs10[10]) {
    sf2_rtc_set_time(&sfdev_, regs10);
  }
  void symbiface_mouse_packet(uint8_t pkt) {
    sf2_mouse_push_packet(&sfdev_, pkt);
  }
  Board* board() { return &board_; }

 private:
  int16_t emit_sample(long acc, double& dc_x, double& dc_y) const;

  // run_frame's per-master-cycle work, split into named steps (each inlines
  // at -O2). They read/write the machine's live state directly.
  void capture_tape_output();  // sample the cassette wires at the audio rate
  void feed_tape_line_in();    // clock one queued live line-in level in
  void service_taps(
      const Bus& committed);  // fire firmware-vector taps this cycle
  void accumulate_audio();    // one PSG sample + analog DACs + stereo emit
  void accumulate_audio_bulk(uint32_t k);  // k level-stable steps at once —
                                           // same arithmetic, boundary-exact
  // The master-cycle tick with every device called by hardcoded direct name
  // instead of the fn-pointer array dispatch — models a fixed "soldered" board
  // and lets the compiler inline each tick. This is the Fast tier (Gate B5);
  // run_frame selects it at runtime when effective_run_tier() == Fast. Mirrors
  // board_tick() exactly (same order + two-phase commit) so it is OBSERVATION-
  // identical — the differential harness (Gate B3) proves it.
  void tick_soldered();

  // Shadow of the keyboard matrix (the PSG holds the live copy): lets key()
  // do per-bit updates without read-back. 0xFF = no keys pressed.
  uint8_t shadow_[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                         0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  std::vector<uint8_t> gmem_, cmem_, pmem_, smem_, mmem_, vmem_, zmem_, fmem_,
      prmem_, prtmem_, admem_, mfmem_, axmem_, swmem_, sfmem_, m4mem_, asmem_,
      tmem_, rsmem_, plmem_, lgmem_;
  std::vector<uint8_t> xmem_;  // 64K expansion RAM: a true 128K 6128
  // Writable-flux DSK overlay (Stage 2): synthesized from the SCP at insert and
  // owned here, it is the FDC's mutable `image`; the caller's SCP stays the
  // pristine source. Empty when drive A holds a DSK or a read-only flux dump.
  std::vector<uint8_t> flux_dsk_;
  Device gdev_{}, cdev_{}, pdev_{}, sdev_{}, mdev_{}, vdev_{}, zdev_{}, fdev_{},
      prdev_{}, tdev_{}, prtdev_{}, addev_{}, mfdev_{}, axdev_{}, swdev_{},
      sfdev_{}, m4dev_{}, adev_{}, rsdev_{}, pldev_{}, lgdev_{};
  Board board_{};
  bool built_ = false;
  // Set when KONCPC_ACTIVE manually pins the active-device count. It disables
  // both automatic dormancy (recompose_active) and the soldered fast path,
  // since an arbitrary manual prefix may drop a non-inert device.
  bool active_override_ = false;

  // Rebuild board_.tick_order/active_count to skip structurally-dormant devices
  // (an unplugged peripheral whose tick early-returns). Called at frame start —
  // dormancy only changes at composition/plug points. Observation-neutral by
  // construction: a dormant device drives nothing and mutates nothing.
  void recompose_active();

  // --- Wake tier (Gate B6, beads-708f) -----------------------------------
  // One master cycle under clock-wake scheduling: same devices, same two-phase
  // commit as board_tick/tick_soldered, but each chip is dispatched only on the
  // cycles its wake contract allows, and a sleeping chip's owned bus lines are
  // held from the committed bus (the board-level generalization of the Z80's
  // internal drive-and-hold). See the contract table at the definition.
  //
  // wake_slot<P> is the ONE schedule definition, specialized on the visible
  // clk.phase P so the static parts (Z80 clock slots, fetch slots, video
  // consume slots, the GA's synthesized phase) constant-fold. tick_wake() is
  // the phase-generic entry (a 16-way switch); run_frame's µs-chunk fast path
  // calls the slots directly in sequence.
  // `cur` is the committed bus this cycle reads; `next` is rest-initialized
  // and driven here, committed by the CALLER (tick_wake copies it into
  // board_.bus; run_wake_us ping-pongs two locals and writes back once).
  template <int P>
  void wake_slot(const Bus& cur, Bus& next);
  void tick_wake();
  // One µs (16 slots) of the chunked fast path, with the per-slot frame-exit
  // check. Returns the number of master cycles executed (16, or fewer when the
  // frame completed mid-chunk).
  int run_wake_us(VideoRegs& vr, uint32_t target, bool& vsync_seen);
  // The wake tier is THE DEFAULT (doctrine: "one faithful simulator + fast
  // modes"): pin-exact, ~2.4x Faithful, degrading automatically off the
  // canonical composition (wake_valid_). set_wake(false) or KONCPC_WAKE=off
  // selects the per-cycle tiers (Faithful, or the soldered experiment via
  // set_run_tier).
  // (Tier selection lives in tier_ below; Wake is the default.)
  uint32_t wake_mask_ = 0x3FF;  // bit n = device n uses its predicate (else it
                                // stays awake every cycle) — the bisect hook
  bool wake_valid_ = false;     // active set is the canonical core (recompose)
  // Transient scheduler shadows — wake-predicate state, NOT machine state.
  // wk_force_ wakes everything for one cycle; run_frame sets it each frame
  // start so host-side mutations between frames (pokes, key rows, snapshot
  // loads, deck buttons) can never be missed by a stale shadow.
  bool wk_force_ = true;
  uint16_t wk_addr_ = 0;    // previous committed cpu.addr
  uint8_t wk_flags_ = 0;    // previous committed cpu control-line pack
  bool wk_motor_ = false;   // previous committed tape.motor
  uint16_t wk_ay_ = 0;      // previous committed AY control pack (bdir/bc1/row)
  uint8_t wk_ay_da_ = 0;    // previous committed ay.da
  bool wk_z80_ran_ = true;  // Z80 ran last cycle (cpu lines may have moved;
                            // starts true so the first cycle checks)
  bool wk_crtc_woke_ = false;  // CRTC ran last cycle (vid.* may have changed)
  // Two flags per pipeline device: *_woke_ arms the one-cycle publish rewake
  // and records only the CAUSED wake (arming it from the wake itself would
  // feed the predicate back and hold the device awake forever); *_ran_ records
  // any run last cycle and gates the input compares (outputs visible now can
  // only differ if the driver actually ran).
  bool wk_ppi_woke_ = false;   // PPI had a caused wake last cycle → publish
  bool wk_psg_woke_ = false;   // PSG had a caused wake last cycle → publish
  bool wk_ppi_ran_ = false;    // PPI ran last cycle (ay/tape outputs may move)
  bool wk_psg_ran_ = false;    // PSG ran last cycle (ay.da may move)
  bool wk_mem_woke_ = false;   // MEM ran during a write strobe → keep it awake
                               // (its one-tick write latch commits under the
                               // NEXT cycle's /RAMDIS)
  bool wk_tape_woke_ = false;  // deck ran last cycle (rdata may have changed)
  bool wk_tape_live_ = false;  // frame-start cache: PLAY pressed or line-in
  bool wk_probe_on_ = false;   // frame-start cache: probe armed (taps/bps)
  bool wk_asic_on_ = false;    // frame-start cache: ASIC plugged (model 3)
  uint64_t wk_prt_skip_ = 0;   // printer cycles skipped since its last tick
  uint64_t wk_ga_skip_ = 0;    // GA cycles synthesized since its last tick
  bool wk_fdc_quiet_ = false;  // fdc_quiet() as of its last tick / frame start
  uint64_t wk_fdc_skip_ = 0;   // FDC cycles skipped since its last tick
  // RS232 + HP7470 plotter wake contract (the bit-serial pair). No skip
  // counter: neither UART runs a free-running per-cycle counter while quiet
  // (tx/rx timers reset on byte start; the plotter's input drain only runs
  // while in_count > 0), so a quiet-skip is byte-identical with no advance()
  // catch-up.
  bool wk_serial_on_ = false;  // rs232/plotter plugged this frame (contract on)
  bool wk_serial_quiet_ = true;  // rs232_quiet && plotter_quiet as of last tick
  bool wk_serial_io_prev_ = false;  // serial I/O ran last cycle → self-rewake
                                    // so the DART/PIT see the select strobe
                                    // drop and reset their access edge (else
                                    // only the first OUT of a burst registers).

  // Number of devices the soldered fast path (tick_soldered) calls by name. The
  // fast tier is valid only when the board's device set matches this (see
  // soldered_available()). build() always adds exactly this many.
  static constexpr int kSolderedDevices = 21;
  RunTier tier_ = RunTier::Wake;  // THE default; frame-boundary swap only

  // --- Fast tier (F6, epic beads-kmzn): instruction-granularity catch-up
  // scheduling over the batch seams (z80_batch_step + Z80BatchIO; the F4/F5
  // driver-let promoted). run_frame enters it mid-frame at the first clean
  // point (Z80 boundary + committed phase 0) and it covers the frame's
  // remainder; exit re-materializes a per-cycle-resumable machine. All fs_*
  // time is relative to the entry (fs_t0_); frame-quiet gates (tape idle, no
  // probe/taps/capture/hook, FDC quiet at entry) are checked per frame. ---
  bool fast_valid_ = false;  // canonical composition, ASIC dormant (recompose)
  uint64_t fs_t0_ = 0;       // z80 tstates at entry (≡ 0 mod 4 on the grid)
  uint64_t fs_chars_ = 0;    // eager CRTC+GA position (µs since entry)
  uint64_t fs_cells_ = 0;    // lazy renderer position
  // Audio rides two counters with the ACCUMULATE trailing the STEP by up to
  // one: chan_level changes only at sound steps, so unit k's accumulate is
  // valid anywhere between step k and step k+1 — deferring it until just
  // before step k+1 (or the exit check) means a frame cut can leave it for
  // the resumed per-cycle loop instead of double-counting it at the seam.
  uint64_t fs_audio_steps_ = 0;
  uint64_t fs_audio_accs_ = 0;
  uint64_t fs_fdc_done_ = 0;  // FDC master-cycle cursor (rel)
  uint64_t fs_prt_done_ = 0;  // printer master-cycle cursor (rel)
  bool fs_fdc_hot_ = false;   // FDC left its quiet contract mid-frame
  bool fs_cut_ = false;       // the frame-completing VSYNC rise was advanced
  bool fs_asic_on_ = false;   // entry cache: the ASIC is plugged (Plus)
  bool fs_bail_ = false;      // leave the frame to the per-cycle loop (a
                              // mid-frame DMA enable — F7 scope note)
  uint32_t fs_target_ = 0;    // video frames target this run
  uint32_t fast_frames_run_ = 0;  // frames the batch driver completed (an
                                  // engagement signal: tests + the tier UI)
  uint32_t fs_frames_seen_ = 0;   // frames at entry + eager-counted rises
  // Views [fs_cells_, fs_chars_) live at [fs_pend_head_, fs_pend_tail_) of a
  // flat per-frame buffer (indices reset at frame entry; capacity grown, never
  // shrunk). The F8 profile showed vector resize() zero-init and erase-front
  // memmove both measurably hot — a consumed-prefix cursor costs neither.
  std::unique_ptr<CrtcCharView[]> fs_pend_buf_;
  size_t fs_pend_cap_ = 0;
  size_t fs_pend_head_ = 0;
  size_t fs_pend_tail_ = 0;
  uint8_t fs_vpages_ = 0;  // 16K pages the chain fetched this frame (bit/page)
  const uint8_t* fs_vram_ = nullptr;
  // IRQ horizon (F8): boundaries at rel T-state <= fs_irq_tmax_ may reuse
  // fs_irq_cache_ instead of advancing the chain and re-polling — the CRTC
  // guarantees no INT-path event before the horizon char (crtc.h). Invalidated
  // (tmax = 0) by any applied I/O write, an INT acknowledge, and the HALT
  // wait path — the three places the line can move off-schedule.
  uint64_t fs_irq_tmax_ = 0;
  int fs_irq_cache_ = 0;

  static uint64_t fs_visible(uint64_t tstate) {  // chars visible at a boundary
    return tstate == 0 ? 0 : ((tstate - 1) / 4) + 1;
  }
  void fs_advance_chars(uint64_t target);
  void fs_render_below(uint64_t target);
  void fs_audio_steps(uint64_t steps);
  bool fs_irq() const;
  void fs_fdc_to(uint64_t rel_master);
  void fs_io_write_event(uint16_t port, uint8_t val, uint64_t rel_t1);
  uint8_t fs_io_read_event(uint16_t port, uint64_t rel_sample);
  // Z80BatchIO trampolines (ctx = this).
  static uint8_t fsb_mem_read(void* ctx, uint16_t addr, uint64_t now);
  static void fsb_mem_write(void* ctx, uint16_t addr, uint8_t val,
                            uint64_t now);
  static uint8_t fsb_io_read(void* ctx, uint16_t port, uint64_t now);
  static void fsb_io_write(void* ctx, uint16_t port, uint8_t val, uint64_t now);
  static uint8_t fsb_int_ack(void* ctx, uint64_t now);
  // Run the frame's remainder under the Fast tier from a clean entry point.
  // Returns false (leaving state per-cycle-consistent) only if nothing ran.
  bool run_frame_fast(VideoRegs& vr, uint32_t target);

  struct Tap {
    uint16_t addr;
    TapFn fn;
    void* ctx;
  };
  Tap taps_[4] = {};
  int tap_count_ = 0;
  // Edge state for the cheap console-tap path: taps fire on the rising edge of
  // the M1 opcode-fetch strobe, checked directly on the committed bus instead
  // of arming the per-cycle debug probe (see service_taps).
  bool tap_prev_fetch_ = false;

  bool out_capture_ = false;
  bool out_src_rdata_ = true;
  bool digiblaster_ = false;  // mix the printer latch as a DAC
  bool amdrum_on_ = false;    // frame-start cache of the AmDrum plug state —
                              // spares accumulate_audio a 1 MHz-rate peek on
                              // the common unplugged machine (all tiers)
  long out_acc_ = 0;
  std::vector<uint8_t> out_q_;  // this frame's outbound wire samples

  std::vector<uint8_t> line_q_;  // rate-clocked line-in levels (FIFO)
  size_t line_q_pos_ = 0;
  long line_acc_ = 0;
  int line_rate_ = 44100;

  std::vector<int16_t> audio_;
  long au_phase_ = 0, au_accL_ = 0, au_accR_ = 0;
  int au_n_ = 0;
  double dcL_x_ = 0, dcL_y_ = 0, dcR_x_ = 0, dcR_y_ = 0;
  AudioOverlay* overlay_ = nullptr;

  CycleHook cycle_hook_ = nullptr;  // per-cycle input-replay seam (Gate B2)
  void* cycle_hook_ctx_ = nullptr;
  InstrHook instr_hook_ = nullptr;  // per-instruction debug-trace seam
  void* instr_hook_ctx_ = nullptr;
  uint64_t instr_hook_last_ = 0;  // last-seen instr_count (per-cycle edge)
};

}  // namespace subcycle

#endif /* KONCPC_SUBCYCLE_MACHINE_H */
