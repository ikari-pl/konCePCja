---
title: "feat: riscv64 port — konCePCja on LilyGO T-Display K230 (handheld CPC)"
type: feat
status: active
date: 2026-09-28
depth: deep
branch_base: master
---

# feat: riscv64 port — konCePCja on the LilyGO T-Display K230

> **Goal:** a battery-powered handheld Amstrad CPC. Kendryte K230 (dual RISC-V,
> 1.6 GHz big core running Linux), 4.1" AMOLED 568x1232, physical keyboard
> (TCA8418), BQ25896/BQ27220 power path.
>
> **Strategy:** every phase is validated under **emulation** (QEMU/Docker
> riscv64) before anything is copied to the board. The board is the last step,
> not the debugging environment.

---

## Summary

The emulator core should cross-compile to `riscv64` essentially unchanged. An
audit of the tree found **no architecture-specific code at all**: zero files use
x86 intrinsics (`immintrin`/`emmintrin`/`xmmintrin`), ARM NEON, inline `asm`, or
`__SSE`/`__AVX`/`__ARM_NEON` guards, and there are no endianness or word-size
assumptions (`BIG_ENDIAN`/`__builtin_bswap` yield nothing). The project is
C++17 with no `-march`/`-mtune`/`native` flags in `CMakeLists.txt`, Linux is
already a first-class target (full `debian/` packaging, `linux.yml` CI), and the
dependency surface is only **freetype, libpng, zlib** plus a *vendored* SDL3
submodule we build ourselves.

The port is therefore **not a CPU problem — it is a presentation problem.**

The blocker is that the entire "modern" video path presents through **SDL_GPU**,
which needs Vulkan/Metal/D3D12. The K230 has no Vulkan. Fortunately the plugin
registry already contains **8 SDL_Renderer plugins** documented as *"No OpenGL
context required"*, selectable by the positional `scr_style` config index:

| Plugin | init fn |
| --- | --- |
| `Direct (SDL)` | `sdlr_init` |
| `Super eagle (SDL)`, `Scale2x (SDL)`, `Advanced Scale2x (SDL)`, `TV 2x (SDL)`, `Bilinear (SDL)`, `Bicubic (SDL)`, `Dot matrix (SDL)` | `sdlr_swscale_init` |

Everything else (`Direct`, the `swscale_gpu_*` family, all three CRT shader
plugins) routes through `gpu_*` and is **out of scope on this hardware**.

### Performance framing

A 4 MHz Z80 on a 1.6 GHz RISC-V core is ~400x the original clock. Z80 emulation
throughput is not a risk. The real budget question is **software blitting plus
Dear ImGui** at panel resolution — see Phase 4.

---

## Evidence gathered (2026-09-28)

| Question | Finding |
| --- | --- |
| Own source size | 233 files, 84,699 LOC under `src/` |
| Arch-specific code | **none** — no intrinsics, no asm, no endianness guards |
| C++ standard | C++17, no arch flags in CMake |
| Dependencies | `freetype`, `libpng`, `zlib` + vendored SDL3 (`release-3.2.0`) |
| Linux support | `debian/` packaging, `linux.yml` CI, `make install` w/ `DESTDIR` |
| Test suite | **133 test files** under `test/` |
| Headless mode | **`-H/--headless`** already exists: *"run without display or audio"* |
| Headless video | `video_headless_plugin()` registered and usable |
| Non-GPU video | 8 SDL_Renderer plugins, *"No OpenGL context required"* |
| CPC surface | `CPC_VISIBLE_SCR_WIDTH 384`, `CPC_RENDER_WIDTH 768`, `CPC_VISIBLE_SCR_HEIGHT 270` |
| Portrait guard | `video_persisted_window_size_is_sane()`: `kMinAspect = 0.9` — *"portrait is never a CPC window"* |
| Audio | SDL3 `SDL_OpenAudioDeviceStream` (maps to ALSA on Linux) |

### Debian trixie `riscv64` package availability — **verified by probe**

Run under `docker run --platform linux/riscv64 debian:trixie-slim`:

```
cmake 3.31.6-2          g++ 4:14.2.0-1        pkg-config 1.8.1-4
libfreetype-dev 2.13.3  zlib1g-dev 1.3.1      libpng-dev 1.6.48
libdrm-dev 2.4.124-2    libgbm-dev 25.0.7     libasound2-dev 1.2.14
libudev-dev 257.13
```

Every dependency exists. Critically **`libdrm` + `libgbm` (Mesa 25)** are
present, so SDL's **KMSDRM** video driver is buildable and software GL
(`llvmpipe`/`softpipe`) is available as a fallback.

### The orientation trick

The panel is 568x1232 portrait (**0.461:1**) — rejected outright by the
`kMinAspect = 0.9` guard. But presenting **rotated to landscape** gives:

| Geometry | Aspect | Guard (0.9 .. 2.2) |
| --- | --- | --- |
| 568x1232 portrait | 0.461 | **rejected** |
| **1232x568 landscape** | **2.169** | **accepted** |
| 768x270 letterbox | 2.844 | rejected (by design) |

So rotating the display at the DRM/SDL layer lands *just* inside the existing
sanity guard — **no change to `video_persisted_window_size_is_sane()` is
required**, and a rotated CPC is the correct ergonomics for a keyboard handheld
anyway. Rotation belongs in the display setup, not in the emulator.

---

## Emulation ladder (do not touch the board until Tier 3)

### Tier 0 — Docker `linux/riscv64` — **working today, verified**

Docker Desktop 29.4.0 on this machine already runs riscv64 via binfmt/QEMU:
`docker run --rm --platform linux/riscv64 alpine uname -m` -> `riscv64`.

* Build and run a **native riscv64** userland with no cross-toolchain.
* **Caveat:** QEMU user-mode emulation makes a full C++ build of SDL3 + ImGui +
  85k LOC *slow* (hours). Use it for correctness, not iteration.
* **Faster loop:** cross-compile on the aarch64 side, execute only the test
  binaries under emulation.

### Tier 1 — correctness under emulation (highest value)

1. Run the **133-file test suite** on riscv64. This is what catches the classic
   port bugs — struct packing, alignment, `char` signedness (**note: `char` is
   unsigned on RISC-V, signed on x86**), integer promotion in the Z80 core.
2. Run `koncepcja --headless` to validate the whole startup path with no display.
3. **Golden-frame test:** boot a known `.dsk`, capture frames via
   `video_headless_plugin()`, diff against PNGs from the macOS build. Proves the
   pixel pipeline is bit-identical across architectures.

### Tier 2 — the display path, still no board

`brew install qemu` -> `qemu-system-riscv64`, Debian riscv64 rootfs,
`-device virtio-gpu-pci`. This yields a real `/dev/dri/card0`, which exercises
**the same SDL KMSDRM driver the K230 will use**. Set the virtual output to
1232x568 to match the board's rotated geometry.

This is the phase that de-risks the single largest unknown without touching
hardware.

### Tier 3 — real hardware

Copy to `/root` on the SD card (or over MTP / SSH). Only after Tiers 0-2 pass.

---

## Phases

### Phase 1 — toolchain + headless build (est. 0.5-2 days)

* `tools/riscv64/Dockerfile` — Debian trixie riscv64 with the verified deps.
* Build vendored SDL3 for riscv64 following `linux.yml`: install to
  `vendor/SDL/install`, wire `PKG_CONFIG_PATH`/`LD_LIBRARY_PATH`.
  SDL cmake flags: enable **KMSDRM**, **ALSA**; disable Vulkan/X11/Wayland
  initially to keep the surface small.
* Skip `scripts/compile_shaders.sh` — shader blobs only serve the GPU plugins.
* **Keep `KONCPC_BUILD_MODERN_UI=ON`.** `CMakeLists.txt` states the `OFF` path
  *"will fail until P1.5.2 lands a headless main and an ImGui-free imgui_ui.h
  split"* — so ImGui ships; do not attempt to strip it.
* **Exit criteria:** `koncepcja --headless` starts and the test suite passes
  under emulation.

### Phase 2 — display via KMSDRM under QEMU (est. 1-3 days) — **main risk**

* Force the non-GPU path: default `scr_style` to `Direct (SDL)`.
* Confirm whether SDL_Renderer needs GL on this build; if so, either accept
  Mesa software GL or force SDL's `software` render driver via
  `SDL_HINT_RENDER_DRIVER`.
* Verify KMSDRM mode-setting at 1232x568.
* **Exit criteria:** recognisable CPC output in the QEMU window.

### Phase 3 — geometry + input (est. 1-2 days)

* Rotate to landscape at the DRM/SDL layer (see orientation trick — keeps the
  aspect guard satisfied without code changes).
* Map the **TCA8418** matrix to the CPC keyboard. `src/cpc_key_tables.h` holds
  the CPC key-name map; the host side needs a scancode source for the base
  board's keyboard rather than a desktop layout.
* Touch (GT9895) drives ImGui via SDL's touch-to-mouse mapping — no emulator
  changes expected.
* Pick sane `scr_style`/scale defaults for the panel and ship them as a profile
  (`config_profile.cpp` already supports profiles outranking the checkout).

### Phase 4 — audio, performance, packaging (est. 1-2 days)

* Audio: SDL3 -> ALSA. Board routes through I2S/MAX98357A; confirm the default
  device and that `Settings > Audio` on the launcher side is not holding it.
* **Performance measurement is mandatory here, not optional.** Software blit at
  1232x568 is ~700k px/frame; at 50 Hz that is ~35 Mpx/s *before* ImGui.
  Levers, cheapest first: 1x scale instead of a 2x scaler; `Direct (SDL)` rather
  than `swscale`; reduce ImGui chrome; consider pinning emulation to the
  800 MHz little core. `koncepcja_bench` and `docs/board-performance.md` already
  exist — reuse them rather than inventing new harnesses.
* Package into `/root` on the SD image via `make install DESTDIR=...`.

---

## Risks

| # | Risk | Severity | Mitigation |
| --- | --- | --- | --- |
| 1 | No Vulkan -> SDL_GPU path unusable; CRT shaders lost | **High**, but solved | 8 SDL_Renderer plugins already exist |
| 2 | KMSDRM absent/incomplete in the K230 BSP | **High** | Probe `/dev/dri` early (see below); QEMU virtio-gpu proves the code path regardless |
| 3 | Software-blit + ImGui too slow at panel res | **Medium** | Phase 4 levers; measure with `koncepcja_bench` |
| 4 | `MODERN_UI=OFF` does not link (P1.5.2 pending) | Medium | Ship ImGui; do not strip |
| 5 | QEMU build times destroy iteration speed | Medium | Cross-compile; emulate only test execution |
| 6 | `char` unsigned on RISC-V | Low-Medium | Test suite is the detector; Tier 1 exists for this |

---

## First action — de-risk Risk #2 in 30 seconds

On the booted board (`Terminal` app or SSH):

```sh
ls /dev/dri/ /sys/class/drm/            # card0 present? -> KMSDRM viable
ldconfig -p | grep -iE 'drm|gbm|EGL|GLESv2|asound'
grep -m1 isa /proc/cpuinfo              # confirm rv64 ISA extensions
aplay -l                                # ALSA devices
```

If `/dev/dri/card0` exists, Phase 2 collapses to near-trivial and the whole port
is a comfortable weekend. If it does not, the present path must be designed
deliberately (framebuffer or a custom DRM dumb-buffer present) rather than
discovered mid-phase.

---

## Out of scope

* CRT shader plugins and every `gpu_*` presentation path.
* Multi-viewport ImGui — the SDL_Renderer plugins set `flip_b = nullptr`.
* The 800 MHz little core / RT-Smart split (interesting for deterministic
  timing; revisit only if Phase 4 measurement demands it).

---

## Phase 1 RESULT — validated under emulation, 2026-09-28

**Phase 1 is complete and green.** Everything below was produced inside
`docker run --platform linux/riscv64` on an aarch64 host, with no hardware.

| Step | Result | Time (-j6, emulated) |
| --- | --- | --- |
| SDL3 3.2.0 configure + build + install | ✅ | ~6m30s |
| konCePCja configure | ✅ | ~4m40s |
| konCePCja build | ✅ | ~10m |
| Binary identity | ✅ ELF64 LSB, `e_machine 0xF3` (EM_RISCV) | — |
| Runs natively | ✅ prints `konCePCja v6.3.1` | — |
| `--headless` smoke | ✅ | — |
| **Test suite** | ✅ **1315 passed / 0 failed / 21 skipped** | ~5s |

### The test result in detail

`test_runner` must be run **from the repo root** — fixtures are referenced
relatively (e.g. `test/zip/test1.zip`), so running from the build directory
produces 17 spurious failures. From the root:

* **1302 pass** in the main sweep, **13 more** in the socket group.
* **21 skipped**, all for expected environmental reasons: `VideoGpuTest.*` and
  `SdlGpuSmokeTest.*` (no Vulkan — exactly as predicted), window/geometry tests
  (no display), optional flux captures not provided, and the shipped-ROM
  resolver (no `APP_PATH`).
* **0 failures.**

**Risk #6 (`char` unsigned on RISC-V) is retired.** The container confirmed
`char is UNSIGNED` on this toolchain, and the full suite still passes — so the
Z80 core, CRTC, gate array, FDC and flux decoders carry no signedness,
endianness or alignment assumptions. This was the single largest correctness
unknown and it is now measured, not assumed.

**Risk #5 (QEMU build times) is retired.** ~17 minutes for a cold full build at
`-j6`. Tier 0 is an iteration loop, not an overnight batch. The Phase 1 estimate
of 0.5-2 days was pessimistic by an order of magnitude.

### Two environment notes for CI

1. **`test_runner` is not in the default target** — it must be built explicitly
   (`--target test_runner`); `ctest` alone reports "No tests were found" and
   *exits 0*, so a zero-test run silently looks like a pass. Use
   `--no-tests=error`.
2. **The 13 `*Net*` M4-board tests raise `SIGPIPE`** in a bare container and
   abort the whole runner. With `SIGPIPE` ignored (`trap "" PIPE`) all 13 pass.
   Worth checking whether the production code should be setting `MSG_NOSIGNAL` /
   `SO_NOSIGPIPE` on those sends rather than relying on the ambient disposition.

### Revised risk table

| # | Risk | Status |
| --- | --- | --- |
| 1 | No Vulkan -> SDL_GPU unusable | Known; 8 SDL_Renderer plugins are the path |
| 2 | KMSDRM absent in K230 BSP | **Reduced** — `libdrm 2.4.124` + `gbm 25.0.7` found, `SDL_KMSDRM: ON`, builds clean. Board-side presence still unverified |
| 3 | Software blit + ImGui too slow | **Now the #1 risk.** Unmeasured |
| 4 | `MODERN_UI=OFF` does not link | Avoided — built with `ON` |
| 5 | QEMU build times | **Retired** — ~17 min cold |
| 6 | `char` unsigned on RISC-V | **Retired** — 1315 tests pass |

### Next: Phase 2

The emulator is proven correct on riscv64. What remains is entirely
presentation. Install `qemu-system-riscv64` (`brew install qemu`), boot a
riscv64 rootfs with `-device virtio-gpu-pci` for a real `/dev/dri/card0`, set
`scr_style` to `Direct (SDL)`, and get a CPC frame on screen at 1232x568.
