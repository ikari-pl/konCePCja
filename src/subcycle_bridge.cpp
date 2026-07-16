/* subcycle_bridge.cpp — see subcycle_bridge.h. */

#include "subcycle_bridge.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

#include "amdrum.h"        // legacy g_amdrum: the UI toggles its enabled flag
#include "amx_mouse.h"     // legacy g_amx_mouse: SDL events fill its counters
#include "drive_sounds.h"  // host audio overlay: motor hum / seek clicks
#include "flux_ingest.h"   // flux::to_scp: unified flux-container dispatcher
#include "hw/asic.h"
#include "hw/crtc.h"
#include "hw/device.h"  // Device (Save-As FDC handle)
#include "hw/fdc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/ppi.h"      // ppi_set_printer_ready: the /BUSY strap
#include "hw/printer.h"  // printer_drain_events -> the pfoPrinter capture
#include "hw/psg.h"
#include "hw_views.h"     // TAPE_LEVEL_HIGH/LOW for the mirrored scope level
#include "imgui_state.h"  // tape_decoded_buf: the BITS-view scope ring
#include "koncepcja.h"
#include "log.h"
#include "m4board.h"  // legacy g_m4board: the deferred command executor
#include "serial_interface.h"  // g_serial_interface config → the serial pair
#include "silicon_disc.h"  // legacy g_silicon_disc: the battery buffer anchor
#include "smartwatch.h"  // legacy g_smartwatch: the UI toggles its enabled flag
#include "symbiface.h"   // legacy g_symbiface: FIFO fill (SDL/IPC) + config
#include "tape_line_in.h"  // auto-route the tape data signal to its own stream
#include "trace.h"  // g_trace: per-instruction debug recorder (engine=1 seam)
#include "zip_archive.h"  // read_media_file: first media entry of a .zip slot

extern t_drive driveA;  // legacy drive struct: UI reads its altered flag
extern t_drive driveB;  // ...and both drives' mechanics (drive_status)
extern t_FDC FDC;       // motor latch for the status surfaces
#include "subcycle/machine.h"
#include "z80_view.h"  // legacy: the Wave-1 view struct + bench lists (transitional)

extern t_CPC CPC;
extern std::string chROMFile[4];  // per-model system ROM names (kon_cpc_ja.cpp)
extern std::unique_ptr<byte[]>
    pbCartridgeImage;  // parsed CPR banks (cartridge.cpp)
extern t_z80regs z80;  // legacy structs: Wave-1 view buffers
extern t_CRTC CRTC;    // (all die with the legacy files)
extern t_GateArray GateArray;
extern t_PSG PSG;
// legacy tape scope level (tape.cpp); engine=1 mirrors it

namespace {

enum class PendingMedia : std::uint8_t {
  kNone,
  kDisk,
  kFlux,
  kEject,
  kTape,
  kTapeEject
};

struct Bridge {
  subcycle::Machine machine;
  std::vector<uint8_t> fb;        // RGB24 native frame the machine renders into
  SDL_Surface* fbsurf = nullptr;  // SDL view of fb for the scaled blit
  SDL_Surface* fbconv = nullptr;  // dst-format staging: convert-then-stretch
                                  // keeps SDL on its fast blit paths (F8: the
                                  // one-pass scale+convert fell into
                                  // SDL_Blit_Slow — ~10 ms/frame on E-cores)
  std::vector<uint8_t> rom, amsdos, media;  // machine wiring: must outlive it
  std::vector<uint8_t>
      media_b;                  // drive B DSK image (unit 1): also must outlive
  std::vector<uint8_t> mf2rom;  // Multiface II 8K ROM (optional)
  std::atomic<bool> mf2_stop{false};  // deferred STOP (UI -> Z80 thread)
  std::vector<uint8_t> ide_img[2];    // Symbiface IDE images (owned)
  std::string ide_path[2];            // their files, for write-back
  bool sf2_ide_loaded = false;
  std::vector<uint8_t> m4rom;  // M4 Board 16K ROM (owned)
  bool m4_loaded = false;
  std::vector<uint8_t> serialrom;   // SI card serial BIOS 16K ROM (owned)
  std::vector<uint8_t> tape_media;  // cassette in the deck (owner)
  uint64_t next_deadline = 0;       // 50 Hz pacing (performance-counter ticks)
  bool active = false;

  // Tape host-side mirror (engine=1): each frame debug_sync mirrors the deck's
  // live wires back to the legacy tape UI/SFX, which the sub-cycle path
  // bypasses.
  bool prev_tape_running = false;  // motor&&play edge → drive_sounds_tape
  bool tape_lineout_auto =
      false;  // WE auto-armed line-out (don't fight IPC arm)
  // Deferred tape block-seek (UI thread requests, Z80 thread applies at the
  // frame boundary): the target block ordinal, or ~0 for "none".
  std::atomic<uint32_t> tape_seek_req{~uint32_t{0}};

  // Run-tier policy (subcycle_bridge.h): Auto follows debug_engaged, which
  // subcycle_bridge_debug_sync() refreshes each frame from the breakpoint /
  // watchpoint / IO-breakpoint lists it already walks for the probe.
  BridgeTierPolicy tier_policy = BridgeTierPolicy::Auto;
  bool tier_env_pinned = false;
  bool debug_engaged = false;

  // Async tier benchmark (Z80-thread-owned except the atomics).
  std::atomic<int> bench_want{0};  // UI arms; Z80 thread consumes
  std::atomic<int> bench_fps[4] = {{-1}, {-1}, {-1}, {-1}};  // fast..faithful
  int bench_tier = -1;  // -1 idle; else tier being sampled
  int bench_frames = 0;
  double bench_secs = 0.0;
  std::vector<uint8_t> bench_snap;  // disposable-run state snapshot

  // Hot-swap handoff (UI thread → Z80 thread): the FDC's media is live wiring
  // and must not change mid-tick, so swaps land here and the emulation thread
  // applies them at its next frame boundary.
  std::mutex swap_mutex;
  std::vector<uint8_t> swap_bytes;
  std::atomic<PendingMedia> swap_kind{PendingMedia::kNone};
  std::atomic<uint8_t> swap_unit{0};
};
Bridge g_bridge;
const std::vector<int16_t> g_empty_audio;

std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return {(std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>()};
}

bool ends_with(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Lowercased 4-char extension (".dsk", ".scp", ...) of `path`, or "" if too
// short. Every media extension we route on is exactly 4 chars.
std::string lower_ext(const std::string& path) {
  if (path.size() < 4) return "";
  std::string e = path.substr(path.size() - 4);
  for (char& c : e)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

// Is `ext` one of the flux containers the sub-cycle FDC plays (as SCP)?
bool is_flux_ext(const std::string& ext) {
  return ext == ".ipf" || ext == ".raw" || ext == ".scp" || ext == ".hfe" ||
         ext == ".a2r";
}

// Read a disk-media slot file's bytes. A .zip is read as its FIRST media
// entry (the same classification the legacy slot loader applies) and `ext`
// is rewritten to the inner entry's extension so the flux/DSK dispatch sees
// the real container — the raw archive bytes would just fail insert_disk.
std::vector<uint8_t> read_media_file(const std::string& path,
                                     std::string& ext) {
  ext = lower_ext(path);
  if (ext != ".zip") return read_file(path);

  zip::t_zip_info zi;
  zi.filename = path;
  zi.extensions = ".dsk.ipf.raw.scp.hfe.a2r";
  if (zip::dir(&zi) != 0 || zi.filesOffsets.empty()) {
    LOG_ERROR("subcycle engine: no disk media inside " << path);
    return {};
  }
  zi.dwOffset =
      zi.filesOffsets[0].second;  // Use the first media entry found by dir().
  FILE* f = nullptr;
  if (zip::extract(zi, &f) != 0 || f == nullptr) {
    LOG_ERROR("subcycle engine: cannot extract " << zi.filesOffsets[0].first
                                                 << " from " << path);
    return {};
  }
  std::vector<uint8_t> out;
  uint8_t buf[65536];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    out.insert(out.end(), buf, buf + n);
  fclose(f);
  ext = lower_ext(zi.filesOffsets[0].first);
  LOG_INFO("subcycle engine: " << path << " -> " << zi.filesOffsets[0].first
                               << " (" << out.size() << " bytes)");
  return out;
}

// Bridges the sub-cycle FDC's mechanical event ring to the host's procedural
// drive-sound generator (drive_sounds.cpp): the machine drains FDC events and
// this forwards motor/seek to drive_sounds. Events-only — the cosmetic audio
// is NOT summed into the AY output; it renders on its own SDL stream
// (drive_sounds_fill_stereo) and mixes at the device. drive_sounds self-gates
// on the [sound] disk_sounds flag. Under engine=1 the legacy
// io_register_fdc_motor_hook path never runs, so this overlay is the ONLY route
// from the FDC to the drive-sound generator for the sub-cycle engine.
struct DriveSoundOverlay : subcycle::AudioOverlay {
  void events(const FdcEvent* ev, int n) override {
    for (int i = 0; i < n; ++i) {
      switch (ev[i].type) {
        case FDC_EV_MOTOR_ON:
          drive_sounds_motor(true);
          break;
        case FDC_EV_MOTOR_OFF:
          drive_sounds_motor(false);
          break;
        case FDC_EV_STEP:
          drive_sounds_seek();
          break;
        default:
          break;  // MOTOR_READY / INDEX: no audible effect
      }
    }
  }
};
DriveSoundOverlay g_drive_overlay;

// Scaled blit of the machine's framebuffer into the app surface (defined with
// the frame path below; also serves the paused "repaint").
void blit_fb(Bridge& b, SDL_Surface* dst);

// One-line note on whether the drive-A disc is flux and, if so, whether it got
// a writable DSK overlay (Stage 2) or fell back to read-only (non-standard
// flux). Self-gating (silent on a DSK disc) so the caller stays a plain call.
void log_flux_writability(Bridge& b) {
  size_t scp_len = 0;
  if (fdc_media_flux_scp(b.machine.fdc(), scp_len) == nullptr) return;
  size_t img_len = 0;
  const bool writable = fdc_media_image(b.machine.fdc(), img_len) != nullptr;
  LOG_INFO("subcycle engine: drive A flux is "
           << (writable ? "WRITABLE (DSK overlay synthesized)"
                        : "read-only (non-standard flux; not synthesizable)"));
}

}  // namespace

bool subcycle_bridge_start() {
  g_bridge.tier_env_pinned = std::getenv("KONCPC_TIER") != nullptr ||
                             std::getenv("KONCPC_WAKE") != nullptr;
  Bridge& b = g_bridge;
  const int model = static_cast<int>(CPC.model) & 3;
  const std::string rom_file = CPC.rom_path + "/" + chROMFile[model];
  if (model == 3 && pbCartridgeImage != nullptr) {
    // Plus: boot from the PARSED cartridge (the legacy slot loader ran
    // cpr_load, extracting the RIFF/AMS! container into pbCartridgeImage — bank
    // 0 = OS / lower ROM, bank 1 = BASIC / upper ROM). Feeding the raw .cpr
    // file to build() would map the container header as executable code.
    b.rom.assign(pbCartridgeImage.get(), pbCartridgeImage.get() + 0x8000);
  } else {
    b.rom = read_file(rom_file);  // models 0-2: a plain 32K OS+BASIC ROM
  }
  if (!b.machine.build(b.rom.data(), b.rom.size())) {
    LOG_ERROR("subcycle engine: cannot load system ROM "
              << rom_file << " (" << b.rom.size()
              << " bytes) — staying on legacy core");
    return false;
  }
  b.machine.set_overlay(&g_drive_overlay);  // drive sounds (self-gated on cfg)
  if (model == 3 && pbCartridgeImage != nullptr) {
    // Plus: hand the memory device the WHOLE parsed CPR (32 x 16K banks, the
    // unused ones zeroed) so RMR2 / ROM-select can page any bank — not just the
    // flat 0+1 loaded above. Lower ROM boots bank 0 (OS), upper bank 1 (BASIC).
    b.machine.attach_cartridge(pbCartridgeImage.get(),
                               static_cast<size_t>(32) * 0x4000);
  }
  b.amsdos = read_file(CPC.rom_path + "/amsdos.rom");
  b.machine.attach_amsdos(b.amsdos.data(), b.amsdos.size());
  if (!CPC.rom_mf2.empty()) {  // Multiface II (multiface-device.md §6)
    b.mf2rom = read_file(CPC.rom_path + "/" + CPC.rom_mf2);
    if (b.mf2rom.size() >= 0x2000)
      b.machine.attach_mf2_rom(b.mf2rom.data(), b.mf2rom.size());
    else
      b.mf2rom.clear();
  }
  if (g_m4board.enabled && !b.m4_loaded) {  // m4-device.md §2
    b.m4rom = read_file(CPC.resources_path + "/m4.rom");
    if (b.m4rom.size() < 0x4000) b.m4rom = read_file(CPC.rom_path + "/m4.rom");
    if (b.m4rom.size() >= 0x4000) {
      b.machine.set_m4_slot(g_m4board.rom_slot);
      b.machine.attach_m4_rom(b.m4rom.data(), b.m4rom.size());
      b.machine.attach_rom(g_m4board.rom_slot, b.m4rom.data());  // the ROM body
      b.machine.set_m4(true);
    } else {
      b.m4rom.clear();
    }
    b.m4_loaded = true;
  }
  {  // SI card serial BIOS ROM (rs232-device.md): the plotter/serial chain's
     // firmware — the serial RSX + AUX routing. engine=1 must page it in
     // itself; the legacy SIRomManager only fills the engine=0 rom_map. Attach
     // into the SI card's slot (DEFAULT_SLOT=2) so the firmware's boot ROM scan
     // finds and initialises it. Gated like the Device pair (enabled + Plotter
     // backend) so ROM and DART stay consistent; only Faithful-tier, never in
     // the bench.
    const SerialConfig sc = g_serial_interface.get_config();
    if (sc.enabled && sc.backend_type == SerialBackendType::Plotter) {
      b.serialrom = read_file(CPC.rom_path + "/serial.rom");
      if (b.serialrom.size() >= 0x4000)
        b.machine.attach_rom(g_si_rom.get_slot(), b.serialrom.data());
      else
        b.serialrom.clear();
    }
  }
  if (g_silicon_disc.enabled) {         // silicon-disc-device.md
    silicon_disc_init(g_silicon_disc);  // idempotent: allocate the 256K buffer
    b.machine.enable_silicon_disc(true);
    if (g_silicon_disc.data != nullptr)
      b.machine.silicon_disc_load(g_silicon_disc.data, SILICON_DISC_SIZE);
  }
  if (g_symbiface.enabled && !b.sf2_ide_loaded) {  // symbiface-device.md §2
    const char* const keys[2] = {g_symbiface.ide_master.image_path.c_str(),
                                 g_symbiface.ide_slave.image_path.c_str()};
    for (int d = 0; d < 2; ++d) {
      if (keys[d][0] == '\0') continue;
      b.ide_img[d] = read_file(keys[d]);
      if (b.ide_img[d].size() >= 512) {
        b.ide_path[d] = keys[d];
        b.machine.symbiface_attach_ide(d, b.ide_img[d].data(),
                                       b.ide_img[d].size());
      } else {
        b.ide_img[d].clear();
      }
    }
    b.sf2_ide_loaded = true;
  }

  b.fb.assign(static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3,
              0);
  b.machine.attach_framebuffer(b.fb.data(), subcycle::kFbWidth,
                               subcycle::kFbHeight);
  b.fbsurf = SDL_CreateSurfaceFrom(subcycle::kFbWidth, subcycle::kFbHeight,
                                   SDL_PIXELFORMAT_RGB24, b.fb.data(),
                                   subcycle::kFbWidth * 3);
  if (b.fbsurf == nullptr)
    LOG_ERROR("subcycle engine: SDL_CreateSurfaceFrom: " << SDL_GetError());

  if (!CPC.driveA.file.empty()) {
    // Every flux container (.ipf/.raw/.scp/.hfe/.a2r) goes through the unified
    // content-sniffing dispatcher (flux::to_scp) into an in-memory SCP capture
    // so the sub-cycle FDC plays it as flux, weak bits included. .dsk keeps the
    // legacy sector path. Flux is drive-A-only (fdc.cpp sel_media). For a
    // CAPS-encoder IPF, to_scp mirrors the legacy driveA the slot loader
    // already ipf_load-ed (loadSlots ran before subcycle_bridge_start).
    std::string ext;  // the INNER extension when the slot file is a .zip
    std::vector<uint8_t> raw = read_media_file(CPC.driveA.file, ext);
    const bool flux = is_flux_ext(ext);
    if (flux) {
      b.media = flux::to_scp(raw.data(), raw.size(), ext);
    } else {
      b.media = std::move(raw);
    }
    const bool ok =
        !b.media.empty() &&
        (flux ? b.machine.insert_flux(b.media.data(), b.media.size())
              : b.machine.insert_disk(b.media.data(), b.media.size()));
    if (ok) {
      LOG_INFO("subcycle engine: drive A <- " << CPC.driveA.file);
      log_flux_writability(b);  // self-gating: silent unless drive A is flux
    } else {
      LOG_ERROR("subcycle engine: cannot attach " << CPC.driveA.file);
    }
  }

  // Drive B (unit 1): DSK only — the CPC's second drive has no flux path, and
  // the FDC's flux capture is drive-A-only (fdc.cpp sel_media). Loaded at
  // engine build when the app already routed a second image to CPC.driveB
  // (two-disk CLI, config slot_b). Runtime hot-swap into B is a separate
  // follow-up (PendingMedia carries no unit yet — beads-xsdc).
  if (!CPC.driveB.file.empty() && !ends_with(CPC.driveB.file, ".scp")) {
    std::string ext_b;
    b.media_b = read_media_file(CPC.driveB.file, ext_b);
    if (!b.media_b.empty() &&
        b.machine.insert_disk(b.media_b.data(), b.media_b.size(), 1)) {
      LOG_INFO("subcycle engine: drive B <- " << CPC.driveB.file);
    } else {
      LOG_ERROR("subcycle engine: cannot attach " << CPC.driveB.file
                                                  << " to drive B");
    }
  }

  // Firmware-vector taps: re-register the app's already-installed console
  // hooks (telnet TXT_OUTPUT / CP/M BDOS) as machine taps — registers still
  // hold the caller's values at the fetch edge, so A / C+E read exactly as
  // the legacy per-instruction hook saw them.
  uint16_t txt_addr = 0;
  if (TxtOutputHook txt_hook = z80_get_txt_output_hook(&txt_addr)) {
    b.machine.add_tap(
        txt_addr,
        [](void* ctx, uint16_t) {
          subcycle::Machine const* mach = subcycle_bridge_machine();
          if (mach == nullptr) return;
          const Z80Regs regs = mach->regs();
          reinterpret_cast<TxtOutputHook>(ctx)(
              static_cast<uint8_t>(regs.af >> 8));  // A = the character
        },
        reinterpret_cast<void*>(txt_hook));
  }
  if (TxtOutputHook bdos_hook = z80_get_bdos_output_hook()) {
    b.machine.add_tap(
        0x0005,
        [](void* ctx, uint16_t) {
          subcycle::Machine const* mach = subcycle_bridge_machine();
          if (mach == nullptr) return;
          const Z80Regs regs = mach->regs();
          if ((regs.bc & 0xFF) == 2)  // C_WRITE: E = the character
            reinterpret_cast<TxtOutputHook>(ctx)(
                static_cast<uint8_t>(regs.de & 0xFF));
        },
        reinterpret_cast<void*>(bdos_hook));
  }

  b.next_deadline = 0;
  b.active = true;
  LOG_INFO("subcycle engine: running the pin-level board ("
           << rom_file << ", model " << model << ")");
  return true;
}

bool subcycle_bridge_active() { return g_bridge.active; }

uint16_t subcycle_bridge_scanned_key_rows() {
  return g_bridge.active ? g_bridge.machine.take_key_scanned_rows() : 0;
}

void subcycle_bridge_disk_leds(bool& drive_a, bool& drive_b) {
  drive_a = drive_b = false;
  if (!g_bridge.active) return;
  FdcRegs r{};
  fdc_peek(g_bridge.machine.fdc(), &r);
  // Match the legacy FDC.led activity light (engine=0, src/fdc.cpp): lit ONLY
  // while a data-transfer command is in flight (from dispatch until its result
  // phase drains — phase leaves then returns to CMD), for the selected unit.
  // NOT the raw motor latch (the firmware holds the motor on for seconds after
  // access — that read as on far too long), and NOT seeks/recalibrates (no
  // transfer). The µPD765A opcode is the low 5 bits; MT/MF/SK are modifiers.
  auto is_transfer = [](uint8_t cmd) {
    switch (cmd & 0x1F) {
      case 0x02:  // READ TRACK
      case 0x05:  // WRITE DATA
      case 0x06:  // READ DATA
      case 0x09:  // WRITE DELETED DATA
      case 0x0A:  // READ ID
      case 0x0C:  // READ DELETED DATA
      case 0x0D:  // FORMAT TRACK
      case 0x11:  // SCAN EQUAL
      case 0x19:  // SCAN LOW OR EQUAL
      case 0x1D:  // SCAN HIGH OR EQUAL
        return true;
      default:  // SPECIFY / SENSE / SEEK / RECALIBRATE: no LED
        return false;
    }
  };
  const bool active = r.phase != 0 /* PH_CMD */ && is_transfer(r.last_cmd);
  drive_a = active && r.unit == 0;
  drive_b = active && r.unit == 1;
}

// ── engine=1 instruction trace ───────────────────────────────────────────────
// The subcycle Machine is standalone (it cannot see g_trace, which pulls in the
// legacy z80_view.h accessors). The bridge is the one layer that knows both, so
// the record adapter lives here: Machine fires the hook per retired
// instruction, we translate Z80Regs -> the TraceRecorder's flat register
// signature. The opcode bytes record() reads come back through z80_read_mem,
// which itself shims to the machine's peek_mem under engine=1 — so the
// disassembly is CPU-correct. (The telnet TXT_OUTPUT / BDOS console mirrors do
// NOT ride this hook: they are re-registered as machine fetch-edge taps in
// subcycle_bridge_start — servicing them here too double-prints every mirrored
// character.)
namespace {
void bridge_trace_instr_hook(void* /*ctx*/, const Z80Regs* regs) {
  g_trace.record(regs->pc, static_cast<uint8_t>(regs->af >> 8),
                 static_cast<uint8_t>(regs->af & 0xFF), regs->bc, regs->de,
                 regs->hl, regs->sp);
}
}  // namespace

void subcycle_bridge_set_instr_trace(bool on) {
  g_bridge.machine.set_instr_hook(on ? bridge_trace_instr_hook : nullptr,
                                  nullptr);
}

void subcycle_bridge_set_tier_policy(BridgeTierPolicy policy) {
  g_bridge.tier_policy = policy;
}

BridgeTierPolicy subcycle_bridge_tier_policy() { return g_bridge.tier_policy; }

int subcycle_bridge_tier_env_pinned() {
  return g_bridge.tier_env_pinned ? 1 : 0;
}

const char* subcycle_bridge_effective_tier_name() {
  using RT = subcycle::Machine::RunTier;
  switch (g_bridge.machine.effective_run_tier()) {
    case RT::Fast:
      return "fast";
    case RT::Wake:
      return "wake";
    case RT::Soldered:
      return "soldered";
    case RT::Faithful:
      return "faithful";
  }
  return "?";
}

const char* subcycle_bridge_effective_tier_label() {
  using RT = subcycle::Machine::RunTier;
  switch (g_bridge.machine.effective_run_tier()) {
    case RT::Fast:
      return "Performance";
    case RT::Wake:
      return "Balanced";
    case RT::Soldered:
      return "Microscope";
    case RT::Faithful:
      return "Microscope, socketed chips";
  }
  return "?";
}

void subcycle_bridge_bench_request() {
  if (g_bridge.bench_tier < 0)
    g_bridge.bench_want.store(1, std::memory_order_relaxed);
}

int subcycle_bridge_bench_running() { return g_bridge.bench_tier; }

int subcycle_bridge_bench_fps(int tier) {
  if (tier < 0 || tier > 3) return -1;
  return g_bridge.bench_fps[tier].load(std::memory_order_relaxed);
}

subcycle::Machine* subcycle_bridge_machine() {
  return g_bridge.active ? &g_bridge.machine : nullptr;
}

const Device* subcycle_bridge_fdc() {
  return g_bridge.active ? g_bridge.machine.fdc() : nullptr;
}

void subcycle_bridge_sync_regs_view() {
  if (!g_bridge.active) return;
  const Z80Regs r = g_bridge.machine.regs();
  z80.AF.w.l = r.af;
  z80.BC.w.l = r.bc;
  z80.DE.w.l = r.de;
  z80.HL.w.l = r.hl;
  z80.AFx.w.l = r.af_;
  z80.BCx.w.l = r.bc_;
  z80.DEx.w.l = r.de_;
  z80.HLx.w.l = r.hl_;
  z80.IX.w.l = r.ix;
  z80.IY.w.l = r.iy;
  z80.SP.w.l = r.sp;
  z80.PC.w.l = r.pc;
  z80.I = r.i;
  z80.R = r.r;
  z80.IM = r.im;
  z80.IFF1 = r.iff1;
  z80.IFF2 = r.iff2;
  z80.HALT = r.halted;
  g_tstate_counter = r.tstates;  // the DevTools T-state readout
}

void subcycle_bridge_regs_to_machine() {
  if (!g_bridge.active) return;
  Z80Regs r = g_bridge.machine.regs();
  r.af = z80.AF.w.l;
  r.bc = z80.BC.w.l;
  r.de = z80.DE.w.l;
  r.hl = z80.HL.w.l;
  r.af_ = z80.AFx.w.l;
  r.bc_ = z80.BCx.w.l;
  r.de_ = z80.DEx.w.l;
  r.hl_ = z80.HLx.w.l;
  r.ix = z80.IX.w.l;
  r.iy = z80.IY.w.l;
  r.sp = z80.SP.w.l;
  r.pc = z80.PC.w.l;
  r.i = z80.I;
  r.r = z80.R;
  r.im = z80.IM;
  r.iff1 = z80.IFF1;
  r.iff2 = z80.IFF2;
  g_bridge.machine.set_regs(r);
}

namespace {

uint8_t to_bcd(int v) {
  return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

void serial_host_tx_byte(uint8_t byte, void* ctx) {
  auto* backend = static_cast<SerialBackend*>(ctx);
  if (backend) backend->send(byte);
}

void sync_serial_backend(Bridge& b) {
  const SerialConfig sc = g_serial_interface.get_config();
  if (!sc.enabled || sc.backend_type == SerialBackendType::Plotter) return;
  if (g_serial_interface.backend == nullptr) return;
  while (g_serial_interface.backend->has_data())
    b.machine.serial_host_rx(g_serial_interface.backend->recv());
}

// Enable/plugged flags mirrored from the legacy UI toggles each frame, so the
// existing checkboxes drive the sub-cycle Devices unchanged.
void sync_peripheral_flags(Bridge& b) {
  b.machine.set_digiblaster(CPC.snd_pp_device != 0);  // printer-device.md §3
  // Printer /BUSY strap (PPI Port B bit 6): READY while a virtual printer is
  // attached ([printer] config), busy like an unconnected Centronics port
  // otherwise. Without this the firmware's print-wait loop (MC WAIT PRINTER)
  // spins forever — the e2e dsk hang.
  ppi_set_printer_ready(b.machine.ppi(), CPC.printer ? 1 : 0);
  // Printer capture parity: drain the strobe-clocked bytes into the legacy
  // capture file (pfoPrinter, opened by the host when [printer] is on). The
  // Digiblaster DAC is independent — the machine mixes the latch internally.
  if (pfoPrinter != nullptr) {
    PrinterEvent ev[64];
    int n = 0;
    while ((n = printer_drain_events(b.machine.printer(), ev, 64)) > 0)
      for (int i = 0; i < n; ++i) fputc(ev[i].byte, pfoPrinter);
  }
  b.machine.set_amdrum(g_amdrum.enabled);
  b.machine.set_mf2(CPC.mf2 != 0 && !b.mf2rom.empty());
  b.machine.set_amx_mouse(g_amx_mouse.enabled);
  b.machine.set_smartwatch(g_smartwatch.enabled);
  b.machine.set_symbiface(g_symbiface.enabled);
  b.machine.set_asic(CPC.model == 3);  // asic-device.md: live on the 6128+
  // The serial pair (rs232-device.md, plotter-device.md): plotter backend uses
  // the bit-serial plotter Device; other backends use rs232 + host bridge.
  {
    const SerialConfig sc = g_serial_interface.get_config();
    if (sc.enabled && sc.backend_type == SerialBackendType::Plotter) {
      b.machine.set_serial_plotter(true, sc.baud_rate);
      // Drop any host_tx left from a non-plotter backend: apply_config()
      // deletes that backend object, so a stale ctx here is a use-after-free
      // on the next DART data write.
      b.machine.set_serial_host_tx(nullptr, nullptr);
    } else {
      b.machine.set_serial_plotter(false, 0);
      b.machine.set_serial_card(sc.enabled);
      if (sc.enabled && g_serial_interface.backend) {
        b.machine.set_serial_host_tx(serial_host_tx_byte,
                                     g_serial_interface.backend);
      } else {
        b.machine.set_serial_host_tx(nullptr, nullptr);
      }
    }
  }
  // The light gun (light-gun-device.md §4): map the host aim (framebuffer
  // pixels in CPC.phazer_x/y) into the CRTC beam space the gun matches.
  // Standard-timing linear approximation — the fb is kVisChars=48 columns of
  // CHARW=16 px (subcycle::kFbWidth/48) and kFbHeight rows; the active display
  // sits inside a ~4-char / ~kTopBorder-line border. The ±2 tolerance window
  // plus real-gun fuzziness absorbs the slop (exactness is neither achievable
  // nor wanted). Only fed under engine=1; the legacy phazer.cpp path serves
  // engine=0 until the core deletion.
  {
    constexpr int kCharW = subcycle::kFbWidth / 48;  // 16 px per char column
    constexpr int kActiveLeftChars = 4;  // centered 40-col display border
    constexpr int kTopBorderLines = 34;  // frame top → visible-window top
    int col = (static_cast<int>(CPC.phazer_x) / kCharW) - kActiveLeftChars;
    col = std::max(col, 0);
    const int line = static_cast<int>(CPC.phazer_y) + kTopBorderLines;
    const int gun_type = static_cast<PhazerType::Value>(CPC.phazer_emulation);
    b.machine.set_light_gun(gun_type, static_cast<uint16_t>(line),
                            static_cast<uint16_t>(col), CPC.phazer_pressed);
  }
}

// Execute a latched M4 command host-side and hand the result back (m4-device.md
// §3): the unchanged m4board_execute() dispatch fills g_m4board.response[].
void sync_m4_command(Bridge& b) {
  if (!g_m4board.enabled) return;
  M4Pending pc;
  if (!b.machine.m4_pending(&pc)) return;
  g_m4board.cmd_buf.assign(pc.frame, pc.frame + pc.len);
  m4board_execute();
  b.machine.m4_respond(g_m4board.response,
                       static_cast<uint16_t>(g_m4board.response_len));
  b.machine.m4_config(g_m4board.config_buf, M4Board::CONFIG_SIZE);
}

// Symbiface DS12887: feed the host clock, and drain the legacy PS/2 FIFO (SDL +
// IPC filled it) packet-for-packet into the Device (symbiface-device.md §3/§4).
void sync_symbiface(Bridge& b) {
  if (!g_symbiface.enabled) return;
  const time_t now_t = time(nullptr);
  const struct tm* lt = localtime(&now_t);
  const uint8_t regs[10] = {
      to_bcd(lt->tm_sec),
      0,
      to_bcd(lt->tm_min),
      0,
      to_bcd(lt->tm_hour),
      0,
      static_cast<uint8_t>(lt->tm_wday == 0 ? 7 : lt->tm_wday),
      to_bcd(lt->tm_mday),
      to_bcd(lt->tm_mon + 1),
      to_bcd(lt->tm_year % 100)};
  b.machine.symbiface_rtc_time(regs);
  auto& mo = g_symbiface.mouse;
  while (mo.tail != mo.head) {
    b.machine.symbiface_mouse_packet(mo.fifo[mo.tail]);
    mo.tail = (mo.tail + 1) % SF2_Mouse::FIFO_SIZE;
  }
}

// Refresh the DS1216 SmartWatch clock from the host (smartwatch-device.md §4).
void sync_smartwatch(Bridge& b) {
  if (!g_smartwatch.enabled) return;
  const time_t now_t = time(nullptr);
  const struct tm* lt = localtime(&now_t);
  const uint8_t t8[8] = {
      0x00,
      to_bcd(lt->tm_sec),
      to_bcd(lt->tm_min),
      static_cast<uint8_t>(0x80 | to_bcd(lt->tm_hour)),
      static_cast<uint8_t>(lt->tm_wday == 0 ? 7 : lt->tm_wday),
      to_bcd(lt->tm_mday),
      to_bcd(lt->tm_mon + 1),
      to_bcd(lt->tm_year % 100)};
  b.machine.set_smartwatch_time(t8);
}

// Move the SDL/IPC-accumulated whole mickeys into the AMX Device, then zero the
// legacy accumulator (amx-mouse-device.md §3).
void sync_amx_mouse(Bridge& b) {
  if (!g_amx_mouse.enabled) return;
  const int mx = g_amx_mouse.mickey_x, my = g_amx_mouse.mickey_y;
  g_amx_mouse.mickey_x -= mx;
  g_amx_mouse.mickey_y -= my;
  b.machine.amx_mouse_feed(mx, my, g_amx_mouse.buttons);
}

}  // namespace

void subcycle_bridge_sync_probe() {
  // Bench mirror: the legacy lists are the UI's editing model until Wave 1
  // deletes them; the probe is the truth that actually fires. Runs both
  // AFTER each frame (debug_sync) and BEFORE each frame (the emulation
  // loop): a list edit made while paused at a hit must reach the probe
  // before the resumed frame runs, or a just-cleared breakpoint re-fires
  // off the stale mirror within milliseconds and re-pauses — the resume
  // livelock of beads-4gf9.
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  Bridge& b = g_bridge;
  if (!b.active) return;
  const Device* pr = b.machine.probe();
  probe_clear_exec(pr);
  for (const auto& bp : z80_list_breakpoints_ref())
    probe_add_exec(pr, static_cast<uint16_t>(bp.address));
  // Legacy old-flavour single breakpoint (z80.break_point — the main loop
  // keeps it re-armed at 0): mirror it into the probe ONLY while an autotype
  // KONCPC_WAITBREAK is in flight (queued or blocking). That covers a
  // `call 0` executing before the queue reaches its WAITBREAK (the hit is
  // then latched for it), while a plain run never pays the debug-engaged
  // tier drop for the always-rearmed break-at-0.
  const bool waitbreak_armed =
      autotype_waitbreak_in_flight() && z80.break_point != Z80_BREAKPOINT_NONE;
  if (waitbreak_armed)
    probe_add_exec(pr, static_cast<uint16_t>(z80.break_point));
  probe_clear_watch(pr);
  for (const auto& wp : z80_list_watchpoints_ref()) {
    const uint16_t len = wp.length ? wp.length : 1;
    probe_add_watch(pr, static_cast<uint16_t>(wp.address), len,
                    (wp.type & READ) ? 1 : 0, (wp.type & WRITE) ? 1 : 0);
  }
  probe_clear_io(pr);
  for (const auto& io : z80_list_io_breakpoints_ref())
    probe_add_io(pr, io.port, io.mask, (io.dir & IO_IN) ? 1 : 0,
                 (io.dir & IO_OUT) ? 1 : 0);
  b.debug_engaged = waitbreak_armed || !z80_list_breakpoints_ref().empty() ||
                    !z80_list_watchpoints_ref().empty() ||
                    !z80_list_io_breakpoints_ref().empty();
}

void subcycle_bridge_request_tape_seek(uint32_t block_ordinal) {
  g_bridge.tape_seek_req.store(block_ordinal, std::memory_order_relaxed);
}

int subcycle_bridge_debug_sync() {
  Bridge& b = g_bridge;
  // Media truth for the UI (fdc-device.md §10): the legacy save-on-eject and
  // "disc altered" indicators read driveA.altered.
  driveA.altered = b.machine.disk_dirty();
  driveB.altered = fdc_media_dirty_unit(b.machine.fdc(), 1) != 0;
  // Each peripheral's once-per-frame host<->Device sync, named (each inlines
  // at -O2). Order is independent — they touch disjoint Devices.
  sync_peripheral_flags(b);  // enable/plugged flags from the legacy UI toggles
  sync_serial_backend(b);    // file/TCP/tty bytes ↔ rs232 Device
  sync_m4_command(b);        // execute a latched M4 command host-side (§3)
  sync_symbiface(b);         // DS12887 clock + PS/2 FIFO drain
  sync_smartwatch(b);        // DS1216 clock refresh
  sync_amx_mouse(b);         // drain the mickey accumulator into the Device
  if (!b.active) return 0;

  subcycle_bridge_sync_probe();

  subcycle_bridge_sync_regs_view();

  // The deck's PLAY button follows the app's tape UI (F4 / menus / IPC).
  b.machine.tape_play_button(CPC.tape_play_button != 0);

  // ── Tape: mirror the sub-cycle deck's live wires back to the legacy host
  // state the tape UI/SFX read. Engine=1 bypasses the z80_OUT_handler +
  // tape.cpp chain that used to drive these, so the status-bar scope
  // (bTapeLevel, gated on CPC.tape_motor), the procedural hiss
  // (drive_sounds_tape), and the audible data signal all went dark. This is the
  // tape counterpart to DriveSoundOverlay (which does the same for the FDC).
  // Nothing here touches the AY — the SFX and the data monitor each render on
  // their own SDL stream.
  {
    const bool play = CPC.tape_play_button != 0;
    const bool motor = b.machine.tape_motor();
    CPC.tape_motor =
        motor ? 0x10 : 0;  // matches legacy (val & 0x10); un-freezes
                           // the scope's sample gate (kon_cpc_ja.cpp)
    bTapeLevel = b.machine.tape_read_level() ? TAPE_LEVEL_HIGH : TAPE_LEVEL_LOW;

    // Mechanics (procedural hiss): edge-fire on the same motor-AND-play
    // condition the legacy io_fire_tape_motor_hooks used. drive_sounds_tape
    // self-gates on the [sound] tape_sounds flag, and renders on the
    // independent SFX stream.
    const bool running = motor && play;
    if (running != b.prev_tape_running) {
      drive_sounds_tape(running);
      b.prev_tape_running = running;
    }

    // Data signal (audible screech): auto-route the cassette RDATA wire to its
    // own playback stream (tape_line_out — never the AY) while the deck plays.
    // Armed once on Play, disarmed on Stop; we only ever touch what WE armed,
    // so a manual IPC `tape` arm is left untouched.
    if (play && !tape_line_out_active()) {
      if (tape_line_out_arm(b.machine, /*data_channel=*/0,
                            /*source_rdata=*/true))
        b.tape_lineout_auto = true;
    } else if (!play && b.tape_lineout_auto) {
      tape_line_out_disarm(b.machine);
      b.tape_lineout_auto = false;
    }
  }

  // Deferred tape block-seek from the UI (Prev/Next): apply on this thread at
  // the frame boundary so the live deck is never repositioned mid-tick.
  {
    const uint32_t seek =
        b.tape_seek_req.exchange(~uint32_t{0}, std::memory_order_relaxed);
    if (seek != ~uint32_t{0}) b.machine.tape_seek(seek);
  }

  // Decoded-bits scope (BITS mode): drain the deck's decoded data bits into the
  // UI ring. The sub-cycle deck knows each bit (hw/tape.cpp pulse_done); the
  // legacy tape.cpp path that used to fill tape_decoded_buf never runs here.
  {
    uint8_t bits[256];
    const int nbits =
        b.machine.tape_drain_bits(bits, static_cast<int>(sizeof(bits)));
    for (int i = 0; i < nbits; ++i) {
      imgui_state.tape_decoded_buf[imgui_state.tape_decoded_head] = bits[i];
      imgui_state.tape_decoded_head = (imgui_state.tape_decoded_head + 1) %
                                      ImGuiUIState::TAPE_DECODED_SAMPLES;
    }
  }

  // Current-block counter: the legacy pbTapeBlock the tape UI tracks never
  // advances under engine=1 (the sub-cycle deck owns the transport), so the
  // "n/M" counter froze at block 0 — which grayed out Prev and made Prev/Next
  // seek relative to the wrong block. Drive the index from the deck's own
  // ordinal so it follows playback and the buttons enable/seek correctly. Both
  // the deck and tape_scan_blocks count CDT blocks in the same order, so the
  // ordinal indexes the host offsets table directly (clamped for safety).
  if (!imgui_state.tape_block_offsets.empty()) {
    TapeRegs tr{};
    tape_peek(b.machine.tape(), &tr);
    if (tr.attached) {
      const int nblk = static_cast<int>(imgui_state.tape_block_offsets.size());
      imgui_state.tape_current_block = static_cast<int>(tr.block) >= nblk
                                           ? nblk - 1
                                           : static_cast<int>(tr.block);
    }
  }

  // Chip-state views: CRTC / Gate Array / PSG windows (and the topbar mode
  // readout) read these legacy structs; publish the Devices' pin-level truth.
  CrtcRegs cr{};
  crtc_peek(b.machine.crtc(), &cr);
  std::memcpy(CRTC.registers, cr.reg, sizeof(cr.reg));
  CRTC.reg_select = cr.reg_select;
  CRTC.crtc_type = cr.type;
  // The deep 6845 counters the IPC chip-state query prints (Wave-1 peek
  // cutover): every one is real chip state, straight off the Device.
  CRTC.char_count = cr.hcc;
  CRTC.line_count = cr.vcc;
  CRTC.raster_count = cr.ra;
  CRTC.hsw_count = cr.hsw;
  CRTC.vsw_count = cr.vsw;
  CRTC.addr = cr.ma;
  CRTC.reg5 = cr.vta;
  CRTC.sl_count = static_cast<unsigned char>(cr.scanline);
  // Plus PRI scanline (asic_debug reads CRTC.interrupt_sl; 0 = PRI off,
  // matching the legacy semantics and the classic machines).
  {
    AsicRegs ar{};
    asic_peek(b.machine.asic(), &ar);
    CRTC.interrupt_sl = ar.pri_line;
  }
  // Drive mechanics for the status surfaces (drive LED / IPC drive query):
  // the physical head position per unit and the motor latch, off the FDC.
  {
    FdcRegs fr{};
    fdc_peek(b.machine.fdc(), &fr);
    driveA.current_track = fr.track[0];
    driveB.current_track = fr.track[1];
    FDC.motor = fr.motor;
  }
  GateArrayRegs ga{};
  ga_peek(b.machine.gate_array(), &ga);
  GateArray.pen = ga.pen;
  std::memcpy(GateArray.ink_values, ga.ink, sizeof(ga.ink));
  GateArray.scr_mode = ga.mode;
  GateArray.requested_scr_mode = ga.req_mode;
  GateArray.ROM_config = ga.rom_config;
  GateArray.RAM_config = ga.ram_config;
  GateArray.sl_count = ga.sl_count;
  GateArray.hs_count = ga.hs_count;
  GateArray.RAM_bank = ga.ram_config & 7;  // the banking field of &7Fxx fn 3
  {
    MemRegs mr{};
    mem_peek(b.machine.mem(), &mr);
    GateArray.upper_ROM = mr.rom_select;  // the &DFxx upper-ROM latch
  }
  PsgRegs ps{};
  psg_peek(b.machine.psg(), &ps);
  std::memcpy(PSG.RegisterAY.Index, ps.reg, sizeof(ps.reg));
  PSG.reg_select = ps.sel;

  ProbeHit hit{};
  if (b.machine.probe_hit(&hit)) {
    if (hit.kind == PROBE_HIT_EXEC && hit.addr == z80.break_point) {
      // The old-flavour single breakpoint (z80.break_point, mirrored into
      // the probe only while a KONCPC_WAITBREAK is in flight): report
      // EC_BREAKPOINT WITHOUT breakpoint_reached, so the main loop takes its
      // legacy else-branch — clear break_point, keep running, release/latch
      // the WAITBREAK. Checked BEFORE should_break: that predicate returns
      // true on an EMPTY list (the step path arms the probe without a list),
      // which would misroute this hit into the pause path. A listed
      // breakpoint at the same address still wins.
      bool listed = false;
      for (const auto& bp : z80_list_breakpoints_ref())
        if (bp.address == hit.addr) {
          listed = true;
          break;
        }
      if (!listed) {
        b.machine.probe_resume();
        return 1;
      }
    }
    if (hit.kind == PROBE_HIT_EXEC && !z80_probe_exec_should_break(hit.addr)) {
      b.machine.probe_resume();
      return 0;
    }
    if (hit.kind == PROBE_HIT_MEM_READ || hit.kind == PROBE_HIT_MEM_WRITE) {
      const bool is_write = hit.kind == PROBE_HIT_MEM_WRITE;
      // Pre-access byte: the mem Device commits a CPU write on the NEXT
      // master cycle (one-tick write latch), and run_frame parks on the tick
      // the probe latched — peek_mem still reads the pre-write value here.
      const uint8_t old_val = b.machine.peek_mem(hit.addr);
      if (!z80_probe_watch_should_break(hit.addr, hit.data, is_write,
                                        old_val)) {
        b.machine.probe_resume();
        return 0;
      }
      z80.watchpoint_old = old_val;
    }
    b.machine
        .probe_resume();  // edge consumed: resume continues mid-instruction
    if (hit.kind == PROBE_HIT_EXEC) {
      z80.breakpoint_reached = 1;
      z80.PC.w.l =
          hit.addr;  // the halted instruction's identity (spec: probe §3)
    } else {
      z80.watchpoint_reached = 1;
      z80.watchpoint_addr = hit.addr;
      z80.watchpoint_value = hit.data;
    }
    z80_remove_ephemeral_breakpoints();
    z80_call_breakpoint_hit_hook(static_cast<word>(hit.addr),
                                 hit.kind != PROBE_HIT_EXEC);
    return 1;
  }
  return 0;
}

void subcycle_bridge_reset() {
  if (g_bridge.active) g_bridge.machine.reset();
}

namespace {
// defined with the media swap machinery
void flush_dirty_media_unit(Bridge& b, uint8_t unit);
void flush_sf2_ide(Bridge& b);
}  // namespace

namespace {
void flush_sf2_ide(Bridge& b) {
  if (!b.machine.symbiface_dirty()) return;  // symbiface-device.md §2
  for (int d = 0; d < 2; ++d) {
    if (b.ide_path[d].empty() || b.ide_img[d].empty()) continue;
    FILE* f = fopen(b.ide_path[d].c_str(), "wb");
    if (f == nullptr) {
      LOG_ERROR("subcycle engine: cannot write back IDE image "
                << b.ide_path[d]);
      continue;
    }
    const size_t n = fwrite(b.ide_img[d].data(), 1, b.ide_img[d].size(), f);
    if (fclose(f) != 0 || n != b.ide_img[d].size())
      LOG_ERROR("subcycle engine: short write saving IDE " << b.ide_path[d]);
  }
  b.machine.symbiface_mark_clean();
}
}  // namespace

void subcycle_bridge_stop() {
  Bridge& b = g_bridge;
  flush_dirty_media_unit(b, 0);  // shutdown: both discs keep their writes
  flush_dirty_media_unit(b, 1);
  flush_sf2_ide(b);  // and the IDE images keep theirs
  if (g_silicon_disc.enabled && g_silicon_disc.data != nullptr)
    b.machine.silicon_disc_save(g_silicon_disc.data, SILICON_DISC_SIZE);
  if (b.fbsurf != nullptr) {
    SDL_DestroySurface(b.fbsurf);
    b.fbsurf = nullptr;
  }
  if (b.fbconv != nullptr) {
    SDL_DestroySurface(b.fbconv);
    b.fbconv = nullptr;
  }
  b.active = false;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
void subcycle_bridge_insert_media(std::vector<uint8_t> bytes, bool flux,
                                  uint8_t unit) {
  Bridge& b = g_bridge;
  if (!b.active || bytes.empty()) return;
  std::scoped_lock const lock(b.swap_mutex);
  b.swap_bytes = std::move(bytes);
  b.swap_unit.store(unit & 1, std::memory_order_release);
  b.swap_kind.store(flux ? PendingMedia::kFlux : PendingMedia::kDisk,
                    std::memory_order_release);
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
void subcycle_bridge_insert_tape(std::vector<uint8_t> bytes) {
  Bridge& b = g_bridge;
  if (!b.active || bytes.empty()) return;
  std::scoped_lock const lock(b.swap_mutex);
  b.swap_bytes = std::move(bytes);
  b.swap_kind.store(PendingMedia::kTape, std::memory_order_release);
}

void subcycle_bridge_refresh_asic_view() {
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  Bridge& b = g_bridge;
  if (!b.active) return;
  const Device* ad = b.machine.asic();
  AsicRegs ar{};
  asic_peek(ad, &ar);
  asic.locked = ar.locked != 0;
  asic.lockSeqPos = asic_lock_pos(ad);
  asic.hscroll = ar.hscroll;
  asic.vscroll = ar.vscroll;
  asic.extend_border = ar.extend_border != 0;
  asic.interrupt_vector = ar.int_vector;
  asic.raster_interrupt = ar.pri_line != 0;
  for (int i = 0; i < 16; ++i) {
    uint16_t x = 0, y = 0;
    uint8_t mx = 0, my = 0;
    asic_vid_sprite_attr(ad, i, &x, &y, &mx, &my);
    asic.sprites_x[i] = static_cast<int16_t>(x);
    asic.sprites_y[i] = static_cast<int16_t>(y);
    asic.sprites_mag_x[i] = mx;
    asic.sprites_mag_y[i] = my;
    for (int sx = 0; sx < 16; ++sx)
      for (int sy = 0; sy < 16; ++sy) {
        const uint8_t pix = asic_vid_sprite_pixel(ad, i, sx, sy);
        // View convention (asic_debug): palette index + 16, 0 = transparent.
        asic.sprites[i][sx][sy] = pix ? static_cast<byte>(pix + 16) : 0;
      }
  }
  for (int c = 0; c < 3; ++c) {
    uint16_t src = 0, loop = 0, loops = 0, pause = 0, ticks = 0;
    uint8_t pre = 0, en = 0, irq = 0;
    asic_dma_regs(ad, c, &src, &pre, &en);
    asic_dma_debug(ad, c, &loop, &loops, &pause, &ticks, &irq);
    asic.dma.ch[c].source_address = src;
    asic.dma.ch[c].loop_address = loop;
    asic.dma.ch[c].prescaler = pre;
    asic.dma.ch[c].enabled = en != 0;
    asic.dma.ch[c].interrupt = irq != 0;
    asic.dma.ch[c].pause_ticks = pause;
    asic.dma.ch[c].tick_cycles = static_cast<byte>(ticks);
    asic.dma.ch[c].loops = loops;
  }
  // Register-page snapshot (asic_debug's palette dump reads it).
  for (int off = 0; off < 0x4000; ++off)
    pbRegisterPage[off] = asic_page_peek(ad, static_cast<uint16_t>(off));
}

void subcycle_bridge_eject_tape() {
  Bridge& b = g_bridge;
  if (!b.active) return;
  std::scoped_lock const lock(b.swap_mutex);
  b.swap_bytes.clear();
  b.swap_kind.store(PendingMedia::kTapeEject, std::memory_order_release);
}

void subcycle_bridge_mf2_stop() {
  g_bridge.mf2_stop.store(true, std::memory_order_release);
}

void subcycle_bridge_eject_media(uint8_t unit) {
  Bridge& b = g_bridge;
  if (!b.active) return;
  std::scoped_lock const lock(b.swap_mutex);
  b.swap_bytes.clear();
  b.swap_unit.store(unit & 1, std::memory_order_release);
  b.swap_kind.store(PendingMedia::kEject, std::memory_order_release);
}

namespace {

// Z80 thread, frame boundary: apply a pending hot-swap (see header).
// Persistence (fdc-device.md §10): the machine mutates the DSK buffer in
// place; when it diverged, write it back to its file — a real CPC's writes
// land on the real disc. Called before the buffer is replaced or dropped.

// Shared write-back core for drive A and drive B.
void flush_dirty_media_unit(Bridge& b, uint8_t unit) {
  std::vector<uint8_t>& buf = unit == 0 ? b.media : b.media_b;
  const std::string& path = unit == 0 ? CPC.driveA.file : CPC.driveB.file;
  bool& altered = unit == 0 ? driveA.altered : driveB.altered;
  const char* label = unit == 0 ? "drive A" : "drive B";
  if (fdc_media_dirty_unit(b.machine.fdc(), unit) == 0) return;
  if (buf.empty() || path.empty()) return;
  // A dirty writable-flux disc (drive A) must be re-encoded to a flux container
  // (scp_from_disk over the dirty map) before write-back — the format-picker UI
  // is Stage 4. `buf` here is the pristine SCP, so writing it back would
  // clobber the file with the *unmodified* capture; skip until export lands.
  if (unit == 0) {
    size_t scp_len = 0;
    if (fdc_media_flux_scp(b.machine.fdc(), scp_len) != nullptr) {
      LOG_INFO(
          "subcycle engine: drive A flux write-back deferred to export "
          "(Stage 4) — writes are live in the overlay");
      return;
    }
  }
  FILE* f = fopen(path.c_str(), "wb");
  if (f == nullptr) {
    LOG_ERROR("subcycle engine: cannot write back " << label << " DSK to "
                                                    << path);
    return;
  }
  const size_t n = fwrite(buf.data(), 1, buf.size(), f);
  if (fclose(f) != 0 || n != buf.size()) {
    LOG_ERROR("subcycle engine: short write saving " << label << " DSK to "
                                                     << path);
    return;
  }
  fdc_media_mark_clean_unit(b.machine.fdc(), unit);
  altered = false;
  LOG_INFO("subcycle engine: " << label << " DSK written back to " << path);
}

void apply_pending_media(Bridge& b) {
  if (b.swap_kind.load(std::memory_order_acquire) == PendingMedia::kNone)
    return;
  std::scoped_lock const lock(b.swap_mutex);
  const PendingMedia kind = b.swap_kind.exchange(PendingMedia::kNone);
  const uint8_t unit = b.swap_unit.exchange(0, std::memory_order_acq_rel);
  const char* drive = unit == 0 ? "A" : "B";
  std::vector<uint8_t>& buf = unit == 0 ? b.media : b.media_b;
  switch (kind) {
    case PendingMedia::kDisk:
    case PendingMedia::kFlux: {
      if (kind == PendingMedia::kFlux && unit != 0) {
        // Reject BEFORE touching buf: the FDC still holds the current
        // drive-B buffer, and replacing it here would dangle that pointer.
        LOG_ERROR("subcycle engine: flux is drive-A-only — swap ignored");
        b.swap_bytes.clear();
        break;
      }
      flush_dirty_media_unit(b, unit);  // the outgoing disc keeps its writes
      buf = std::move(b.swap_bytes);
      const bool ok = (kind == PendingMedia::kFlux)
                          ? b.machine.insert_flux(buf.data(), buf.size())
                          : b.machine.insert_disk(buf.data(), buf.size(), unit);
      if (ok) {
        LOG_INFO("subcycle engine: drive "
                 << drive << " hot-swapped (" << buf.size() << " bytes"
                 << (kind == PendingMedia::kFlux ? ", flux)" : ")"));
      } else {
        LOG_ERROR("subcycle engine: drive "
                  << drive << " hot-swap rejected (bad image)");
        b.machine.eject_disk(unit);
      }
      break;
    }
    case PendingMedia::kEject:
      flush_dirty_media_unit(b, unit);  // the outgoing disc keeps its writes
      b.machine.eject_disk(unit);
      LOG_INFO("subcycle engine: drive " << drive << " ejected");
      break;
    case PendingMedia::kTape: {
      b.tape_media = std::move(b.swap_bytes);
      if (b.machine.insert_tape(b.tape_media.data(), b.tape_media.size())) {
        LOG_INFO("subcycle engine: cassette inserted (" << b.tape_media.size()
                                                        << " bytes)");
      } else {
        LOG_ERROR("subcycle engine: not a CDT/TZX — deck left empty");
        b.machine.eject_tape();
      }
      break;
    }
    case PendingMedia::kTapeEject:
      b.machine.eject_tape();
      b.tape_media.clear();
      LOG_INFO("subcycle engine: cassette ejected");
      break;
    case PendingMedia::kNone:
      break;
  }
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
const std::vector<int16_t>& subcycle_bridge_frame(const uint8_t rows[16],
                                                  SDL_Surface* dst,
                                                  bool limit) {
  Bridge& b = g_bridge;
  if (!b.active) return g_empty_audio;

  // Tier benchmark slice (before the real frame: last frame's audio is
  // consumed, and the real frame repaints the whole framebuffer after us).
  if (b.bench_want.load(std::memory_order_relaxed) != 0 || b.bench_tier >= 0) {
    using RT = subcycle::Machine::RunTier;
    static const RT kBench[4] = {RT::Fast, RT::Wake, RT::Soldered,
                                 RT::Faithful};
    if (b.bench_tier < 0) {  // arm: start at tier 0 with fresh numbers
      b.bench_want.store(0, std::memory_order_relaxed);
      for (auto& f : b.bench_fps) f.store(-1, std::memory_order_relaxed);
      b.bench_tier = 0;
      b.bench_frames = 0;
      b.bench_secs = 0.0;
      b.bench_snap = b.machine.save_devices();
    }
    const uint64_t freq = SDL_GetPerformanceFrequency();
    const uint64_t slice_end = SDL_GetPerformanceCounter() + (freq * 12 / 1000);
    b.machine.set_run_tier(kBench[b.bench_tier]);
    // Per tier: up to 100 frames or 1.0 s of accumulated bench time,
    // whichever first — fast tiers finish in a few slices, the Microscope
    // tiers spread over ~1.5 s of real frames each; ~3.5-5 s total.
    while (b.bench_frames < 100 && b.bench_secs < 1.0 &&
           SDL_GetPerformanceCounter() < slice_end) {
      const uint64_t t0 = SDL_GetPerformanceCounter();
      b.machine.run_frame();
      b.bench_secs +=
          static_cast<double>(SDL_GetPerformanceCounter() - t0) / freq;
      b.bench_frames++;
    }
    if (b.bench_frames >= 100 ||
        b.bench_secs >= 1.0) {  // tier sampled: publish, restore, advance
      b.bench_fps[b.bench_tier].store(
          b.bench_secs > 0.0 ? static_cast<int>(b.bench_frames / b.bench_secs)
                             : -1,
          std::memory_order_relaxed);
      b.machine.load_devices(b.bench_snap);
      b.bench_frames = 0;
      b.bench_secs = 0.0;
      if (++b.bench_tier >= 4) {
        b.bench_tier = -1;  // done; state restored, policy re-resolves below
        b.bench_snap.clear();
        b.bench_snap.shrink_to_fit();
      }
    } else {
      b.machine.load_devices(b.bench_snap);  // partial slice: restore, resume
    }  // this tier next frame
  }

  apply_pending_media(b);  // hot-swaps land between frames, never mid-tick
  if (b.mf2_stop.exchange(false, std::memory_order_acq_rel))
    b.machine.mf2_stop_button();  // the red button, between frames

  // The whole keyboard, every frame: inherits SDL, autotype, IPC and session
  // input for free — they all end in the app's published matrix.
  for (uint8_t row = 0; row < 16; ++row) b.machine.set_key_row(row, rows[row]);

  if (!b.tier_env_pinned) {  // env pins the tier for bench/harness runs
    using RT = subcycle::Machine::RunTier;
    RT want = RT::Fast;
    switch (b.tier_policy) {
      case BridgeTierPolicy::Auto:
        want = b.debug_engaged ? RT::Wake : RT::Fast;
        break;
      case BridgeTierPolicy::Fast:
        want = RT::Fast;
        break;
      case BridgeTierPolicy::Wake:
        want = RT::Wake;
        break;
      case BridgeTierPolicy::Soldered:
        want = RT::Soldered;
        break;
      case BridgeTierPolicy::Faithful:
        want = RT::Faithful;
        break;
    }
    if (b.machine.run_tier() != want) b.machine.set_run_tier(want);
  }

  b.machine.run_frame();

  blit_fb(b, dst);

  if (limit) {  // drift-corrected 50 Hz deadline (the legacy limiter only
                // paces EC_CYCLE_COUNT exits, which this engine never emits)
    const uint64_t freq = SDL_GetPerformanceFrequency();
    const uint64_t tick = freq / 50;
    uint64_t now = SDL_GetPerformanceCounter();
    if (b.next_deadline == 0 || now > b.next_deadline + (freq / 4))
      b.next_deadline = now;  // (re)sync after start, pause, or a long stall
    while (now < b.next_deadline) {
      const uint64_t remaining_ms = (b.next_deadline - now) * 1000 / freq;
      SDL_Delay(remaining_ms > 2 ? static_cast<Uint32>(remaining_ms - 1) : 0);
      now = SDL_GetPerformanceCounter();
    }
    b.next_deadline += tick;
  } else {
    b.next_deadline = 0;
  }

  return b.machine.audio();
}

/* Re-blit the machine's CURRENT framebuffer without running a frame (the IPC
 * "repaint" path: refresh the presented picture while paused). */
void subcycle_bridge_repaint(SDL_Surface* dst) {
  Bridge& b = g_bridge;
  if (b.active) blit_fb(b, dst);
}

namespace {
void blit_fb(Bridge& b, SDL_Surface* dst) {
  if (dst != nullptr && b.fbsurf != nullptr) {
    // Two passes, each on an SDL fast path: unscaled RGB24→dst-format
    // convert, then a same-format nearest stretch. The one-pass
    // SDL_BlitSurfaceScaled(convert+scale) takes SDL's generic per-pixel
    // fallback — measured ~2 ms/frame on P-cores and ~10 ms on E-cores,
    // 39% of the Z80 thread's time under §8.3 (F8).
    if (b.fbconv == nullptr) {
      b.fbconv = SDL_CreateSurface(subcycle::kFbWidth, subcycle::kFbHeight,
                                   dst->format);
      // Alpha formats default to SDL_BLENDMODE_BLEND — the stretch would
      // alpha-blend every pixel (SDL_Blit_..._Blend_Scale, the E-core
      // profile's top entry). The frame is opaque; copy it.
      if (b.fbconv != nullptr)
        SDL_SetSurfaceBlendMode(b.fbconv, SDL_BLENDMODE_NONE);
    }
    // Integer-exact vertical mapping: the legacy plugins' input surfaces
    // are built around CPC_VISIBLE_SCR_HEIGHT=270 (540 when line-doubled)
    // while the machine's monitor window is 272 lines. A raw full-surface
    // stretch resamples 272→540/270 at a non-integer ratio — nearest drops
    // and doubles lines unevenly, visibly under the CRT styles (Lottes).
    // Crop the fb to the largest centered window the dst holds at an
    // integer factor; the stretch is then exact (270 → clean 2× or 1:1).
    SDL_Rect src{0, 0, b.fbsurf->w, b.fbsurf->h};
    {
      const int fbh = b.fbsurf->h;
      int factor = (dst->h + (fbh / 2)) / fbh;
      factor = std::max(factor, 1);
      int src_h = dst->h / factor;
      src_h = std::min(src_h, fbh);
      src_h = std::max(src_h, 1);
      src.y = (fbh - src_h) / 2;
      src.h = src_h;
    }
    if (b.fbconv != nullptr) {
      SDL_BlitSurface(b.fbsurf, nullptr, b.fbconv, nullptr);
      SDL_BlitSurfaceScaled(b.fbconv, &src, dst, nullptr,
                            SDL_SCALEMODE_NEAREST);
    } else {
      SDL_BlitSurfaceScaled(b.fbsurf, &src, dst, nullptr,
                            SDL_SCALEMODE_NEAREST);
    }
  }
}
}  // namespace
