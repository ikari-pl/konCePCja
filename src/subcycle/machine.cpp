/* machine.cpp — the embeddable sub-cycle CPC (see machine.h). Lifted from the
 * standalone sim's Machine and given the host-agnostic contract: byte-based
 * media, caller-owned framebuffer, overlay hook for drive sounds. */

#include "machine.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/flux.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/ppi.h"
#include "hw/psg.h"
#include "hw/video.h"
#include "hw/z80.h"

namespace subcycle {

bool Machine::build(const uint8_t* rom, size_t rom_len) {
  if (rom == nullptr || rom_len < 0x8000) return false;

  board_init(&board_);
  // One line per chip: size its caller-owned storage, construct its Device
  // view, and wire it onto the board. Init and board_add on the same line so
  // they cannot drift; board order does not affect results (docs/hw-spec §7).
  auto add = [&](std::vector<uint8_t>& mem, Device& dev, size_t (*size_fn)(),
                 Device (*init_fn)(void*)) {
    mem.resize(size_fn());
    dev = init_fn(mem.data());
    board_add(&board_, dev);
  };
  add(gmem_, gdev_, ga_state_size, ga_init);
  add(cmem_, cdev_, crtc_state_size, crtc_init);
  add(pmem_, pdev_, ppi_state_size, ppi_init);
  add(smem_, sdev_, psg_state_size, psg_init);
  add(mmem_, mdev_, mem_state_size, mem_init);
  add(vmem_, vdev_, video_state_size, video_init);
  add(zmem_, zdev_, z80_state_size, z80_init);
  add(fmem_, fdev_, fdc_state_size, fdc_init);
  add(prmem_, prdev_, probe_state_size, probe_init);            // ICE bus probe
  add(tmem_, tdev_, tape_state_size, tape_init);                // cassette deck
  add(prtmem_, prtdev_, printer_state_size, printer_init);      // printer latch
  add(admem_, addev_, amdrum_state_size, amdrum_init);          // AmDrum DAC
  add(mfmem_, mfdev_, mf2_state_size, mf2_init);                // Multiface II
  add(axmem_, axdev_, amx_state_size, amx_init);                // AMX mouse
  add(swmem_, swdev_, smartwatch_state_size, smartwatch_init);  // SmartWatch
  add(sfmem_, sfdev_, sf2_state_size, sf2_init);                // Symbiface II
  add(m4mem_, m4dev_, m4_state_size, m4_init);                  // M4 Board
  add(asmem_, adev_, asic_state_size, asic_init);               // Plus ASIC
  // The serial pair, appended last so every earlier device keeps its build
  // index (recompose_active's canonical-core check and the save_devices blob
  // order both key off it).
  add(rsmem_, rsdev_, rs232_state_size, rs232_init);  // RS232 card
  add(plmem_, pldev_, plotter_hp7470a_state_size,
      plotter_hp7470a_init);                                  // HP 7470A
  add(lgmem_, lgdev_, light_gun_state_size, light_gun_init);  // light gun
  board_reset(&board_);
  // Measurement hook (config-driven-composition preview): KONCPC_ACTIVE=N
  // limits board_tick to the first N devices. The rest stay constructed + in
  // dev[] (so attach/UI/IPC keep working — unlike a compile-time strip), just
  // undispatched.
  if (const char* e = std::getenv("KONCPC_ACTIVE")) {
    board_.active_count = std::atoi(e);
    active_override_ =
        true;  // pins the set; disables auto-dormancy + fast tier
  }
  // Wake tier (Gate B6) — THE DEFAULT dispatch, all predicates armed (0x3FF).
  // KONCPC_WAKE=<bitmask> overrides the mask for CKSUM bisecting (a clear bit
  // keeps that device awake every cycle): 1=z80 2=crtc 4=psg 8=ppi 16=mem
  // 32=video 64=tape 128=printer 256=gate-array 512=fdc. KONCPC_WAKE=off
  // disables the tier entirely (per-cycle Faithful / soldered dispatch).
  if (const char* e = std::getenv("KONCPC_WAKE")) {
    if (std::strcmp(e, "off") == 0) {
      tier_ = RunTier::Faithful;
    } else {
      wake_mask_ = static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
      tier_ = RunTier::Wake;
    }
  }
  // KONCPC_TIER=fast|wake|soldered|faithful — the four-tier request by name
  // (bench/CI hook; composition-aware degradation still applies).
  if (const char* e = std::getenv("KONCPC_TIER")) {
    if (std::strcmp(e, "fast") == 0)
      tier_ = RunTier::Fast;
    else if (std::strcmp(e, "wake") == 0)
      tier_ = RunTier::Wake;
    else if (std::strcmp(e, "soldered") == 0)
      tier_ = RunTier::Soldered;
    else if (std::strcmp(e, "faithful") == 0)
      tier_ = RunTier::Faithful;
  }
  crtc_attach_asic(&cdev_, &adev_);  // Plus split screen (no-op on models 0-2)
  ga_attach_asic(&gdev_, &adev_);    // Plus PRI deference (no-op on models 0-2)
  mem_attach_asic(&mdev_,
                  &adev_);  // Plus RMR2 low-ROM remap gate (no-op sans cart)

  mem_load_lower_rom(&mdev_, rom, 0x4000);           // OS at 0x0000
  mem_load_upper_rom(&mdev_, rom + 0x4000, 0x4000);  // BASIC at 0xC000
  xmem_.assign(0x10000, 0);  // 64K expansion: the machine is a real 128K 6128
  mem_attach_expansion(&mdev_, xmem_.data(), xmem_.size());
  built_ = true;
  recompose_active();  // establish dormancy + wake-tier validity at
                       // construction (refreshed every frame; this makes
                       // wake_active() meaningful before the first run_frame
                       // too)
  return true;
}

// NOLINTNEXTLINE(readability-non-const-parameter): pointer written through a
// cast or passed to a non-const callee
void Machine::attach_framebuffer(uint8_t* fb, int w, int h) {
  video_attach(&vdev_, &gdev_, fb, w, h);
  // The video Device reads Plus state (12-bit palette + sprites) from the ASIC;
  // it self-gates on asic.plugged, so this is a no-op on models 0-2.
  video_attach_asic(&vdev_, &adev_);
}

void Machine::attach_amsdos(const uint8_t* rom16k, size_t len) {
  if (rom16k != nullptr && len >= 0x4000) mem_attach_rom(&mdev_, 7, rom16k);
}

void Machine::attach_rom(int slot, const uint8_t* rom16k) {
  if (rom16k != nullptr && slot >= 0 && slot < 256)
    mem_attach_rom(&mdev_, slot, rom16k);
}

void Machine::attach_cartridge(const uint8_t* image, size_t bytes) {
  mem_load_cartridge(&mdev_, image, bytes);
}

// NOLINTNEXTLINE(readability-non-const-parameter): pointer written through a
// cast or passed to a non-const callee
bool Machine::insert_disk(uint8_t* dsk, size_t len, uint8_t unit) {
  return fdc_attach_disk(&fdev_, dsk, len, unit) == 0;
}

bool Machine::disk_dirty() const { return fdc_media_dirty(&fdev_) != 0; }
void Machine::mark_disk_clean() { fdc_media_mark_clean(&fdev_); }

bool Machine::insert_flux(const uint8_t* scp, size_t len) {
  // Synthesize a writable DSK overlay from the flux so a flux-backed disc can
  // be written (Stage 2): clean tracks still serve the rotating flux cache,
  // written tracks serve this overlay. A generous cap covers a full 102-track
  // DD disc
  // (~256 + ~8192 bytes/track); flux_scp_to_dsk shrinks it to the real size.
  constexpr size_t kOverlayCap = 0x100 + (102 * (0x100 + 8192));
  flux_dsk_.assign(kOverlayCap, 0);
  const long dsk_len =
      flux_scp_to_dsk(scp, len, flux_dsk_.data(), flux_dsk_.size(), nullptr);
  if (dsk_len > 0) {
    flux_dsk_.resize(static_cast<size_t>(dsk_len));
    if (fdc_attach_flux_writable(&fdev_, scp, len, flux_dsk_.data(),
                                 flux_dsk_.size()) == 0)
      return true;
  }
  // Non-standard / un-synthesizable flux: keep the disc playable read-only
  // rather than failing the insert (weak-bit protection still reads).
  flux_dsk_.clear();
  return fdc_attach_flux(&fdev_, scp, len) == 0;
}

void Machine::eject_disk(uint8_t unit) { fdc_eject_disk(&fdev_, unit); }

bool Machine::insert_tape(const uint8_t* cdt, size_t len) {
  return tape_attach_cdt(&tdev_, cdt, len) == 0;
}
void Machine::eject_tape() { tape_eject(&tdev_); }
void Machine::tape_play_button(bool on) { tape_play(&tdev_, on ? 1 : 0); }
void Machine::tape_rewind_deck() { tape_rewind(&tdev_); }
void Machine::tape_seek(uint32_t block_ordinal) {
  ::tape_seek(&tdev_,
              block_ordinal);  // ::-qualified: the free function, not this
}
int Machine::tape_drain_bits(uint8_t* out, int max) {
  return ::tape_drain_bits(&tdev_, out, max);
}
void Machine::tape_line_in(bool on) { tape_line_mode(&tdev_, on ? 1 : 0); }
void Machine::feed_line_levels(const uint8_t* levels, int count,
                               int sample_rate) {
  if (levels == nullptr || count <= 0) return;
  line_rate_ = sample_rate > 0 ? sample_rate : 44100;
  // Compact the consumed prefix, cap the backlog at ~2 frames of samples.
  line_q_pos_ = std::min<size_t>(line_q_pos_, 0);
  line_q_.erase(line_q_.begin(),
                line_q_.begin() + static_cast<long>(line_q_pos_));

  const size_t cap = static_cast<size_t>(line_rate_) / 25;
  if (line_q_.size() > cap) line_q_.resize(cap);
  line_q_.insert(line_q_.end(), levels, levels + count);
}

void Machine::tape_out_capture(bool on, bool source_rdata) {
  out_capture_ = on;
  out_src_rdata_ = source_rdata;
  out_q_.clear();
  out_acc_ = 0;
}

void Machine::set_tape_line(bool level) {
  tape_line_level(&tdev_, level ? 1 : 0);
}

void Machine::set_crtc_type(uint8_t type) { crtc_set_type(&cdev_, type); }

std::vector<uint8_t> Machine::save_devices() const {
  std::vector<uint8_t> blob;
  for (int i = 0; i < board_.count; ++i) {
    const Device& dev = board_.dev[i];
    size_t n = dev.state_size(dev.self);
    blob.resize(blob.size() + 8 + n);
    uint8_t* at = blob.data() + blob.size() - 8 - n;
    std::memcpy(at, &n, 8);
    dev.save(dev.self, at + 8);
  }
  const size_t tail = blob.size();
  blob.resize(tail + sizeof(Bus) + 8);
  std::memcpy(blob.data() + tail, &board_.bus, sizeof(Bus));
  std::memcpy(blob.data() + tail + sizeof(Bus), &board_.master_cycles, 8);
  return blob;
}

void Machine::load_devices(const std::vector<uint8_t>& blob) {
  size_t at = 0;
  for (int i = 0; i < board_.count; ++i) {
    const Device& dev = board_.dev[i];
    size_t n = 0;
    if (at + 8 > blob.size()) return;
    std::memcpy(&n, blob.data() + at, 8);
    if (at + 8 + n > blob.size()) return;
    dev.load(dev.self, blob.data() + at + 8);
    at += 8 + n;
  }
  if (at + sizeof(Bus) + 8 > blob.size()) return;
  std::memcpy(&board_.bus, blob.data() + at, sizeof(Bus));
  std::memcpy(&board_.master_cycles, blob.data() + at + sizeof(Bus), 8);
}

void Machine::reset() { board_reset(&board_); }

void Machine::set_key_row(uint8_t row, uint8_t columns) {
  psg_set_key_row(&sdev_, row, columns);
  // A host-side matrix change moves no bus line, so the wake predicates can't
  // see it: force-wake so a PSG parked in a keyboard read never serves a stale
  // row. Frame-boundary feeds were already covered by run_frame's force; this
  // makes MID-frame writes (the B2 replay cycle-hook path) exact too.
  wk_force_ = true;
}

uint16_t Machine::take_key_scanned_rows() {
  return psg_take_scanned_rows(&sdev_);
}

void Machine::key(uint8_t code, bool down) {
  // The packed (row << 4 | bit) convention used across the codebase; the
  // machine keeps a shadow matrix so single keys update without read-back.
  const uint8_t row = static_cast<uint8_t>((code >> 4) & 0x0F);
  const uint8_t bit = code & 0x0F;
  uint8_t& cols = shadow_[row];
  if (down)
    cols = static_cast<uint8_t>(cols & ~(1u << bit));
  else
    cols = static_cast<uint8_t>(cols | (1u << bit));
  psg_set_key_row(&sdev_, row, cols);
  wk_force_ = true;  // host-side mutation: see set_key_row
}

int Machine::drain_fdc_events(FdcEvent* out, int max) {
  return fdc_drain_events(&fdev_, out, max);
}

int16_t Machine::emit_sample(long acc, double& dc_x, double& dc_y) const {
  // Average the accumulated 1 MHz inputs, DC-block (the AY level is unipolar),
  // scale, clamp.
  const double raw =
      static_cast<double>((au_n_ ? acc / au_n_ : 0) * kAudioGain);
  dc_y = (raw - dc_x) + (0.9975 * dc_y);
  dc_x = raw;
  return static_cast<int16_t>(
      std::clamp<long>(static_cast<long>(dc_y), -32768, 32767));
}

// Live cassette wires for the host-side scope + drive-sound overlay
// (machine.h).
bool Machine::tape_motor() const { return board_.bus.tape.motor; }
bool Machine::tape_read_level() const { return board_.bus.tape.rdata; }

// Tape OUTPUT capture: sample the cassette wires (data + motor) at the audio
// rate while armed (machine.h tape_out_capture).
void Machine::capture_tape_output() {
  if (!out_capture_) return;
  out_acc_ += kAudioHz;
  if (out_acc_ < 16000000L) return;
  out_acc_ -= 16000000L;
  const bool data =
      out_src_rdata_ ? board_.bus.tape.rdata : board_.bus.tape.wdata;
  out_q_.push_back(
      static_cast<uint8_t>((data ? 1 : 0) | (board_.bus.tape.motor ? 2 : 0)));
}

// Live tape line-in: clock the next queued POST-SCHMITT level onto the deck at
// the feed rate (feed_line_levels), so transitions land with one-sample timing.
void Machine::feed_tape_line_in() {
  if (line_q_pos_ >= line_q_.size()) return;
  line_acc_ += line_rate_;
  if (line_acc_ < 16000000L) return;
  line_acc_ -= 16000000L;
  tape_line_level(&tdev_, line_q_[line_q_pos_++] ? 1 : 0);
}

// Firmware-vector taps (telnet TXT_OUTPUT / BDOS): fire any tap whose address
// the CPU fetches (M1 opcode read) this master cycle. Edge-detected directly on
// the committed bus — a self-contained PC compare, NOT the debug probe. Routing
// taps through the probe kept it live every master cycle even when only the
// always-on console taps were set (~9 ms/f at Faithful — per-cycle
// observability for nothing); firing the same fetch edge here leaves the probe
// dormant on a tap-only machine and keeps Wake's quiet-pair elision available.
// The callback reads architectural registers that hold the caller's values for
// the whole instruction (A for TXT_OUTPUT, C+E for BDOS), so fetch-edge
// granularity is exact — the same point the probe fired, and what the legacy
// engine used.
void Machine::service_taps(const Bus& committed) {
  if (tap_count_ == 0) return;
  const bool fetch = committed.cpu.m1 && committed.cpu.mreq && committed.cpu.rd;
  const bool edge = fetch && !tap_prev_fetch_;
  tap_prev_fetch_ = fetch;
  if (!edge) return;
  for (int t = 0; t < tap_count_; ++t)
    if (taps_[t].addr == committed.cpu.addr) {
      taps_[t].fn(taps_[t].ctx, committed.cpu.addr);
      break;
    }
}

// One PSG output sample: accumulate the channels, mix the analog-domain DACs
// (Digiblaster/AmDrum) into the left, and at the host rate emit one stereo pair
// with the drive-sound overlay panned right.
void Machine::accumulate_audio() { accumulate_audio_bulk(1); }

// k steps' worth of level-stable accumulation — the k=1 case IS the original
// per-step accumulate (integer sums of a constant commute), and every
// host-rate boundary inside the window emits at its exact step.
void Machine::accumulate_audio_bulk(uint32_t k) {
  uint8_t lv[3];
  psg_levels(&sdev_, lv);
  long addL = lv[0] + lv[1];        // channel A + B -> left
  long const addR = lv[2] + lv[1];  // channel C + B -> right
  // Analog-domain DACs (printer-device.md §3, amdrum-device.md §2): a DAC
  // swings like one PSG channel, mixed LEFT per the golden master. Their
  // latches are constant across a bulk window: any write drains audio first
  // (catch-up-then-apply).
  if (digiblaster_) {
    PrinterRegs pr;
    printer_peek(&prtdev_, &pr);
    addL += (static_cast<int>(pr.latch) - 128) * 31 / 128;
  }
  // amdrum_on_ is refreshed each frame (plug state changes only between frames
  // via the host) — skips a 1 MHz-rate peek on the common unplugged machine.
  if (amdrum_on_) {
    AmdrumRegs ar;
    amdrum_peek(&addev_, &ar);
    addL += (static_cast<int>(ar.dac) - 128) * 31 / 128;
  }
  while (k > 0) {
    // Steps until au_phase_ crosses the host-rate boundary (≥ 1 by the
    // au_phase_ < kPsgHz loop invariant).
    const long to_boundary = ((kPsgHz - au_phase_) + kAudioHz - 1) / kAudioHz;
    const uint32_t c = k < static_cast<uint32_t>(to_boundary)
                           ? k
                           : static_cast<uint32_t>(to_boundary);
    au_accL_ += addL * c;
    au_accR_ += addR * c;
    au_n_ += static_cast<int>(c);
    au_phase_ += kAudioHz * static_cast<long>(c);
    k -= c;
    if (au_phase_ < kPsgHz) continue;  // not yet a host-rate sample boundary
    au_phase_ -= kPsgHz;
    int32_t const left = emit_sample(au_accL_, dcL_x_, dcL_y_);
    int32_t const right = emit_sample(au_accR_, dcR_x_, dcR_y_);
    if (overlay_ != nullptr) {
      // Forward the FDC's mechanical events to the host drive-sound layer. The
      // cosmetic audio itself is NOT summed here — it renders on its own SDL
      // stream (drive_sounds_fill_stereo) and mixes at the device, so the
      // emulated AY sample stays pure (and deterministic).
      FdcEvent ev[16];
      int n;
      while ((n = fdc_drain_events(&fdev_, ev, 16)) > 0)
        overlay_->events(ev, n);
    }
    audio_.push_back(static_cast<int16_t>(std::clamp(left, -32768, 32767)));
    audio_.push_back(static_cast<int16_t>(std::clamp(right, -32768, 32767)));
    au_accL_ = au_accR_ = 0;
    au_n_ = 0;
  }
}

// The Fast tier's tick (Gate B5): mirror board_tick() exactly (same reset, same
// board_add order, same commit) — only the dispatch differs. Instead of the
// runtime fn-pointer array loop, we unroll a fixed sequence of the devices' own
// tick pointers. Under LTO the compiler sees every device's definition and can
// devirtualize gdev_.tick -> ga_tick and inline it, which the generic array
// loop (contents set at runtime via board_add) can't be proven safe to do. It
// runs the same devices in the same order over the same two-phase bus, so it
// stays observation-identical to board_tick (the harness proves it), and is
// valid only for the canonical composition that soldered_available() gates.
void Machine::tick_soldered() {
  Bus next = bus_resting();
  const Bus* in = &board_.bus;
  gdev_.tick(gdev_.self, in, &next);
  cdev_.tick(cdev_.self, in, &next);
  pdev_.tick(pdev_.self, in, &next);
  sdev_.tick(sdev_.self, in, &next);
  mdev_.tick(mdev_.self, in, &next);
  vdev_.tick(vdev_.self, in, &next);
  zdev_.tick(zdev_.self, in, &next);
  fdev_.tick(fdev_.self, in, &next);
  prdev_.tick(prdev_.self, in, &next);
  tdev_.tick(tdev_.self, in, &next);
  prtdev_.tick(prtdev_.self, in, &next);
  addev_.tick(addev_.self, in, &next);
  mfdev_.tick(mfdev_.self, in, &next);
  axdev_.tick(axdev_.self, in, &next);
  swdev_.tick(swdev_.self, in, &next);
  sfdev_.tick(sfdev_.self, in, &next);
  m4dev_.tick(m4dev_.self, in, &next);
  adev_.tick(adev_.self, in, &next);
  rsdev_.tick(rsdev_.self, in, &next);
  pldev_.tick(pldev_.self, in, &next);
  lgdev_.tick(lgdev_.self, in, &next);
  board_.bus = next;
  board_.master_cycles += 1;
}

// The Wake tier's tick (Gate B6, beads-708f): the same devices over the same
// two-phase bus commit — but each chip is dispatched only on the master cycles
// its CLOCK-WAKE CONTRACT allows, and a sleeping chip's owned bus lines are
// HELD from the committed bus: the board-level generalization of the Z80's own
// internal drive-and-hold. Byte-identity argument per device:
//
//   GA      awake every cycle — it IS the ÷16 clock divider (and the raster
//           counter + irq driver: rest-init deassert needs its re-drive).
//   Z80     advances only on clk.cpu (4 of 16); between advances it holds its
//           lines — the hold below IS that re-drive. Its only per-cycle work is
//           the NMI edge latch, so an nmi/reset change also wakes it; the lines
//           can then only change while it is awake.
//   CRTC    counts on clk.crtc (1 of 16) and decodes I/O while iorq — its
//           outputs (vid.*) are re-driven from state each tick, so holding them
//           while asleep is the identical value. R7-write VSYNC tricks happen
//           on an iorq wake, so vid.* can still only change on a CRTC wake.
//   PSG     sound_step on clk.psg; the AY bus responds to PPI-held lines, which
//           only change when the PPI runs — so the PSG wakes whenever the PPI
//           does (also keeps the shared ay.da single-held: PSG awake ⊇ PPI
//           awake) or did last cycle (its outputs land one commit later).
//   PPI     drives ay/tape lines purely from its registers: constant between
//           I/O writes and busak edges. Port-B reads pass LIVE vsync/rdata
//           levels, so a read-in-progress also re-wakes it when those movers
//           (CRTC, tape) ran last cycle.
//   MEM     responds only while a strobe is on the bus or on the two GA video
//           fetch slots; its response is idempotent while the lines are held,
//           so one wake per line-CHANGE plus the fetch slots is exact.
//   VIDEO   drives nothing; consumes ram.data at visible phases 13/15 and sync
//           edges — which only move when the CRTC ran last cycle.
//   FDC     awake every cycle (stage 1): free-running mechanics counters stamp
//           event timestamps; exact batch-advance is the next increment.
//   TAPE    idle deck: latches motor (constant between PPI wakes) and re-drives
//           a constant rdata — held. Playing or line-in: awake every cycle.
//   PRINTER a write-only latch: between iorq-write strobes its tick is exactly
//           `now++`, re-applied in one printer_advance() at the next wake so
//           PrinterEvent timestamps stay identical to a per-cycle run.
//   cpu.data a live strobe holds the byte (the faithful run's responder/CPU
//           re-drives it every cycle); no strobe → rest 0xFF, matching the
//           faithful float. ram.data needs no hold: driver (mem, fetch slots
//           12/14) and reader (video, 13/15) interlock within the µs.
//
// KONCPC_WAKE bit clear ⇒ that device stays awake every cycle (forced-true
// predicate == tick_soldered semantics for it) — the per-device bisect hook.
//
// wake_slot<P> is specialized on the visible clk.phase so the µs
// microstructure's fixed positions (Z80 clock slots, fetch slots, video
// consume slots, the synthesized GA phase) constant-fold; tick_wake() is the
// phase-generic 16-way switch over it, and run_wake_us() chains the slots
// directly. Change detection runs only on cycles where change is POSSIBLE
// (a line moves only when its driver ran) — and over-waking is always safe:
// every contract device's tick is idempotent under unchanged inputs (the tape,
// whose counters are not, is awake for every cycle it is live), so scheduling
// a SUPERSET of the contract cycles stays byte-identical.
template <int P>
void Machine::wake_slot(const Bus& cur, Bus& next) {
  static_assert(P >= 0 && P < 16, "P is the visible clk.phase");
  next = bus_resting();

  // Fixed positions in the µs microstructure (constant-folded).
  constexpr bool kZ80Clk = (P & 0x03) == 0;    // clk.cpu: 4 MHz slots
  constexpr bool kClk0 = P == 0;               // clk.crtc / clk.psg
  constexpr bool kFetch = P == 12 || P == 14;  // GA drove ram.fetch
  constexpr bool kVid = P == 13 || P == 15;    // video consumes ram.data

  const bool strobe = cur.cpu.mreq || cur.cpu.iorq;

  // --- change detection, only where change is possible: CPU lines move on the
  //     cycle after the Z80 ran (or any cycle on a Plus — the ASIC is a second
  //     bus master); tape.motor after a PPI run; AY lines after a PPI/PSG/ASIC
  //     run. Elsewhere the shadow still equals the line and the compare (and
  //     its shadow update) is skipped whole. ---
  bool tuple_changed = false;
  bool z80_async = false;   // an NMI/reset edge to latch
  bool busak_edge = false;  // AY-bus master handover
  if (wk_z80_ran_ || wk_asic_on_) {
    const uint8_t flags = static_cast<uint8_t>(
        (cur.cpu.m1 ? 0x01 : 0) | (cur.cpu.mreq ? 0x02 : 0) |
        (cur.cpu.iorq ? 0x04 : 0) | (cur.cpu.rd ? 0x08 : 0) |
        (cur.cpu.wr ? 0x10 : 0) | (cur.cpu.busak ? 0x20 : 0) |
        (cur.cpu.nmi ? 0x40 : 0) | (cur.cpu.reset ? 0x80 : 0));
    const uint8_t delta = static_cast<uint8_t>(flags ^ wk_flags_);
    tuple_changed = cur.cpu.addr != wk_addr_ || (delta & 0x3F) != 0;
    z80_async = (delta & 0xC0) != 0;
    busak_edge = (delta & 0x20) != 0;
    wk_addr_ = cur.cpu.addr;
    wk_flags_ = flags;
  }
  bool motor_edge = false;
  if (wk_ppi_ran_) {
    motor_edge = cur.tape.motor != wk_motor_;
    wk_motor_ = cur.tape.motor;
  }

  // PSG inputs: wake on ANY movement of its AY-bus lines, compared directly —
  // the bus has TWO masters (the PPI, and on a Plus the ASIC's DMA sound
  // sequencer in its BUSAK slot), so a PPI-chain heuristic would sleep through
  // DMA PSG writes.
  bool ay_edge = false;
  if (wk_ppi_ran_ || wk_psg_ran_ || wk_asic_on_) {
    const uint16_t ay_in =
        static_cast<uint16_t>((cur.ay.bdir ? 0x1000 : 0) |
                              (cur.ay.bc1 ? 0x0100 : 0) | cur.ay.kbd_row);
    ay_edge = ay_in != wk_ay_ || cur.ay.da != wk_ay_da_;
    wk_ay_ = ay_in;
    wk_ay_da_ = cur.ay.da;
  }

  // --- wake predicates ---
  const bool io = tuple_changed && cur.cpu.iorq;
  bool w_z80 = kZ80Clk || z80_async;
  bool w_crtc = kClk0 || io;
  // Self-rewake (wk_*_woke_): ppi_tick drives its outputs BEFORE decoding the
  // I/O write that changes them — a one-cycle drive-then-latch pipeline. The
  // faithful run publishes the new lines on the very next tick, so a CAUSED
  // wake (one whose inputs may have changed device state) is followed by one
  // more to publish. Only the CAUSE arms the flag — arming it from the wake
  // itself would feed the predicate back into itself and hold the device
  // awake forever (found by profile: PPI+PSG were ~11% of the frame).
  const bool ppi_cause =
      io || busak_edge ||
      (cur.cpu.iorq && cur.cpu.rd && (wk_crtc_woke_ || wk_tape_woke_));
  bool w_ppi = ppi_cause || wk_ppi_woke_;
  // `|| w_ppi` keeps the shared ay.da single-held (the da hold below assumes
  // PSG awake ⊇ PPI awake); a PPI publish that moved the lines re-arms the
  // PSG through ay_edge the cycle after. The held-WRITE term is the ONE
  // idempotency exception in the contracts: ay_bus re-applies the register
  // write every cycle the (bdir, /bc1) state is held, and a reg-13 write
  // RESTARTS THE ENVELOPE each time — the faithful run pins the envelope for
  // the whole strobe, so the PSG must stay awake through it.
  const bool psg_cause =
      kClk0 || w_ppi || ay_edge || (cur.ay.bdir && !cur.ay.bc1);
  bool w_psg = psg_cause || wk_psg_woke_;
  // MEM has its own drive-then-latch pipeline: a write arms a one-tick latch
  // committed on the NEXT tick under that cycle's /RAMDIS (docs §4b — the
  // expansion veto settles one tick behind the strobes). The rewake keeps the
  // memory awake for every cycle of a write strobe, so the armed commit lands
  // under the same per-cycle /RAMDIS the faithful run sees (the ASIC's
  // register-page overlay varies it on a Plus).
  bool w_mem = kFetch || (tuple_changed && strobe) || wk_mem_woke_;
  bool w_vid = kVid || wk_crtc_woke_;
  bool w_tape = wk_tape_live_ || motor_edge;
  bool w_prt = cur.cpu.iorq && cur.cpu.wr;
  // GA: on a quiet cycle (no sync movement, no CPU I/O, no ack) its tick is
  // exactly `phase++` plus bus outputs that are pure functions of that phase —
  // synthesized below. Stateful work needs a wake: sync edges (CRTC ran), the
  // I/O write decode and the INT-ack (both under iorq). Plus-only caveat: irq
  // is wired-OR with the ASIC, and the hold below assumes the GA is its SOLE
  // driver — so the GA never sleeps while the ASIC is plugged.
  bool w_ga = wk_asic_on_ || wk_crtc_woke_ || io;
  bool forced = false;
  if (wk_force_) {  // frame start / post-mutation: shadows may be stale
    w_z80 = w_crtc = w_psg = w_ppi = w_mem = w_vid = w_tape = w_prt = true;
    w_ga = true;
    forced = true;  // a forced wake may change state → publish next cycle too
    wk_force_ = false;
  }
  if (!(wake_mask_ & 0x01)) w_z80 = true;
  if (!(wake_mask_ & 0x02)) w_crtc = true;
  if (!(wake_mask_ & 0x04)) w_psg = true;
  if (!(wake_mask_ & 0x08)) w_ppi = true;
  if (!(wake_mask_ & 0x10)) w_mem = true;
  if (!(wake_mask_ & 0x20)) w_vid = true;
  if (!(wake_mask_ & 0x40)) w_tape = true;
  if (!(wake_mask_ & 0x80)) w_prt = true;
  if (!(wake_mask_ & 0x100)) w_ga = true;
  // FDC: awake while its select could be on the bus (any iorq level — the
  // decode is its own) or while it is not quiet (motor / seek / timed phase);
  // fdc_quiet() is re-read after every real tick, so a motor-on write flips it
  // that same cycle. Quiet skips are exactly `now++`, re-applied at wake.
  bool w_fdc = cur.cpu.iorq || !wk_fdc_quiet_ || forced;
  if (!(wake_mask_ & 0x200)) w_fdc = true;
  wk_z80_ran_ = w_z80;
  wk_crtc_woke_ = w_crtc;
  wk_ppi_woke_ = ppi_cause || forced;  // publish follows the CAUSE, not itself
  wk_psg_woke_ = psg_cause || forced;
  wk_ppi_ran_ = w_ppi;  // the compare gates key off the RUN (outputs may move)
  wk_psg_ran_ = w_psg;
  wk_mem_woke_ = w_mem && cur.cpu.mreq && cur.cpu.wr;  // write-latch pipeline
  wk_tape_woke_ = w_tape;

  // --- holds: a sleeping chip's owned lines persist (== its own re-drive).
  //     Applied before dispatch, so an awake driver simply overwrites. ---
  if (!w_z80) {
    next.cpu.addr = cur.cpu.addr;
    next.cpu.m1 = cur.cpu.m1;
    next.cpu.mreq = cur.cpu.mreq;
    next.cpu.iorq = cur.cpu.iorq;
    next.cpu.rd = cur.cpu.rd;
    next.cpu.wr = cur.cpu.wr;
    next.cpu.rfsh = cur.cpu.rfsh;
    next.cpu.halt = cur.cpu.halt;
    next.cpu.busak = cur.cpu.busak;
  }
  if (strobe) next.cpu.data = cur.cpu.data;
  if (!w_crtc) {
    next.vid = cur.vid;  // ma/ra/syncs/dispen/cursor/frame_line — all CRTC's
  }
  if (!w_ppi) {
    next.ay.bdir = cur.ay.bdir;
    next.ay.bc1 = cur.ay.bc1;
    next.ay.kbd_row = cur.ay.kbd_row;
    next.tape.motor = cur.tape.motor;
    next.tape.wdata = cur.tape.wdata;
  }
  // ay.da holds on the PPI alone: an awake PSG only drives da in the READ
  // state (its drive overrides this hold), so keying the hold on w_psg let da
  // collapse to rest at every clk.psg wake while the PPI slept in port-A
  // OUTPUT mode — faithful re-drives port A there every cycle. Carrying the
  // last commit is exact for the rest case too (if nobody drove last cycle,
  // cur.ay.da is already the resting 0xFF).
  if (!w_ppi) next.ay.da = cur.ay.da;
  if (!w_tape) next.tape.rdata = cur.tape.rdata;

  // --- dispatch (build() order, same as tick_soldered) ---
  const Bus* in = &cur;
  if (w_ga) {
    if (wk_ga_skip_ != 0) {
      ga_advance(&gdev_, wk_ga_skip_);  // land the ÷16 divider exactly
      wk_ga_skip_ = 0;
    }
    gdev_.tick(gdev_.self, in, &next);
  } else {
    // The GA's pure-function-of-phase outputs — ga_clock_out is the SAME
    // definition ga_tick executes, so the sleeping and live paths cannot
    // drift. The GA's internal phase is stale while it sleeps; ga_advance()
    // re-lands it at the next wake. P is the phase the GA last drove, so the
    // cycle being produced is P+1 — a compile-time constant here, folding the
    // fetch-slot branch away entirely.
    ga_clock_out(static_cast<uint8_t>((P + 1) & 0x0F), cur.vid.ma, cur.vid.ra,
                 &next);
    // 6128: the GA is the only INT driver (w_ga forces awake when the ASIC is
    // plugged), so holding its wired-OR assertion is exact.
    if (cur.cpu.irq) next.cpu.irq = true;
    wk_ga_skip_++;
  }
  // Light gun: tick on char boundaries (clk.crtc) with the committed bus so it
  // tracks the beam one char behind the CRTC exactly as in the per-cycle tiers,
  // then latch the char the CRTC has just advanced to (the per-cycle path
  // publishes the strobe one master cycle after the gun ticks, and the CRTC
  // latches that same char — the one after the gun's view).  crtc_tick's own
  // pen.strobe handler stays inert here: at a char boundary the committed
  // pen.strobe is LOW (the gun drives it only on clk.crtc and the line rests
  // between char ticks), so the level-sensitive batch latch owns the decision.
  // The whole block is skipped on the common unplugged machine, leaving the
  // plain CRTC tick.
  if (w_crtc) {
    if (wk_light_gun_on_) {
      bool gun_strobe = false;
      if (cur.clk.crtc) {
        Bus lg_next = bus_resting();
        lgdev_.tick(lgdev_.self, in, &lg_next);
        gun_strobe = lg_next.pen.strobe;
        next.pen.strobe = gun_strobe;
      }
      cdev_.tick(cdev_.self, in, &next);
      if (cur.clk.crtc) crtc_batch_lpen_latch(&cdev_, gun_strobe);
    } else {
      cdev_.tick(cdev_.self, in, &next);
    }
  }
  if (w_ppi) pdev_.tick(pdev_.self, in, &next);
  if (w_psg) sdev_.tick(sdev_.self, in, &next);
  if (w_mem) mdev_.tick(mdev_.self, in, &next);
  if (w_vid) vdev_.tick(vdev_.self, in, &next);
  if (w_z80) zdev_.tick(zdev_.self, in, &next);
  if (w_fdc) {
    if (wk_fdc_skip_ != 0) {
      fdc_advance(&fdev_, wk_fdc_skip_);  // exact `now` catch-up (fdc.h)
      wk_fdc_skip_ = 0;
    }
    fdev_.tick(fdev_.self, in, &next);
    wk_fdc_quiet_ = fdc_quiet(&fdev_) != 0;
  } else {
    wk_fdc_skip_++;
  }
  if (wk_probe_on_) prdev_.tick(prdev_.self, in, &next);
  if (w_tape) tdev_.tick(tdev_.self, in, &next);
  if (w_prt) {
    if (wk_prt_skip_ != 0) {
      printer_advance(&prtdev_, wk_prt_skip_);  // exact timestamp catch-up
      wk_prt_skip_ = 0;
    }
    prtdev_.tick(prtdev_.self, in, &next);
  } else {
    wk_prt_skip_++;
  }
  if (wk_asic_on_) adev_.tick(adev_.self, in, &next);
  // RS232 + HP7470 plotter — the bit-serial pair, ticked as ONE unit (they
  // cross-couple through serial.txd/rxd, so whenever one transmits it is
  // non-quiet and the other must be awake to sample the start edge). Awake
  // while the CPU could select them ($FAxx/$FBxx iorq), while either UART is
  // mid-frame or holds buffered work (wk_serial_quiet_ false), or on a forced
  // wake. Quiet-skip is byte-identical: serial.txd/rxd stay at bus_resting()'s
  // mark (the held idle level) and neither device runs a per-cycle counter
  // while quiet. Dispatch order (rs232 then plotter) matches tick_soldered.
  if (wk_serial_on_) {
    const uint8_t ser_hi = static_cast<uint8_t>(cur.cpu.addr >> 8);
    const bool serial_io = cur.cpu.iorq && (ser_hi == 0xFA || ser_hi == 0xFB);
    // Self-rewake one cycle past each serial access (wk_serial_io_prev_): the
    // DART/PIT edge-detect their I/O select, so they need a tick with the
    // strobe GONE to reset that edge — the same drive-then-latch self-rewake
    // the PPI/MEM use. Without it only the FIRST OUT of a burst registers
    // (trace: the DART never transmitted, plotter received 0 bytes).
    if (serial_io || wk_serial_io_prev_ || !wk_serial_quiet_ || forced) {
      rsdev_.tick(rsdev_.self, in, &next);
      pldev_.tick(pldev_.self, in, &next);
      wk_serial_quiet_ =
          rs232_quiet(&rsdev_) != 0 && plotter_hp7470a_quiet(&pldev_) != 0;
    }
    wk_serial_io_prev_ = serial_io;
  }
  // No commit here: the caller owns it — tick_wake copies into board_.bus per
  // cycle; run_wake_us ping-pongs two local buffers and writes back once.
}

// Phase-generic entry: dispatch to the slot matching the committed clk.phase.
// The GA (or its synthesized stand-in) drives phase = last+1 every cycle, so
// the committed value always names the slot about to execute.
void Machine::tick_wake() {
  Bus next;  // rest-initialized by wake_slot before any drive
  switch (board_.bus.clk.phase & 0x0F) {
    case 0:
      wake_slot<0>(board_.bus, next);
      break;
    case 1:
      wake_slot<1>(board_.bus, next);
      break;
    case 2:
      wake_slot<2>(board_.bus, next);
      break;
    case 3:
      wake_slot<3>(board_.bus, next);
      break;
    case 4:
      wake_slot<4>(board_.bus, next);
      break;
    case 5:
      wake_slot<5>(board_.bus, next);
      break;
    case 6:
      wake_slot<6>(board_.bus, next);
      break;
    case 7:
      wake_slot<7>(board_.bus, next);
      break;
    case 8:
      wake_slot<8>(board_.bus, next);
      break;
    case 9:
      wake_slot<9>(board_.bus, next);
      break;
    case 10:
      wake_slot<10>(board_.bus, next);
      break;
    case 11:
      wake_slot<11>(board_.bus, next);
      break;
    case 12:
      wake_slot<12>(board_.bus, next);
      break;
    case 13:
      wake_slot<13>(board_.bus, next);
      break;
    case 14:
      wake_slot<14>(board_.bus, next);
      break;
    default:
      wake_slot<15>(board_.bus, next);
      break;
  }
  board_.bus = next;
  board_.master_cycles += 1;
}

// One µs of the chunked fast path: the 16 slots in sequence with no per-cycle
// dispatch switch and no per-cycle frame bookkeeping beyond what is exact —
// audio accumulates only after slot 15 (the only commit with clk.psg high, by
// the phase invariant above), taps drain per slot only while installed, and
// the frame-exit peek mirrors the per-cycle path's VSYNC-neighbourhood gate
// slot for slot. The caller guarantees alignment (committed clk.phase == 15),
// distance from VSYNC, and quiet-frame mode (no capture, no line-in feed, no
// cycle hook, no armed comparators).
int Machine::run_wake_us(VideoRegs& vr, uint32_t target, bool& vsync_seen) {
  // Ping-pong bus buffers: for the whole µs the committed bus lives in a/b
  // alternately (stack-hot, unaliased with board_), and board_.bus is written
  // back once on exit — dropping 15 of 16 per-cycle commit copies, the largest
  // single term of the measured structural floor. Nothing inside the chunk
  // reads board_.bus (capture/line-in/hook are gated off; taps, audio and the
  // frame peek are device-side), so the deferred write-back is unobservable.
  Bus bufa = board_.bus;
  Bus bufb;
  Bus* cur = &bufa;
  Bus* nxt = &bufb;
  int n = 0;
#define KONCPC_WAKE_SLOT(P)                \
  wake_slot<P>(*cur, *nxt);                \
  board_.master_cycles += 1;               \
  n++;                                     \
  if (tap_count_ != 0) service_taps(*nxt); \
  if ((P) == 15) accumulate_audio();       \
  {                                        \
    const bool was = vsync_seen;           \
    vsync_seen = nxt->vid.vsync;           \
    if (vsync_seen || was) {               \
      video_peek(&vdev_, &vr);             \
      if (vr.frames >= target) {           \
        board_.bus = *nxt;                 \
        return n;                          \
      }                                    \
    }                                      \
  }                                        \
  {                                        \
    Bus* flip = cur;                       \
    cur = nxt;                             \
    nxt = flip;                            \
  }
  // Quiet-pair elision — the Z80-slot-spacing payoff WITHOUT touching the Z80
  // core: at slots {2,3}, {6,7}, {10,11} every device is provably asleep on a
  // quiet µs (the Z80 last ran two slots ago so no line can move; no CPU I/O;
  // no AY WRITE state held; no publish/write-latch chains pending; deck idle;
  // FDC quiet; probe/ASIC off; away from VSYNC; every predicate armed). The two
  // commits those slots would produce are the identity holds plus the GA's pure
  // clock fields — and of those, only phase/cpu-enable//WAIT differ from what
  // the buffer already holds (crtc/psg enables and the fetch strobe are false
  // on both sides). So: patch three clock fields IN PLACE, book the skipped
  // cycles for the catch-up counters, and hand the SAME buffer to the next Z80
  // slot. Pure scheduler algebra over the holds — the chips are untouched —
  // enforced byte-identical by the harness and CKSUM oracles. NEXTP is the next
  // Z80 slot (4/8/12, always a clk.cpu slot). Frame-constant eligibility,
  // hoisted: on a composition where elision can never fire (Plus: the ASIC is a
  // live every-cycle actor) the per-pair check collapses to one predictable
  // branch.
  const bool elide_base =
      wake_mask_ == 0x3FF && !wk_tape_live_ && !wk_probe_on_ && !wk_asic_on_;
#define KONCPC_ELIDABLE()                                                     \
  (elide_base && !cur->cpu.iorq && !(cur->ay.bdir && !cur->ay.bc1) &&         \
   !wk_z80_ran_ && !wk_crtc_woke_ && !wk_ppi_woke_ && !wk_psg_woke_ &&        \
   !wk_ppi_ran_ && !wk_psg_ran_ && !wk_mem_woke_ && !wk_tape_woke_ &&         \
   wk_fdc_quiet_ && wk_serial_quiet_ && !wk_serial_io_prev_ && !vsync_seen && \
   !cur->vid.vsync)
#define KONCPC_ELIDE_PAIR(NEXTP)                                               \
  /* The GA's pure clock fields for the commit the next slot reads — the ONE \
   * shared definition (ga_clock_out), which also drives the video-fetch       \
   * strobe when NEXTP is a fetch slot (12): the {10,11} pair's second commit  \
   * must carry ram.fetch/addr or the visible-12 fetch never happens. The      \
   * crtc/psg enables it writes are false on both sides; every other field     \
   * of the buffer is already the identity the two skipped holds would keep.   \
   */                                                                          \
  ga_clock_out((NEXTP), cur->vid.ma, cur->vid.ra, cur);                        \
  board_.master_cycles += 2;                                                   \
  wk_ga_skip_ += 2;                                                            \
  wk_prt_skip_ += 2;                                                           \
  wk_fdc_skip_ += 2;                                                           \
  n += 2;
  KONCPC_WAKE_SLOT(0)
  KONCPC_WAKE_SLOT(1)
  // NOLINTNEXTLINE(readability-simplify-boolean-expr): boolean expression is
  // produced by the wake-scheduler KONCPC_* macro; not rewritable at this site
  if (KONCPC_ELIDABLE()) {
    KONCPC_ELIDE_PAIR(4)
  } else {
    KONCPC_WAKE_SLOT(2)
    KONCPC_WAKE_SLOT(3)
  }
  KONCPC_WAKE_SLOT(4)
  KONCPC_WAKE_SLOT(5)
  // NOLINTNEXTLINE(readability-simplify-boolean-expr): boolean expression is
  // produced by the wake-scheduler KONCPC_* macro; not rewritable at this site
  if (KONCPC_ELIDABLE()) {
    KONCPC_ELIDE_PAIR(8)
  } else {
    KONCPC_WAKE_SLOT(6)
    KONCPC_WAKE_SLOT(7)
  }
  KONCPC_WAKE_SLOT(8)
  KONCPC_WAKE_SLOT(9)
  // NOLINTNEXTLINE(readability-simplify-boolean-expr): boolean expression is
  // produced by the wake-scheduler KONCPC_* macro; not rewritable at this site
  if (KONCPC_ELIDABLE()) {
    KONCPC_ELIDE_PAIR(12)
  } else {
    KONCPC_WAKE_SLOT(10)
    KONCPC_WAKE_SLOT(11)
  }
  KONCPC_WAKE_SLOT(12)
  KONCPC_WAKE_SLOT(13)
  KONCPC_WAKE_SLOT(14)
  KONCPC_WAKE_SLOT(15)
#undef KONCPC_ELIDE_PAIR
#undef KONCPC_ELIDABLE
#undef KONCPC_WAKE_SLOT
  board_.bus = *cur;  // after the final flip, cur holds slot 15's commit
  return n;
}

// ===========================================================================
// The Fast tier (F6): instruction-granularity catch-up scheduling — the F4/F5
// driver-let promoted into the machine. The Z80 batch engine drives the frame;
// the CRTC+GA advance EAGERLY (the irq counter must lead the CPU), the
// renderer and audio catch up LAZILY (writes pull them forward), the FDC and
// printer ride master-cycle cursors (advance while quiet, burst-tick while
// hot). Time mapping (derived and oracle-proven in fast_video_chain_test /
// fast_video_render_test): with entry at a committed-phase-0 boundary and
// rel = tstates - fs_t0_, CRTC char k runs at rel master 16k, its irq effect
// is CPU-visible at rel T-state 4k+1, an access with T1 at rel tau applies
// after char floor(tau/4), and everything with T1 in µs j lands before cell
// j's render. Audio units are (sound_step; accumulate) pairs; an AY op with
// T1 in µs j applies after unit j.
// ===========================================================================

void Machine::fs_advance_chars(uint64_t target) {
  if (target <= fs_chars_) return;
  // Frame-length guard: a real frame is VSYNC-cut well before kMaxFastChars.
  // Overshooting it means no frame boundary is coming this batch (the
  // post-reset no-VSYNC window, where a HALT with interrupts off free-runs the
  // char clock). Clamp and raise fs_bail_ so the batch loop stops and the
  // caller finishes the frame per-cycle — without this the walk below (and
  // fs_audio_steps at the exit) run billions of iterations and exhaust memory.
  if (target > kMaxFastChars) {
    target = kMaxFastChars;
    fs_bail_ = true;
    if (target <= fs_chars_) return;
  }
  uint32_t const n = static_cast<uint32_t>(target - fs_chars_);
  if (fs_pend_tail_ + n > fs_pend_cap_) {
    size_t cap = fs_pend_cap_ != 0 ? fs_pend_cap_ * 2 : 4096;
    while (cap < fs_pend_tail_ + n) cap *= 2;
    std::unique_ptr<CrtcCharView[]> grown(new CrtcCharView[cap]);
    std::memcpy(grown.get(), fs_pend_buf_.get(),
                fs_pend_tail_ * sizeof(CrtcCharView));
    fs_pend_buf_ = std::move(grown);
    fs_pend_cap_ = cap;
  }
  CrtcCharView* views = fs_pend_buf_.get() + fs_pend_tail_;
  uint32_t last_line = 0xFFFFFFFF;  // 16-bit frame_line: the first view always
                                    // relays (asic_batch_frame_line dedupes)
  // Per-view work shared between the bulk (closed-form) and light-gun
  // (char-by-char) paths.  Kept as a local lambda so both arms stay in sync.
  auto apply_view = [this, &last_line](CrtcCharView& view) {
    if (view.edges & (1u << CRTC_EDGE_VSYNC_RISE)) {
      ga_batch_vsync_rise(&gdev_);
      if (++fs_frames_seen_ >= fs_target_) fs_cut_ = true;
    }
    if (view.edges & (1u << CRTC_EDGE_HSYNC_RISE)) ga_batch_hsync_rise(&gdev_);
    if (view.edges & (1u << CRTC_EDGE_HSYNC_FALL)) ga_batch_hsync_fall(&gdev_);
    if (fs_asic_on_ && view.frame_line != last_line) {
      // The PRI counts the same frame-line reference. It moves once per
      // scanline, and the ASIC edge-detects on pri_prev_line anyway (a repeat
      // call is a contract no-op) — relaying changes only drops ~64 Device
      // hops per line from the Plus hot path.
      asic_batch_frame_line(&adev_, view.frame_line);
      last_line = view.frame_line;
    }
    view.mode = ga_current_mode(&gdev_);  // the latch as of THIS char —
                                          // stamped in edge order
    fs_vpages_ |= static_cast<uint8_t>(1u << ((view.ma >> 12) & 3));
  };

  if (fs_lpen_on_) {
    // Light-gun Fast contract: advance one char at a time so c->ma sits at the
    // latch char.  The per-cycle tiers publish the gun's strobe one master
    // cycle after the gun ticks, and the CRTC then latches the char it has just
    // advanced to — i.e. the char AFTER the one the gun saw.  Mirror that here:
    // tick the gun with this char's view, defer its decision by one char, and
    // latch the next char.  The latch is LEVEL-sensitive
    // (crtc_batch_lpen_latch) because the gun pulses per char, so a held
    // trigger latches every in-window char, not just the first of a run.
    for (uint32_t i = 0; i < n; ++i) {
      CrtcCharView& view = views[i];
      crtc_advance_view(&cdev_, 1, &view);
      apply_view(view);
      crtc_batch_lpen_latch(&cdev_, fs_lpen_pending_);
      Bus lg_in = bus_resting();
      lg_in.clk.crtc = true;
      lg_in.vid.frame_line = view.frame_line;
      lg_in.vid.dispen = (view.levels & CRTC_LVL_DISPEN) != 0;
      Bus lg_out = bus_resting();
      lgdev_.tick(lgdev_.self, &lg_in, &lg_out);
      fs_lpen_pending_ = lg_out.pen.strobe;
    }
  } else {
    crtc_advance_view(&cdev_, n, views);
    for (uint32_t i = 0; i < n; ++i) {
      apply_view(views[i]);
    }
  }
  fs_pend_tail_ += n;
  fs_chars_ = target;
}

// The combined INT line the CPU samples (wired-OR: GA + the Plus ASIC).
bool Machine::fs_irq() const {
  if (ga_irq_asserted(&gdev_) != 0) return true;
  return fs_asic_on_ && asic_irq_asserted(&adev_) != 0;
}

void Machine::fs_render_below(uint64_t target) {
  target = std::min(target, fs_chars_);
  if (target <= fs_cells_) return;
  size_t const n = static_cast<size_t>(target - fs_cells_);
  video_batch_cells(&vdev_, fs_vram_, fs_pend_buf_.get() + fs_pend_head_,
                    static_cast<int>(n));
  fs_pend_head_ += n;
  fs_cells_ = target;
}

void Machine::fs_audio_steps(uint64_t steps) {
  while (fs_audio_steps_ < steps) {
    if (fs_audio_accs_ < fs_audio_steps_) {
      accumulate_audio();  // the previous unit's deferred accumulate — its
      fs_audio_accs_++;    // levels are stable until this next step
    }
    psg_batch_step(&sdev_);
    fs_audio_steps_++;
    // F8 bulk: the step above ran the mixer against live registers, and
    // registers cannot move inside this drain (writes drain audio first,
    // then apply) — so until the next counter event the output levels are
    // provably static. Fold whole (accumulate; step) pairs: m accumulates
    // of a constant level, then m closed-form counter skips. m stops one
    // short of the event step, which the next iteration runs for real.
    if (fs_audio_steps_ < steps && fs_audio_accs_ < fs_audio_steps_) {
      const uint32_t quiet = psg_batch_quiet_steps(&sdev_);
      const uint64_t want = steps - fs_audio_steps_;
      const uint32_t m =
          static_cast<uint32_t>(want < quiet - 1 ? want : quiet - 1);
      if (m > 0) {
        accumulate_audio_bulk(m);
        fs_audio_accs_ += m;
        psg_batch_skip(&sdev_, m);
        fs_audio_steps_ += m;
      }
    }
  }
}

void Machine::fs_fdc_to(uint64_t rel_master) {
  if (rel_master <= fs_fdc_done_) return;
  uint64_t const n = rel_master - fs_fdc_done_;
  if (fs_fdc_hot_) {
    // Deferred burst: run the skipped masters now, in order, with a resting
    // bus (the FDC's decode sees non-selected iorq exactly as rest). Its
    // `now` sequence — and so every deadline decision and event timestamp —
    // is identical to a per-cycle run; nothing could have observed the FDC
    // between accesses. Hot frames only (quiet ones take fdc_advance).
    const Bus rest = bus_resting();
    Bus scratch;
    for (uint64_t i = 0; i < n; ++i) {
      scratch = bus_resting();
      fdev_.tick(fdev_.self, &rest, &scratch);
    }
    fs_fdc_hot_ = fdc_quiet(&fdev_) == 0;
  } else {
    fdc_advance(&fdev_, n);
  }
  fs_fdc_done_ = rel_master;
}

void Machine::fs_io_write_event(uint16_t port, uint8_t val, uint64_t rel_t1) {
  fs_irq_tmax_ = 0;  // a write can move the INT geometry (CRTC R0/R2/R7) or
                     // the line itself (GA RMR bit4 rearm) — re-poll next
  const uint64_t j = rel_t1 / 4;
  // GA write-vs-edge ordering (beads-agha): the GA ticks every master and
  // decodes the I/O strobes from T1's first master (z80.cpp MC::IO drives
  // them at t==1), one master BEFORE char j's HSYNC rise reaches it (GA
  // clk at phase 0 → CRTC advances at +1 → rise commits at +2). So a
  // GA-selected write whose T1 sits at T-slot 0 updates req_mode / the
  // rearm BEFORE that char's rise latches the mode or counts the line.
  // Apply the GA half early for exactly that alignment — but only AFTER
  // draining the render: cells < j must still see the pre-write inks (the
  // first cut of this fix skipped that and repainted pending cells with
  // post-write colours — the boot-lockstep regression). Slot ≥ 1 T1s
  // commit after the rise and keep the late path; the CRTC keeps the
  // after-char-j contract in all cases (it samples the bus only at its
  // own clk, one char later).
  const bool ga_early = (port & 0xC000) == 0x4000 && (rel_t1 & 3) == 0;
  if (ga_early) {
    fs_advance_chars(j);  // the chars strictly before the write's own char
    fs_render_below(j);   // the pre-write world, exactly as the late path
    ga_fast_io_write(&gdev_, port, val);
  }
  fs_advance_chars(j + 1);  // through char j (GA edges + mode stamps ride)
  fs_render_below(j);       // cells < j see the pre-write world (no-op when
                            // the early path already drew them)
  fs_audio_steps(j + 1);    // sound steps 0..j pre-op (an AY op with T1 in
                            // µs j takes effect at step j+1)

  if ((port & 0x1000) == 0) {              // printer latch select (A12 = 0)
    const uint64_t at = (4 * rel_t1) + 2;  // one hop after the T1 drive
    if (at > fs_prt_done_) {
      printer_advance(&prtdev_, at - fs_prt_done_);
      fs_prt_done_ = at;
    }
    Bus in = bus_resting();
    in.cpu.addr = port;
    in.cpu.data = val;
    in.cpu.iorq = true;
    in.cpu.wr = true;
    Bus out = bus_resting();
    prtdev_.tick(prtdev_.self, &in, &out);  // the edge-detected latch update
    const Bus rest = bus_resting();
    out = bus_resting();
    prtdev_.tick(prtdev_.self, &rest, &out);  // release the edge detector
    fs_prt_done_ += 2;
  }

  // The bus snoopers — two-phase devices all see the same cycle, so relative
  // order here is free; each decodes its own select.
  const uint8_t caused = crtc_fast_io_write(&cdev_, port, val);
  if (caused & (1u << CRTC_EDGE_VSYNC_RISE)) {
    ga_batch_vsync_rise(&gdev_);
    if (++fs_frames_seen_ >= fs_target_) fs_cut_ = true;
    if (fs_pend_tail_ > fs_pend_head_) {  // the mid-µs beam reset before cell
                                          // j paints
      CrtcCharView& view = fs_pend_buf_[fs_pend_tail_ - 1];
      view.levels |= CRTC_LVL_VSYNC;
      view.edges |= 1u << CRTC_EDGE_VSYNC_RISE;
    }
  }
  if (!ga_early) ga_fast_io_write(&gdev_, port, val);
  mem_fast_io_write(&mdev_, port, val);
  if (fs_asic_on_)  // knock snoop, classic-palette snoop, RMR2 page map
    asic_fast_io_write(&adev_, port, val);
  PpiAyLines lines{};
  if (ppi_fast_io_write(&pdev_, port, val, &lines) != 0) {
    // Relay the AY line change as one event (edge semantics shared with the
    // per-cycle ay_bus). The tape motor level reaches the idle deck through
    // the exit bus synthesis — the deck is gated idle under Fast.
    psg_fast_lines(&sdev_, lines.bdir, lines.bc1, lines.da);
  }
  if ((port & 0x0480) == 0) {  // FDC select (A10 = 0, A7 = 0)
    fs_fdc_to((4 * rel_t1) + 2);
    Bus in = bus_resting();
    in.cpu.addr = port;
    in.cpu.data = val;
    in.cpu.iorq = true;
    in.cpu.wr = true;
    Bus out = bus_resting();
    fdev_.tick(fdev_.self, &in, &out);
    const Bus rest = bus_resting();
    out = bus_resting();
    fdev_.tick(fdev_.self, &rest, &out);  // release the access edge
    fs_fdc_done_ += 2;
    fs_fdc_hot_ = fdc_quiet(&fdev_) == 0;
  }

  // Serial (DART $FAxx / 8253 $FBxx, low byte $DC-$DF) write under Fast. The
  // fast_pending gate never starts a batched frame with the pair non-quiet, so
  // the pair is idle here and a write can only START a transmission. Apply the
  // I/O decode via the synthesized-bus double-tick (the second tick releases
  // the access edge, like the FDC/printer), then BAIL: bit-serial shifting is
  // not batchable, so the per-cycle remainder carries the byte out on the wake
  // serial contract. (Only the first write of a burst reaches here — it bails
  // the rest of the frame to per-cycle; subsequent frames stay per-cycle via
  // the gate until the wire is quiet again.)
  if (wk_serial_on_ && ((port >> 8) == 0xFA || (port >> 8) == 0xFB) &&
      (port & 0xFF) >= 0xDC && (port & 0xFF) <= 0xDF) {
    Bus sin = bus_resting();
    sin.cpu.addr = port;
    sin.cpu.data = val;
    sin.cpu.iorq = true;
    sin.cpu.wr = true;
    Bus sout = bus_resting();
    rsdev_.tick(rsdev_.self, &sin, &sout);
    const Bus srest = bus_resting();
    sout = bus_resting();
    rsdev_.tick(rsdev_.self, &srest, &sout);
    fs_bail_ = true;
  }
}

uint8_t Machine::fs_io_read_event(uint16_t port, uint64_t rel_sample) {
  const uint64_t j = rel_sample / 4;
  fs_advance_chars(j + 1);  // the CRTC level a status read passes through
  uint8_t value = 0xFF;     // the floating bus
  uint8_t v = 0;
  if (crtc_fast_io_read(&cdev_, port, &v) != 0) value = v;
  {
    CrtcRegs cr{};
    crtc_peek(&cdev_, &cr);
    TapeRegs deck{};
    tape_peek(&tdev_, &deck);
    PpiAyLines lines{};
    ppi_fast_lines(&pdev_, &lines);
    const uint8_t ay_da =
        (lines.bdir == 0 && lines.bc1 != 0)
            ? psg_fast_read(&sdev_, lines.kbd_row, 0xFF /* row_ext rest */)
            : 0xFF;
    if (ppi_fast_io_read(&pdev_, port, cr.vsync, deck.level, ay_da, &v) != 0)
      value = v;
  }
  if ((port & 0x0480) == 0) {  // FDC data/status read — edge side effects
    fs_fdc_to((4 * rel_sample) + 2);
    Bus in = bus_resting();
    in.cpu.addr = port;
    in.cpu.iorq = true;
    in.cpu.rd = true;
    Bus out = bus_resting();
    fdev_.tick(fdev_.self, &in, &out);
    value = out.cpu.data;
    const Bus rest = bus_resting();
    out = bus_resting();
    fdev_.tick(fdev_.self, &rest, &out);
    fs_fdc_done_ += 2;
    fs_fdc_hot_ = fdc_quiet(&fdev_) == 0;
  }
  // Serial read under Fast: the pair is quiet (frame gate), so this is a status
  // / idle poll (e.g. RR0 TX-buffer-empty). Apply it via the double-tick — the
  // idle tx/rx_advance are no-ops — and return the latched byte. No bail: a
  // read starts no transmission.
  if (wk_serial_on_ && ((port >> 8) == 0xFA || (port >> 8) == 0xFB) &&
      (port & 0xFF) >= 0xDC && (port & 0xFF) <= 0xDF) {
    Bus sin = bus_resting();
    sin.cpu.addr = port;
    sin.cpu.iorq = true;
    sin.cpu.rd = true;
    Bus sout = bus_resting();
    rsdev_.tick(rsdev_.self, &sin, &sout);
    value = sout.cpu.data;
    const Bus srest = bus_resting();
    sout = bus_resting();
    rsdev_.tick(rsdev_.self, &srest, &sout);
  }
  return value;
}

uint8_t Machine::fsb_mem_read(void* ctx, uint16_t addr, uint64_t /*unused*/) {
  Machine const* m = static_cast<Machine*>(ctx);
  uint8_t v = 0;  // the Plus register page overlays RAM when unlocked+paged
  if (m->fs_asic_on_ && asic_fast_mem_read(&m->adev_, addr, &v) != 0) return v;
  return mem_fast_read(&m->mdev_, addr);
}

void Machine::fsb_mem_write(void* ctx, uint16_t addr, uint8_t val,
                            uint64_t now) {
  Machine* m = static_cast<Machine*>(ctx);
  // A RAM write lands before cell floor(rel/4) displays it (`now` is the
  // µs-aligned memory T1): render the pre-write cells, then commit. The same
  // catch-up covers Plus register-page writes — sprites/palette/config are
  // exactly the raster-trick surface.
  //
  // F8 write filter: catch-up is only OWED when the write can change a byte
  // some PENDING cell (fetch time before this write) would read — i.e. it
  // physically lands in the base-64K window on a 16K page the chain has
  // fetched this frame (fs_vpages_, stamped per view at generation). Cells
  // advanced after the write legitimately see the new byte, in batch exactly
  // as per-cycle. Plus register-page writes (sprites/palette/config) always
  // catch up: their raster effects are the snapshot surface itself.
  const int page_claim = m->fs_asic_on_ && addr >= 0x4000 && addr < 0x8000
                             ? asic_page_armed(&m->adev_)
                             : 0;
  if (page_claim == 0) {
    const int32_t off = mem_fast_write_off(&m->mdev_, addr);
    if (off >= 0 && ((m->fs_vpages_ >> (off >> 14)) & 1U) != 0) {
      const uint64_t j = (now - m->fs_t0_) / 4;
      m->fs_advance_chars(j + 1);
      m->fs_render_below(j);
    }
    mem_fast_write(&m->mdev_, addr, val);
    return;
  }
  const uint64_t j = (now - m->fs_t0_) / 4;
  m->fs_advance_chars(j + 1);
  m->fs_render_below(j);
  asic_fast_mem_write(&m->adev_, addr, val);  // claims: guard == page_claim
  // A page write can enable a DMA channel; the sequencer steals bus
  // masters the batch driver cannot model — leave the frame's remainder
  // to the per-cycle loop (F7 scope note in asic.h).
  if (asic_dma_active(&m->adev_) != 0) m->fs_bail_ = true;
  // The RAM underneath is vetoed.
}

uint8_t Machine::fsb_io_read(void* ctx, uint16_t port, uint64_t now) {
  Machine* m = static_cast<Machine*>(ctx);
  return m->fs_io_read_event(port, now - m->fs_t0_);
}

void Machine::fsb_io_write(void* ctx, uint16_t port, uint8_t val,
                           uint64_t now) {
  Machine* m = static_cast<Machine*>(ctx);
  m->fs_io_write_event(port, val, now - m->fs_t0_);
}

uint8_t Machine::fsb_int_ack(void* ctx, uint64_t now) {
  Machine* m = static_cast<Machine*>(ctx);
  m->fs_irq_tmax_ = 0;  // the line falls here — the cached poll is stale
  m->fs_advance_chars(((now - m->fs_t0_) / 4) + 1);
  ga_batch_int_ack(&m->gdev_);
  // On a Plus the ASIC drives its IM2 vector and clears its sources on this
  // very cycle; the classic bus floats.
  if (asic_vid_active(&m->adev_) != 0) return asic_batch_int_ack(&m->adev_);
  return 0xFF;
}

bool Machine::run_frame_fast(VideoRegs& vr, uint32_t target) {
  // ENTRY CONTRACT (checked by the caller): the Z80 sits at a clean boundary
  // (z80_batch_ready) with the committed bus at clk.phase == 0, every device
  // per-cycle-synced. The grid invariant then puts tstates ≡ 0 mod 4.
  fs_t0_ = z80_batch_tstates(&zdev_);
  fs_chars_ = fs_cells_ = 0;
  fs_audio_steps_ = fs_audio_accs_ = 0;
  fs_fdc_done_ = fs_prt_done_ = 0;
  fs_pend_head_ = fs_pend_tail_ = 0;
  fs_vpages_ = 0;
  fs_vram_ = mem_video_ram(&mdev_);
  fs_target_ = target;
  video_peek(&vdev_, &vr);
  fs_frames_seen_ = vr.frames;
  fs_cut_ = false;
  fs_bail_ = false;
  fs_asic_on_ = asic_vid_active(&adev_) != 0;
  {
    LightGunRegs lg{};
    light_gun_peek(&lgdev_, &lg);
    fs_lpen_on_ = lg.plugged != 0;
  }
  fs_lpen_pending_ =
      false;  // no deferred latch carries into this frame's batch
  fs_fdc_hot_ = fdc_quiet(&fdev_) == 0;  // gate said quiet; stay honest
  fs_irq_cache_ = fs_irq() ? 1 : 0;      // the boundary-0 poll (zero chars)
  fs_irq_tmax_ = 0;  // first real boundary computes a horizon
  const Z80BatchIO bio{this,
                       &Machine::fsb_mem_read,
                       &Machine::fsb_mem_write,
                       &Machine::fsb_io_read,
                       &Machine::fsb_io_write,
                       &Machine::fsb_int_ack};

  const long bound = kMasterPerFrame;  // instruction-count safety bound
  for (long guard = 0; guard < bound && !fs_cut_ && !fs_bail_; ++guard) {
    const uint64_t rel = z80_batch_tstates(&zdev_) - fs_t0_;
    if (z80_batch_halted(&zdev_) != 0) {
      fs_irq_tmax_ = 0;  // this path polls fs_irq() directly per hop — force
                         // the fall-through boundary to re-poll too
      fs_advance_chars(fs_visible(rel));
      if (fs_cut_) break;
      const int iff1 = z80_batch_iff1(&zdev_);
      const bool serviceable = iff1 != 0 && fs_irq();
      if (!serviceable) {
        // Wait for the next event edge-exactly when an interrupt could wake
        // the CPU (one char per hop — the F4 pattern); an unserviceable HALT
        // free-runs a scanline at a time until the frame cut.
        const uint64_t hop = (iff1 != 0) ? 1 : 64;
        fs_advance_chars(fs_chars_ + hop);
        const uint64_t vis = (4 * (fs_chars_ - 1)) + 1;
        if (vis > rel) z80_batch_halt(&zdev_, static_cast<uint32_t>(vis - rel));
        continue;
      }
      // irq visible and serviceable: fall through — the wake consumes no time.
    }
    const uint64_t b_al = (rel + 3) & ~3ULL;
    if (b_al > fs_irq_tmax_) {
      fs_advance_chars(fs_visible(b_al));
      if (fs_cut_) break;  // per-cycle stops before this instruction could run
      fs_irq_cache_ = fs_irq() ? 1 : 0;
      // Boundaries whose visible-char count stays below the next INT-path
      // event char reuse this poll: fs_visible(t) <= C+h-1  ⟺  t <= 4(C+h-1).
      // A mid-instruction catch-up can overrun the event char, but its access
      // T1 then sits at or past the horizon, so the instruction's END boundary
      // lands past tmax and re-polls — the cache is never consulted stale.
      fs_irq_tmax_ = 4 * (fs_chars_ + crtc_irq_horizon_chars(&cdev_) - 1);
    }
    const int irq_now = fs_irq_cache_;
    if (tap_count_ != 0 && z80_batch_will_accept(&zdev_, irq_now) == 0) {
      // Firmware-vector taps (console TXT/BDOS): per-cycle the probe latches
      // the M1 fetch at the tap address — here the boundary IS that fetch
      // edge (registers still hold the caller's values), unless a pending
      // interrupt preempts it (then per-cycle fires only on the post-ISR
      // re-arrival, and so do we). Batch scope: instruction boundaries only —
      // no prefix-byte M1s, no HALT-refetch repeats (not firmware-vector
      // shapes; host-visible console bytes only, machine state untouched).
      const uint16_t pc_now = z80_batch_pc(&zdev_);
      for (int t = 0; t < tap_count_; ++t)
        if (taps_[t].addr == pc_now) taps_[t].fn(taps_[t].ctx, pc_now);
    }
    if (instr_hook_) {  // debug trace: record the instruction about to run
      Z80Regs r;
      z80_peek(&zdev_, &r);
      // Guard on instr_count so the per-cycle prelude and this batch loop do
      // not both record the boundary instruction at the hand-off (they share
      // instr_hook_last_). Same key the per-cycle path uses.
      if (r.instr_count != instr_hook_last_) {
        instr_hook_last_ = r.instr_count;
        instr_hook_(instr_hook_ctx_, &r);
      }
    }
    z80_batch_step(&zdev_, &bio, irq_now, 0xFF, /*grid=*/1);
  }

  // EXIT: materialize a per-cycle-resumable machine at the CPU's boundary.
  const uint64_t relB = z80_batch_tstates(&zdev_) - fs_t0_;
  if (relB == 0) return false;  // nothing ran — caller keeps the frame
  const uint64_t m_next = (4 * (relB - 1)) + 2;  // next per-cycle master
  fs_advance_chars(fs_visible(relB));  // == chars a per-cycle run reaches
  fs_render_below(fs_chars_);  // CPU-ahead cells sit inside VSYNC: no pixels
  // Audio: all sound steps a per-cycle run reaches (== the char count), then
  // the final trailing accumulate ONLY if its master (rel 16k+15) has passed
  // — otherwise the resumed per-cycle loop runs it at that exact master.
  fs_audio_steps(fs_chars_);
  if (fs_audio_accs_ < fs_audio_steps_ &&
      (16 * (fs_audio_steps_ - 1)) + 15 <= m_next - 1) {
    accumulate_audio();
    fs_audio_accs_++;
  }
  fs_fdc_to(m_next);
  if (m_next > fs_prt_done_) printer_advance(&prtdev_, m_next - fs_prt_done_);
  ga_advance(&gdev_, m_next);  // the ÷16 divider lands exactly
  board_.master_cycles += m_next;

  // Synthesize the committed bus of master m_next-1: the GA's clock fabric
  // (phase = m_next mod 16 — always ≡ 2 mod 4, so never a fetch-data commit),
  // the CRTC's driven levels, the PPI's published lines, the deck's rdata,
  // the GA's INT. The CPU is idle at a boundary: rest lines, hold released.
  Bus bus = bus_resting();
  CrtcRegs cr{};
  crtc_peek(&cdev_, &cr);
  ga_clock_out(static_cast<uint8_t>(m_next & 0x0F), cr.ma, cr.ra, &bus);
  bus.vid.hsync = cr.hsync != 0;
  bus.vid.vsync = cr.vsync != 0;
  bus.vid.dispen = cr.dispen != 0;
  bus.vid.ma = cr.ma;
  bus.vid.ra = cr.ra;
  bus.vid.frame_line = cr.scanline;
  PpiAyLines lines{};
  ppi_fast_lines(&pdev_, &lines);
  bus.ay.bdir = lines.bdir != 0;
  bus.ay.bc1 = lines.bc1 != 0;
  bus.ay.kbd_row = lines.kbd_row;
  bus.ay.da = (lines.bdir == 0 && lines.bc1 != 0)
                  ? psg_fast_read(&sdev_, lines.kbd_row, 0xFF)
                  : lines.da;
  bus.tape.motor = lines.tape_motor != 0;
  bus.tape.wdata = lines.tape_wdata != 0;
  {
    TapeRegs deck{};
    tape_peek(&tdev_, &deck);
    bus.tape.rdata = deck.level != 0;
  }
  if (fs_irq()) bus.cpu.irq = true;
  board_.bus = bus;
  z80_batch_release_bus(&zdev_);
  ga_batch_set_sync(&gdev_, cr.hsync, cr.vsync);
  video_batch_set_sync(&vdev_, cr.hsync, cr.vsync);
  if (fs_asic_on_) asic_batch_set_sync(&adev_, cr.hsync, cr.scanline);
  video_peek(&vdev_, &vr);
  // On a bail the frame is NOT complete: the caller's per-cycle loop resumes
  // from the state materialized above and finishes it exactly.
  return fs_cut_;
}

void Machine::recompose_active() {
  if (active_override_) return;  // KONCPC_ACTIVE pins the set manually
  // Drop structurally-dormant peripherals from the per-cycle tick list. Each of
  // these ticks opens with `if (!plugged) return;` — unplugged, it drives
  // nothing and mutates nothing, so skipping it is byte-identical to ticking it
  // (verified: bench framebuffer checksum is unchanged) yet saves an indirect
  // call every master cycle. Everything else (the core chips, probe, tape,
  // printer) always ticks. Matched by state pointer, since board_.dev[] holds
  // copies of these Device views.
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  int n = 0;
  for (int i = 0; i < board_.count; ++i) {
    const void* s = board_.dev[i].self;
    bool dormant = false;
    if (s == addev_.self) {
      AmdrumRegs r;
      amdrum_peek(&addev_, &r);
      dormant = !r.plugged;
    } else if (s == axdev_.self) {
      AmxRegs r;
      amx_peek(&axdev_, &r);
      dormant = !r.plugged;
    } else if (s == mfdev_.self) {
      Mf2Regs r;
      mf2_peek(&mfdev_, &r);
      dormant = !r.plugged;
    } else if (s == swdev_.self) {
      SmartwatchRegs r;
      smartwatch_peek(&swdev_, &r);
      dormant = !r.plugged;
    } else if (s == sfdev_.self) {
      Sf2Regs r;
      sf2_peek(&sfdev_, &r);
      dormant = !r.plugged;
    } else if (s == m4dev_.self) {
      M4Regs r;
      m4_peek(&m4dev_, &r);
      dormant = !r.plugged;
    } else if (s == adev_.self) {
      dormant = !asic_vid_active(&adev_);  // returns the ASIC's plugged flag
    } else if (s == rsdev_.self) {
      Rs232Regs r;
      rs232_peek(&rsdev_, &r);
      dormant = !r.plugged;
    } else if (s == pldev_.self) {
      PlotterRegs r;
      plotter_hp7470a_peek(&pldev_, &r);
      dormant = !r.plugged;
    } else if (s == lgdev_.self) {
      LightGunRegs r;
      light_gun_peek(&lgdev_, &r);
      dormant = !r.plugged;
    } else if (s == prdev_.self) {
      // The debug probe: inert while nothing is armed (no bp/wp/io/tap/latch).
      // Its tick no-ops in that state, so skipping it is byte-identical.
      dormant = !probe_active(&prdev_);
    }
    if (!dormant) board_.tick_order[n++] = i;
  }
  board_.active_count = n;

  // Wake-tier validity: the hand-scheduled tick_wake() knows the wake contract
  // of exactly the canonical core (build() indices 0..10: ga crtc ppi psg mem
  // video z80 fdc probe tape printer) plus the ASIC when plugged. Any other
  // active device (a plugged expansion with its own bus behaviour) has no
  // contract in the scheduler — fall back to Faithful.
  wake_valid_ = true;
  wk_serial_on_ = false;
  wk_light_gun_on_ = false;
  for (int k = 0; k < board_.active_count; ++k) {
    const int idx = board_.tick_order[k];
    const void* dself = board_.dev[idx].self;
    // The RS232+plotter pair and the light gun now carry wake contracts
    // (wake_slot dispatches each on its own predicate), so they no longer
    // degrade the tier.
    const bool serial = dself == rsdev_.self || dself == pldev_.self;
    const bool light_gun = dself == lgdev_.self;
    if (serial) wk_serial_on_ = true;
    if (light_gun) wk_light_gun_on_ = true;
    const bool known = idx <= 10 || dself == adev_.self || serial || light_gun;
    if (!known) {
      wake_valid_ = false;
      break;
    }
  }
  // Fast-tier validity (F7): the canonical core, with or without the ASIC —
  // the batch contracts cover the Plus register page, PRI, split/scroll and
  // the compositor. DMA sound is frame-gated instead (run_frame): a frame
  // with a channel enabled runs per-cycle, and a mid-frame enable bails.
  // The RS232+plotter pair is fast-valid too. The batch path has no per-cycle
  // serial dispatch, but (a) the fast_pending gate never starts a batched frame
  // with a byte in flight, and (b) the Fast I/O hooks apply serial accesses and
  // BAIL the moment a write could start a transmission, so the per-cycle
  // remainder carries the bit shifting (fs_io_write_event). Net: idle frames
  // run full Fast; only active transmission drops to per-cycle for that
  // stretch.
  fast_valid_ = wake_valid_;
}

void Machine::run_frame() {
  if (!built_) return;
  recompose_active();  // frame-boundary: pick up any plug/unplug since last
                       // frame
  audio_.clear();
  out_q_.clear();
  VideoRegs vr{};
  video_peek(&vdev_, &vr);
  const uint32_t target = vr.frames + 1;
  // Armed-at-frame-start is stable: comparators change on this thread only.
  const bool watch_probe = probe_armed(&prdev_) != 0;
  {  // plug state changes only between frames (host thread) — cache for audio
    AmdrumRegs drum{};
    amdrum_peek(&addev_, &drum);
    amdrum_on_ = drum.plugged != 0;
  }
#ifndef SOLDERED
  // Latch the tier ONCE per frame — the switch is frame-boundary only (plan §14
  // Q7); it never changes mid-frame. effective_run_tier() applies the
  // composition-aware degradation (Soldered/Wake → Faithful off-canonical).
  const RunTier tier = effective_run_tier();
  const bool soldered = tier == RunTier::Soldered;
  // Fast tier (F6): frame-quiet gates — modes needing per-cycle attention run
  // the frame on the per-cycle path instead (the ladder's next rung is the
  // loop below with wake=false → board_tick). The FDC must be quiet at entry
  // (mid-frame heat is handled by the driver's burst path). Entry happens
  // mid-frame at the first clean point: a per-cycle frame end usually leaves
  // the Z80 mid-instruction, so the loop ticks per-cycle until the batch
  // engine can take over.
  bool fast_pending = false;
  if (tier == RunTier::Fast) {
    TapeRegs deck{};
    tape_peek(&tdev_, &deck);
    // digiblaster_: the batch audio defers each unit's accumulate until the
    // next sound step, which would read a printer-latch write landing in the
    // gap one microsecond early — per-µs DAC mixing stays on the per-cycle
    // tiers until it earns its own oracle.
    // Taps do NOT gate the batch: run_frame_fast fires them itself at the
    // instruction boundary (F8 — the GUI console taps are always armed, and
    // a tap-gated Fast tier could never engage there). A latched probe hit
    // still forces per-cycle until the host acks it.
    fast_pending =
        deck.playing == 0 && deck.line_mode == 0 && !watch_probe &&
        probe_pending(&prdev_, nullptr) == 0 && !out_capture_ &&
        line_q_pos_ >= line_q_.size() && cycle_hook_ == nullptr &&
        !digiblaster_ && fdc_quiet(&fdev_) != 0 &&
        asic_dma_active(&adev_) == 0 &&
        // The serial pair has a Fast-path bail (fs_io_write_event),
        // but bit shifting itself is per-cycle: never START a batched
        // frame with a byte in flight or the plotter mid-drain.
        (!wk_serial_on_ ||
         (rs232_quiet(&rsdev_) != 0 && plotter_hp7470a_quiet(&pldev_) != 0));
  }
  // Wake tier (Gate B6): frame-start is the one boundary where host-side state
  // can have changed under the scheduler (pokes, key rows, deck buttons,
  // snapshot loads, a debug single-step via board_tick) — refresh the caches
  // and force-wake everything for the first cycle so no shadow is stale.
  // The tier ladder (F8): a Fast frame that cannot batch — gates failed, or
  // the batch bailed mid-frame — runs the WAKE schedule, the byte-exact next
  // rung, never plain per-cycle. (Before this, an un-batchable Fast frame
  // fell all the way to board_tick: faithful speed under a Fast request.)
  const bool wake = tier == RunTier::Wake || tier == RunTier::Fast;
  if (wake) {
    wk_force_ = true;
    TapeRegs deck{};
    tape_peek(&tdev_, &deck);
    wk_tape_live_ = deck.playing != 0 || deck.line_mode != 0;
    wk_probe_on_ = probe_active(&prdev_) != 0;
    wk_asic_on_ = asic_vid_active(&adev_) != 0;
    wk_fdc_quiet_ = fdc_quiet(&fdev_) != 0;  // snapshot loads / host pokes
    {
      LightGunRegs lg{};
      light_gun_peek(&lgdev_, &lg);
      wk_light_gun_on_ = lg.plugged != 0;
    }
    // Serial pair quiet snapshot: unplugged ⇒ always quiet (never blocks
    // elision); plugged ⇒ the real UART-idle state, so a byte in flight at
    // frame start keeps the pair (and elision) awake from slot 0.
    wk_serial_quiet_ = !wk_serial_on_ || (rs232_quiet(&rsdev_) != 0 &&
                                          plotter_hp7470a_quiet(&pldev_) != 0);
  }
  bool vsync_seen = board_.bus.vid.vsync;
  // µs-chunk fast path (stage 3): eligible while the frame is quiet — no tape
  // capture, no line-in feed, no cycle hook, no armed ICE comparators. Those
  // modes need genuinely per-cycle attention and simply stay on the per-cycle
  // path below (taps are fine: the chunk drains them per slot).
  const bool chunk_ok = wake && !watch_probe && !out_capture_ &&
                        line_q_pos_ >= line_q_.size() &&
                        cycle_hook_ == nullptr &&
                        instr_hook_ == nullptr;  // trace needs every retire
#endif
  const long bound = kMasterPerFrame * 2;
  long i = 0;
  while (i < bound && vr.frames < target) {
#ifndef SOLDERED
    // Entry needs a GENUINE GA phase-0 commit — clk.cpu rides along on those
    // (phase & 3 == 0), while the power-on resting bus fakes phase 0 with the
    // clocks down; anchoring there would put the batch one master early.
    if (fast_pending && z80_batch_ready(&zdev_) != 0 &&
        board_.bus.clk.phase == 0 && board_.bus.clk.cpu) {
      if (run_frame_fast(vr, target)) {  // the frame completed batched
        fast_frames_run_++;
        break;
      }
      fast_pending = false;  // could not engage — finish per-cycle
    }
#endif
#ifndef SOLDERED
    // Chunk when aligned at the µs boundary and away from the VSYNC
    // neighbourhood (frame completion can only move there; the chunk still
    // mirrors the per-cycle peek gate slot-for-slot, so entering near VSYNC
    // would be correct too — just pointless).
    // Alignment: wake_slot<P> serves the cycle that READS clk.phase == P, so
    // the 0..15 chain starts when the committed phase is 0 (slot 15's commit
    // then leaves phase 0 again — chunks tile without re-checking).
    if (chunk_ok && !wk_force_ && board_.bus.clk.phase == 0 &&
        !board_.bus.vid.vsync && !vsync_seen && i + 16 <= bound) {
      i += run_wake_us(vr, target, vsync_seen);
      continue;
    }
#endif
    if (cycle_hook_) cycle_hook_(cycle_hook_ctx_, board_.master_cycles);  // B2
#ifdef SOLDERED
    tick_soldered();  // monomorphic measurement baseline (Gate B4)
#else
    if (wake) {
      tick_wake();  // clock-wake scheduled dispatch (Gate B6)
    } else if (soldered) {
      tick_soldered();  // Soldered tier: devirtualized direct dispatch
    } else {
      board_tick(&board_);  // Faithful tier: pluggable, observable, steppable
    }
#endif
    if (instr_hook_) {  // debug trace: fire once per per-cycle instruction
                        // retire
      Z80Regs r;
      z80_peek(&zdev_, &r);
      if (r.instr_count != instr_hook_last_) {
        instr_hook_last_ = r.instr_count;
        instr_hook_(instr_hook_ctx_, &r);
      }
    }
    capture_tape_output();
    feed_tape_line_in();
    service_taps(board_.bus);
    if (watch_probe && probe_pending(&prdev_, nullptr)) break;  // ICE halt
    if (board_.bus.clk.psg) accumulate_audio();
    i++;
#ifndef SOLDERED
    if (wake) {
      // Frame completion can only move while VSYNC is on the bus (frames
      // increments at the rise video sees, thousands of cycles into the pulse's
      // neighbourhood) — peeking only around it is exit-cycle-identical and
      // drops ~300k video_peek calls per frame.
      const bool near_vsync = board_.bus.vid.vsync || vsync_seen;
      vsync_seen = board_.bus.vid.vsync;
      if (near_vsync) video_peek(&vdev_, &vr);
      continue;
    }
#endif
    video_peek(&vdev_, &vr);
  }
#ifndef SOLDERED
  // Frame-boundary settle: re-apply the cycles the scheduler skipped so each
  // chip's counters are exactly where a per-cycle run would leave them — the
  // state a snapshot, the differential harness, or a debugger now observes.
  if (wake && wk_prt_skip_ != 0) {
    printer_advance(&prtdev_, wk_prt_skip_);
    wk_prt_skip_ = 0;
  }
  if (wake && wk_ga_skip_ != 0) {
    ga_advance(&gdev_, wk_ga_skip_);
    wk_ga_skip_ = 0;
  }
  if (wake && wk_fdc_skip_ != 0) {
    fdc_advance(&fdev_, wk_fdc_skip_);
    wk_fdc_skip_ = 0;
  }
#endif
}

bool Machine::add_tap(uint16_t addr, TapFn fn, void* ctx) {
  if (tap_count_ >= 4 || fn == nullptr) return false;
  // Taps fire from service_taps' own fetch-edge check, not the debug probe — so
  // installing one does NOT arm the per-cycle probe. Seed the edge state from
  // the current bus so a tap added while an M1 fetch is on the wire cannot
  // spuriously fire on the next cycle.
  tap_prev_fetch_ =
      board_.bus.cpu.m1 && board_.bus.cpu.mreq && board_.bus.cpu.rd;
  taps_[tap_count_++] = Tap{addr, fn, ctx};
  return true;
}

void Machine::clear_taps() {
  tap_count_ = 0;
  tap_prev_fetch_ = false;
}

void Machine::io_write(uint16_t port, uint8_t val) const {
  Bus in = bus_resting();
  in.cpu.addr = port;
  in.cpu.data = val;
  in.cpu.iorq = true;
  in.cpu.wr = true;
  Bus out = bus_resting();
  // The passive chips latch I/O writes level-triggered; one crafted cycle
  // applies the write once. The Z80 is off the bus (a DMA master owns it).
  gdev_.tick(gdev_.self, &in, &out);
  cdev_.tick(cdev_.self, &in, &out);
  pdev_.tick(pdev_.self, &in, &out);
  mdev_.tick(mdev_.self, &in, &out);
  prtdev_.tick(prtdev_.self, &in, &out);  // printer latch (A12 = 0)
  addev_.tick(addev_.self, &in, &out);    // AmDrum, when plugged
  mfdev_.tick(mfdev_.self, &in, &out);    // MF2 shadow snoops the replay
  m4dev_.tick(m4dev_.self, &in, &out);    // M4 snoops ROM-select on replay
  swdev_.tick(swdev_.self, &in, &out);    // SmartWatch tracks the ROM enable
  rsdev_.tick(rsdev_.self, &in, &out);    // RS232 card latches DART/8253 writes
}

void Machine::psg_poke(uint8_t reg, uint8_t val) {
  psg_poke_reg(&sdev_, reg, val);
}
void Machine::psg_select(uint8_t sel) { psg_poke_sel(&sdev_, sel); }

size_t Machine::ram_size() const { return 0x10000 + xmem_.size(); }

// The DK'Tronics Silicon Disc (silicon-disc-device.md): battery-backed RAM at
// expansion banks 4-7. Not a Device — sizing + persistence of the region the
// memory Device already banks. Grow the expansion to 512K so banks 4-7 exist;
// re-attach (resize may reallocate). Existing contents are preserved.
void Machine::enable_silicon_disc(bool on) {
  const size_t want = on ? kSiliconEnd : 0x10000;
  if (xmem_.size() == want) return;
  xmem_.resize(want, 0);
  mem_attach_expansion(&mdev_, xmem_.data(), xmem_.size());
}

void Machine::silicon_disc_load(const uint8_t* src, size_t len) {
  if (src == nullptr || xmem_.size() < kSiliconEnd) return;
  size_t const n = len < kSiliconSize ? len : kSiliconSize;
  std::memcpy(xmem_.data() + kSiliconStart, src, n);
}

// NOLINTNEXTLINE(readability-non-const-parameter): pointer written through a
// cast or passed to a non-const callee
void Machine::silicon_disc_save(uint8_t* dst, size_t len) const {
  if (dst == nullptr || xmem_.size() < kSiliconEnd) return;
  size_t const n = len < kSiliconSize ? len : kSiliconSize;
  std::memcpy(dst, xmem_.data() + kSiliconStart, n);
}

uint8_t Machine::ram_read(size_t addr) const {
  if (addr < 0x10000) return mem_read_ram(&mdev_, static_cast<uint16_t>(addr));
  return xmem_[addr - 0x10000];
}

void Machine::ram_write(size_t addr, uint8_t val) {
  if (addr < 0x10000) {
    mem_write_ram(&mdev_, static_cast<uint16_t>(addr), val);
    return;
  }
  xmem_[addr - 0x10000] = val;
}

Z80Regs Machine::regs() const {
  Z80Regs regs{};
  z80_peek(&zdev_, &regs);
  return regs;
}

void Machine::set_regs(const Z80Regs& regs) { z80_poke(&zdev_, &regs); }

uint8_t Machine::peek_mem(uint16_t addr) const {
  return mem_peek_cpu(&mdev_, addr);
}

void Machine::poke_mem(uint16_t addr, uint8_t val) {
  // Host pokes always land and never trip watchpoints — parity with the
  // legacy core, whose debug path writes via write_mem_no_watchpoint.
  mem_poke_cpu(&mdev_, addr, val);
}

// NOLINTNEXTLINE(readability-make-member-function-const): mutates state via a
// free function taking a non-const pointer
void Machine::step_instruction() {
  if (!built_) return;
  Z80Regs before{};
  z80_peek(&zdev_, &before);
  Z80Regs now = before;
  // Bound: the longest instruction is 23 T-states; x4 master cycles per
  // T-state plus WAIT stretching and interrupt entry stays far below this.
  for (int i = 0; i < 4096 && now.instr_count == before.instr_count; ++i) {
    board_tick(&board_);
    z80_peek(&zdev_, &now);
  }
}

}  // namespace subcycle
