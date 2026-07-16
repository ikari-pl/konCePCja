/* koncepcja_sim.cpp — a standalone host that runs the sub-cycle, pin-level chip simulation
 * (src/hw) as a live Amstrad CPC: it assembles the board (Z80 + Gate Array + CRTC +
 * PPI 8255 + AY-3-8912 PSG + memory + video), boots the real firmware ROM, shows the
 * screen in an SDL window, and feeds the host keyboard into the PSG's key matrix
 * through the same path the firmware scans.
 *
 * This deliberately does NOT touch the legacy emulator loop — it drives the simulation
 * cores directly, so the chip Devices stay swappable and independently verifiable.
 *
 *   Interactive:  ./koncepcja_sim [--rom PATH] [--disk PATH.dsk] [--flux PATH.scp] [--scale N]
 *   Headless dump: ./koncepcja_sim --frames N [--shot out.ppm]   (no window)
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "hw/fdc.h"
#include "hw/flux.h"
#include "subcycle/machine.h"

namespace {

constexpr int kW = subcycle::kFbWidth, kH = subcycle::kFbHeight;

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

constexpr long kAudioHz = subcycle::kAudioHz;

// Minimal RIFF reader for the drive-sound assets: PCM16 mono 44.1 kHz only.
bool load_wav(const char* path, std::vector<int16_t>& out) {
  const std::vector<uint8_t> f = read_file(path);
  if (f.size() < 44 || std::memcmp(f.data(), "RIFF", 4) != 0 ||
      std::memcmp(f.data() + 8, "WAVE", 4) != 0)
    return false;
  size_t pos = 12;
  bool fmt_ok = false;
  while (pos + 8 <= f.size()) {
    const uint32_t len = static_cast<uint32_t>(f[pos + 4]) | (f[pos + 5] << 8) |
                         (f[pos + 6] << 16) | (static_cast<uint32_t>(f[pos + 7]) << 24);
    if (std::memcmp(&f[pos], "fmt ", 4) == 0 && len >= 16) {
      const uint16_t afmt = static_cast<uint16_t>(f[pos + 8] | (f[pos + 9] << 8));
      const uint16_t ch = static_cast<uint16_t>(f[pos + 10] | (f[pos + 11] << 8));
      const uint32_t rate = static_cast<uint32_t>(f[pos + 12]) | (f[pos + 13] << 8) |
                            (f[pos + 14] << 16) | (static_cast<uint32_t>(f[pos + 15]) << 24);
      const uint16_t bits = static_cast<uint16_t>(f[pos + 22] | (f[pos + 23] << 8));
      fmt_ok = (afmt == 1 && ch == 1 && rate == kAudioHz && bits == 16);
    } else if (std::memcmp(&f[pos], "data", 4) == 0 && fmt_ok) {
      const size_t n = std::min<size_t>(len, f.size() - pos - 8) / 2;
      out.resize(n);
      std::memcpy(out.data(), &f[pos + 8], n * 2);
      return n > 0;
    }
    pos += 8 + len + (len & 1);
  }
  return false;
}

// The whole simulated machine, kept together so both modes share assembly.
// Drive-sound overlay: WAV assets + voices, fed FDC events by the machine and
// mixed one mono sample at a time (see subcycle::AudioOverlay).
struct DriveSounds final : subcycle::AudioOverlay {
  struct Voice { const std::vector<int16_t>* pcm; size_t pos; bool loop; };
  std::vector<int16_t> spinup, hum, spindown, step, index_tick;
  std::vector<Voice> voices;
  bool enabled = false;

  void load() {
    struct { const char* name; std::vector<int16_t>* dst; } assets[] = {
        {"motor_spinup.wav", &spinup},     {"motor_hum_loop.wav", &hum},
        {"motor_spindown.wav", &spindown}, {"step_single.wav", &step},
        {"index_tick.wav", &index_tick},
    };
    enabled = true;
    for (auto& a : assets) {
      char path[256];
      std::snprintf(path, sizeof(path), "resources/drive-sounds/%s", a.name);
      if (!load_wav(path, *a.dst)) {
        std::snprintf(path, sizeof(path), "../resources/drive-sounds/%s", a.name);
        if (!load_wav(path, *a.dst)) enabled = false;
      }
    }
    if (!enabled) std::fprintf(stderr, "drive sounds: assets not found, disabled\n");
  }

  void start(const std::vector<int16_t>& pcm, bool loop) {
    if (voices.size() < 12) voices.push_back(Voice{&pcm, 0, loop});
  }
  void stop_loops() {
    for (size_t i = 0; i < voices.size();)
      if (voices[i].loop) voices.erase(voices.begin() + static_cast<long>(i));
      else ++i;
  }

  void events(const FdcEvent* ev, int n) override {
    if (!enabled) return;
    for (int i = 0; i < n; ++i) {
      switch (ev[i].type) {
        case FDC_EV_MOTOR_ON: start(spinup, false); break;
        case FDC_EV_MOTOR_READY: start(hum, true); break;
        case FDC_EV_MOTOR_OFF: stop_loops(); start(spindown, false); break;
        case FDC_EV_STEP: start(step, false); break;
        case FDC_EV_INDEX: start(index_tick, false); break;
        default: break;
      }
    }
  }

  int32_t sample() override {
    int32_t sum = 0;
    for (size_t i = 0; i < voices.size();) {
      Voice& v = voices[i];
      sum += (*v.pcm)[v.pos];
      if (++v.pos >= v.pcm->size()) {
        if (v.loop) { v.pos = 0; ++i; }
        else voices.erase(voices.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
    return sum;
  }
};

// The sim host: file loading + buffer ownership around the embeddable machine.
struct Machine {
  subcycle::Machine core;
  std::vector<uint8_t> rom, amsdos, disk, fb;
  DriveSounds sounds;

  bool build(const char* rom_path) {
    rom = read_file(rom_path);
    if (!core.build(rom.data(), rom.size())) {
      std::fprintf(stderr, "ROM %s not found/too small\n", rom_path);
      return false;
    }
    amsdos = read_file("rom/amsdos.rom");
    if (amsdos.size() < 0x4000) amsdos = read_file("../rom/amsdos.rom");
    core.attach_amsdos(amsdos.data(), amsdos.size());
    sounds.load();
    if (sounds.enabled) core.set_overlay(&sounds);
    fb.assign(static_cast<size_t>(kW) * kH * 3, 0);
    core.attach_framebuffer(fb.data(), kW, kH);
    return true;
  }

  bool insert_disk(const char* path) {
    disk = read_file(path);
    if (disk.empty() || !core.insert_disk(disk.data(), disk.size())) {
      std::fprintf(stderr, "disk %s not found / not a DSK image\n", path);
      return false;
    }
    return true;
  }

  bool insert_flux(const char* path) {
    disk = read_file(path);  // the buffer IS the live medium: keep it owned here
    if (disk.empty() || !core.insert_flux(disk.data(), disk.size())) {
      std::fprintf(stderr, "flux %s not found / not an SCP flux image\n", path);
      return false;
    }
    std::fprintf(stderr, "flux %s: %d cylinder(s), %d revolution(s)/track\n", path,
                 flux_scp_cylinders(disk.data(), disk.size()),
                 flux_scp_revolutions(disk.data(), disk.size()));
    return true;
  }

  void run_frame() { core.run_frame(); }
  const std::vector<int16_t>& audio() const { return core.audio(); }
};

// Printable char → CPC matrix byte (line<<4|bit) + whether SHIFT is needed. Enough to
// type a BASIC line; unmapped chars return {0xFF, false}.
struct CpcKey { uint8_t code; bool shift; };
CpcKey cpc_char(char c) {
  switch (c) {
    case 'a': return {0x85,0}; case 'b': return {0x66,0}; case 'c': return {0x76,0};
    case 'd': return {0x75,0}; case 'e': return {0x72,0}; case 'f': return {0x65,0};
    case 'g': return {0x64,0}; case 'h': return {0x54,0}; case 'i': return {0x43,0};
    case 'j': return {0x55,0}; case 'k': return {0x45,0}; case 'l': return {0x44,0};
    case 'm': return {0x46,0}; case 'n': return {0x56,0}; case 'o': return {0x42,0};
    case 'p': return {0x33,0}; case 'q': return {0x83,0}; case 'r': return {0x62,0};
    case 's': return {0x74,0}; case 't': return {0x63,0}; case 'u': return {0x52,0};
    case 'v': return {0x67,0}; case 'w': return {0x73,0}; case 'x': return {0x77,0};
    case 'y': return {0x53,0}; case 'z': return {0x87,0};
    case '0': return {0x40,0}; case '1': return {0x80,0}; case '2': return {0x81,0};
    case '3': return {0x71,0}; case '4': return {0x70,0}; case '5': return {0x61,0};
    case '6': return {0x60,0}; case '7': return {0x51,0}; case '8': return {0x50,0};
    case '9': return {0x41,0};
    case ' ': return {0x57,0}; case '\n': return {0x22,0};       // RETURN
    case '"': return {0x81,1};  // shift+2
    case '+': return {0x34,1};  // shift+;  → +
    case '*': return {0x35,1};  // shift+:  → *
    case '&': return {0x60,1};  // shift+6  → &  (hex literals)
    case ':': return {0x35,0};  // statement separator
    case '(': return {0x50,1};  // shift+8
    case ')': return {0x41,1};  // shift+9
    case '.': return {0x37,0}; case ',': return {0x47,0};
    default:  return {0xFF,0};
  }
}

// Inject a string through the PSG key matrix the way a person would type it, holding
// each key across several keyboard scans, then releasing with a gap. Advances the
// simulation between events so the firmware actually samples the keys.
void type_text(Machine& m, const char* text) {
  auto hold = [&](int cpc, bool down) {
    if (cpc == 0xFF) return;
    m.core.key(static_cast<uint8_t>(cpc), down);
  };
  for (const char* p = text; *p; ++p) {
    const CpcKey k = cpc_char(*p);
    if (k.code == 0xFF) continue;
    if (k.shift) hold(0x25, true);                 // press CPC SHIFT
    hold(k.code, true);
    for (int f = 0; f < 4; ++f) m.run_frame();     // held across scans
    hold(k.code, false);
    if (k.shift) hold(0x25, false);
    for (int f = 0; f < 4; ++f) m.run_frame();     // debounce gap
  }
}

// Presentation-only CRT scanline effect: double the height, copying each simulated
// scanline to an even row and leaving a blank (black) row between. This is a display
// filter on the emitted frame — the video Device stays a faithful hardware model.
void expand_scanlines(const uint8_t* fb, uint8_t* disp) {
  for (int y = 0; y < kH; ++y) {
    uint8_t* even = disp + (static_cast<size_t>(2 * y) * kW * 3);
    std::memcpy(even, fb + (static_cast<size_t>(y) * kW * 3), static_cast<size_t>(kW) * 3);
    std::memset(even + static_cast<size_t>(kW) * 3, 0, static_cast<size_t>(kW) * 3);  // blank row
  }
}

// Minimal PCM s16le WAV writer (ch channels, interleaved), for capturing the output.
void write_wav(const char* path, const std::vector<int16_t>& s, int rate, int ch) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  const uint32_t data_bytes = static_cast<uint32_t>(s.size() * 2);
  const uint16_t block = static_cast<uint16_t>(ch * 2);
  const uint32_t byte_rate = static_cast<uint32_t>(rate) * block;
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); u32(36 + data_bytes); std::fwrite("WAVE", 1, 4, f);
  std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(static_cast<uint16_t>(ch));
  u32(static_cast<uint32_t>(rate)); u32(byte_rate); u16(block); u16(16);
  std::fwrite("data", 1, 4, f); u32(data_bytes);
  std::fwrite(s.data(), 2, s.size(), f);
  std::fclose(f);
  std::fprintf(stderr, "wrote %s (%zu frames, %.2fs)\n", path, s.size() / ch,
               static_cast<double>(s.size() / ch) / rate);
}

void write_ppm(const char* path, const uint8_t* fb, int height) {
  if (FILE* f = std::fopen(path, "wb")) {
    std::fprintf(f, "P6\n%d %d\n255\n", kW, height);
    std::fwrite(fb, 1, static_cast<size_t>(kW) * height * 3, f);
    std::fclose(f);
    std::fprintf(stderr, "wrote %s\n", path);
  }
}

}  // namespace

#ifndef SIM_HEADLESS_ONLY
#include "SDL3/SDL.h"

namespace {

// Physical SDL scancode → CPC matrix byte (high nibble = keyboard line, low nibble =
// bit; a pressed key CLEARS its bit). Host Shift/Ctrl map to the CPC's own Shift/Ctrl
// keys, so Shift+a naturally yields 'A'. Unmapped keys return 0xFF.
uint8_t cpc_scancode(SDL_Scancode sc) {
  switch (sc) {
    case SDL_SCANCODE_A: return 0x85; case SDL_SCANCODE_B: return 0x66;
    case SDL_SCANCODE_C: return 0x76; case SDL_SCANCODE_D: return 0x75;
    case SDL_SCANCODE_E: return 0x72; case SDL_SCANCODE_F: return 0x65;
    case SDL_SCANCODE_G: return 0x64; case SDL_SCANCODE_H: return 0x54;
    case SDL_SCANCODE_I: return 0x43; case SDL_SCANCODE_J: return 0x55;
    case SDL_SCANCODE_K: return 0x45; case SDL_SCANCODE_L: return 0x44;
    case SDL_SCANCODE_M: return 0x46; case SDL_SCANCODE_N: return 0x56;
    case SDL_SCANCODE_O: return 0x42; case SDL_SCANCODE_P: return 0x33;
    case SDL_SCANCODE_Q: return 0x83; case SDL_SCANCODE_R: return 0x62;
    case SDL_SCANCODE_S: return 0x74; case SDL_SCANCODE_T: return 0x63;
    case SDL_SCANCODE_U: return 0x52; case SDL_SCANCODE_V: return 0x67;
    case SDL_SCANCODE_W: return 0x73; case SDL_SCANCODE_X: return 0x77;
    case SDL_SCANCODE_Y: return 0x53; case SDL_SCANCODE_Z: return 0x87;
    case SDL_SCANCODE_0: return 0x40; case SDL_SCANCODE_1: return 0x80;
    case SDL_SCANCODE_2: return 0x81; case SDL_SCANCODE_3: return 0x71;
    case SDL_SCANCODE_4: return 0x70; case SDL_SCANCODE_5: return 0x61;
    case SDL_SCANCODE_6: return 0x60; case SDL_SCANCODE_7: return 0x51;
    case SDL_SCANCODE_8: return 0x50; case SDL_SCANCODE_9: return 0x41;
    case SDL_SCANCODE_RETURN: return 0x22; case SDL_SCANCODE_SPACE: return 0x57;
    case SDL_SCANCODE_BACKSPACE: return 0x97;  // DEL (line 9 bit 7)
    case SDL_SCANCODE_ESCAPE: return 0x82; case SDL_SCANCODE_TAB: return 0x84;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x25;  // CPC SHIFT
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x27;    // CPC CONTROL
    case SDL_SCANCODE_LALT: return 0x11;                             // CPC COPY
    case SDL_SCANCODE_UP: return 0x00; case SDL_SCANCODE_DOWN: return 0x02;
    case SDL_SCANCODE_LEFT: return 0x10; case SDL_SCANCODE_RIGHT: return 0x01;
    case SDL_SCANCODE_PERIOD: return 0x37; case SDL_SCANCODE_COMMA: return 0x47;
    case SDL_SCANCODE_SEMICOLON: return 0x34; case SDL_SCANCODE_SLASH: return 0x36;
    case SDL_SCANCODE_MINUS: return 0x31; case SDL_SCANCODE_LEFTBRACKET: return 0x21;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x23; case SDL_SCANCODE_BACKSLASH: return 0x26;
    default: return 0xFF;
  }
}

int run_interactive(Machine& m, int scale, bool scanlines) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  const int tex_h = scanlines ? kH * 2 : kH;  // doubled height for the CRT look
  SDL_Window* win = SDL_CreateWindow("konCePCja — chip simulation",
                                     kW * scale, tex_h * scale, 0);
  SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
  SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
                                       SDL_TEXTUREACCESS_STREAMING, kW, tex_h);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  std::vector<uint8_t> disp(static_cast<size_t>(kW) * (kH * 2) * 3, 0);

  // Stereo 16-bit PSG audio at the host rate (CPC wiring: A left, C right, B centre);
  // we push each frame and let SDL buffer it. We feed with SDL_PutAudioStreamData.
  const SDL_AudioSpec aspec{SDL_AUDIO_S16, 2, static_cast<int>(kAudioHz)};
  SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(
      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &aspec, nullptr, nullptr);
  if (audio) SDL_ResumeAudioStreamDevice(audio);
  else std::fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());

  for (int r = 0; r < 16; ++r) m.core.set_key_row(static_cast<uint8_t>(r), 0xFF);

  constexpr double kFrameMs = 1000.0 / 50.0;  // 50 Hz
  double next_frame = static_cast<double>(SDL_GetTicks());
  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) running = false;
      else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F5) {
          m.core.reset();  // F5 = reset the machine
          continue;
        }
        const uint8_t cpc = cpc_scancode(e.key.scancode);
        if (cpc == 0xFF) continue;
        m.core.key(cpc, e.type == SDL_EVENT_KEY_DOWN);
      }
    }

    m.run_frame();
    // Push this frame's audio, but cap the queue (~4 frames) so latency can't grow if
    // we ever run a touch fast — drop a frame's worth rather than drift behind.
    if (audio && !m.audio().empty()) {
      const int cap = static_cast<int>(kAudioHz / 50) * 2 * 2 * 4;  // ~4 stereo frames
      if (SDL_GetAudioStreamQueued(audio) < cap)
        SDL_PutAudioStreamData(audio, m.audio().data(),
                               static_cast<int>(m.audio().size() * sizeof(int16_t)));
    }
    if (scanlines) {
      expand_scanlines(m.fb.data(), disp.data());
      SDL_UpdateTexture(tex, nullptr, disp.data(), kW * 3);
    } else {
      SDL_UpdateTexture(tex, nullptr, m.fb.data(), kW * 3);
    }
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);

    // Drift-corrected pacing: sleep until the next 50 Hz deadline (not a fixed
    // per-frame delay), and resync if we ever fall far behind so it can't spiral.
    next_frame += kFrameMs;
    const double now = static_cast<double>(SDL_GetTicks());
    if (now < next_frame) SDL_Delay(static_cast<uint32_t>(next_frame - now));
    else if (now - next_frame > 100.0) next_frame = now;
  }

  if (audio) SDL_DestroyAudioStream(audio);
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}

}  // namespace
#endif  // SIM_HEADLESS_ONLY

int main(int argc, char** argv) {
  const char* rom = "rom/cpc6128.rom";
  const char* disk = nullptr;
  const char* flux = nullptr;
  const char* shot = nullptr;
  const char* type = nullptr;
  const char* wav = nullptr;
  int scale = 3, frames = 0, tail = 40, crtc_type = 0;
  bool scanlines = true;  // CRT look: blank row between scanlines (test display)
  bool drive_sounds_on = true;  // FDC mechanical events → the modeled WAV assets
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
    else if (!std::strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
    else if (!std::strcmp(argv[i], "--flux") && i + 1 < argc) flux = argv[++i];
    else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!std::strcmp(argv[i], "--type") && i + 1 < argc) type = argv[++i];
    else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
    else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) tail = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--crtc") && i + 1 < argc) crtc_type = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--no-scanlines")) scanlines = false;
    else if (!std::strcmp(argv[i], "--scanlines")) scanlines = true;
    else if (!std::strcmp(argv[i], "--no-drive-sounds")) drive_sounds_on = false;
  }

  Machine m;
  if (!m.build(rom)) return 1;
  if (disk && !m.insert_disk(disk)) return 1;
  if (flux && !m.insert_flux(flux)) return 1;
  m.core.set_crtc_type(static_cast<uint8_t>(crtc_type & 3));  // which 6845 variant
  if (!drive_sounds_on) { m.sounds.enabled = false; m.core.set_overlay(nullptr); }

  if (frames > 0) {  // headless batch: boot, optionally type a line, then dump a PPM/WAV
    int au_peak = 0;
    std::vector<int16_t> cap;
    auto pump = [&](int n) {
      for (int f = 0; f < n; ++f) {
        m.run_frame();
        for (int16_t s : m.audio()) au_peak = std::max<int>(au_peak, std::abs(s));
        if (wav) cap.insert(cap.end(), m.audio().begin(), m.audio().end());
      }
    };
    pump(frames);
    if (type) type_text(m, type);
    pump(tail);  // let any triggered sound play out
    std::fprintf(stderr, "audio peak sample = %d\n", au_peak);
    if (wav) write_wav(wav, cap, static_cast<int>(kAudioHz), 2);
    int nonzero = 0;
    for (uint8_t v : m.fb) if (v) nonzero++;
    std::fprintf(stderr, "ran %d frames, %d/%zu nonzero fb bytes\n", frames, nonzero, m.fb.size());
    if (shot) {
      if (scanlines) {
        std::vector<uint8_t> disp(static_cast<size_t>(kW) * (kH * 2) * 3, 0);
        expand_scanlines(m.fb.data(), disp.data());
        write_ppm(shot, disp.data(), kH * 2);
      } else {
        write_ppm(shot, m.fb.data(), kH);
      }
    }
    return 0;
  }

#ifndef SIM_HEADLESS_ONLY
  return run_interactive(m, scale, scanlines);
#else
  std::fprintf(stderr, "built headless-only; use --frames N\n");
  return 0;
#endif
}
