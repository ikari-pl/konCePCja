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

---

## Phase 2 (partial) RESULT — the SDL_Renderer path runs, 2026-09-28

The SDL3 build has **`KMSDRM`, `OFFSCREEN` and `DUMMY`** video drivers compiled
in (confirmed in `SDL_build_config.h`). `OFFSCREEN` makes the whole render path
testable **with no display and no VM** — so most of Phase 2's correctness
question was answerable in the container.

### SDL_GPU is confirmed unavailable — and the code already handles it

Running plugin index 0 (`Direct`, a `gpu_*` plugin):

```
video_gpu.cpp:153  - SDL_CreateGPUDevice failed: No supported SDL_GPU backend found!
kon_cpc_ja.cpp:1981 - Could not set requested video mode: ... — trying SDL_Renderer fallback
```

Index 11 (`Direct (SDL)`) produces **no GPU error at all**. This is direct
confirmation of the plan's central thesis, from the target architecture.

**New finding — Risk #1 is softer than assumed.** `kon_cpc_ja.cpp:1981` already
implements an **automatic SDL_Renderer fallback** when GPU init fails. The
emulator degrades gracefully on hardware without Vulkan rather than dying, so a
wrong `scr_style` on the board is a performance/quality issue, not a brick.
Setting `scr_style` correctly is still preferred (skip the failed GPU probe).

### Measured: full-speed emulation, render cost ~zero

`--exit-after=120f --fps`, `SDL_VIDEODRIVER=offscreen`, pin-level board
(`run_tier: faithful`, model 2, 128 KB):

| scr_style | Plugin | Result |
| --- | --- | --- |
| 0 | Direct (GPU, falls back) | 51 FPS, 102% speed, render-wait 0.0 ms/f (0%) |
| **11** | **Direct (SDL)** | **50 FPS, 100% speed, render-wait 0.0 ms/f (0%)** |
| 13 | Scale2x (SDL) | 50 FPS, 100% speed, render-wait 0.0 ms/f (0%) |
| 21 | Dot matrix (SDL) | 51 FPS, 102% speed, render-wait 0.0 ms/f (0%) |

**Machine time ~19.9-20.0 ms/frame against a 20 ms budget, and render-wait is
0% on every SDL_Renderer plugin including the 2x scalers.**

### Honest caveats on those numbers

These are **not** a prediction for K230 silicon:

1. **QEMU user-mode emulation** of riscv64 on an M2-class aarch64 host. The host
   core is far stronger than a 1.6 GHz K230 core, while QEMU adds 5-15x
   overhead. The two effects offset by an unknown ratio, so treat this as a
   plausibility proxy, not a measurement.
2. **`OFFSCREEN` never scans out.** Real presentation through KMSDRM costs more
   than rendering to an offscreen surface, so per-frame render cost here is
   understated.

What the numbers *do* establish: the SDL_Renderer plugins **initialise and run
correctly on riscv64 with no GPU and no display**, and the render path does not
stall emulation even with a 2x software scaler. Risk #3 is materially reduced but
not closed — it needs real silicon.

### Phase 2 remaining

`qemu-system-riscv64` is now installed. What is left is the genuine KMSDRM
scanout test: boot a riscv64 rootfs with `-device virtio-gpu-pci` for a real
`/dev/dri/card0`, present at 1232x568, and confirm a visible CPC frame.

---

## Phase 2 REPLAN — SDL's KMSDRM is unusable; present via DRM dumb buffers

**Board fact (2026-09-28): `/dev/dri/card0` exists, and it is the ONLY node in
`/dev/dri` — there is no `renderD128` render node.**

The board's own rootfs was then inspected directly by loop-mounting the ext4
partition of the verified `sysimage-sdcard.img` (offset `134217728`) — no board
interaction needed, since the image is SHA-256 identical to the card.

### The rootfs has no GL stack at all

| Library | Present? |
| --- | --- |
| `libdrm.so.2` (2.124.0) | ✅ |
| `libevdev.so.2` | ✅ |
| `libfreetype.so.6` | ✅ |
| `libasound.so.2` + `/usr/share/alsa` | ✅ |
| `libpng16`, `libz`, `libstdc++.so.6.0.33` | ✅ |
| **`libEGL` / `libGLESv2` / `libgbm` / Mesa / any `dri/*.so`** | ❌ **NONE** |

An exhaustive `find` for `*egl*`, `*gles*`, `*gbm*`, `*mesa*` across the whole
rootfs returns **nothing**. Distro is Buildroot 2025.02.1, riscv64 lp64d.

### Why that rules out SDL's video layer

Verified in the vendored SDL3 source:

* `SDL_kmsdrmvideo.c` has **64 GBM references**; `gbm_create_device()` and
  `gbm_surface_create()` are mandatory. **No `libgbm` -> KMSDRM cannot init.**
* KMSDRM does **not** implement `CreateWindowFramebuffer`, so SDL's
  software-surface present path does not exist on it (the `offscreen` driver
  does implement it — which is why the earlier offscreen tests worked).
* Our build compiled only `SDL_VIDEO_RENDER_GPU` (needs Vulkan) and
  `SDL_VIDEO_RENDER_OGL_ES2` (needs EGL/GLES2). `SDL_VIDEO_RENDER_SW` is
  `#undef`, and even enabled it presents through
  `SDL_GetWindowSurface`/`SDL_UpdateWindowSurface` — the hook KMSDRM lacks.

**Correction to the earlier Phase 2 entry:** the 50-51 FPS offscreen numbers
measured a presentation path the board does not have. They remain valid evidence
that the *emulator core* runs at full speed on riscv64 (`machine 19.9 ms/f`
against a 20 ms budget), but they say nothing about presentation on this
hardware. Risk #3 was not reduced by that test.

### The launcher shows exactly what to do

`/app/k230_phone_ui` links **`liblvgl_linux.so`, `liblvgl.so.9`, `libdrm.so.2`,
`libevdev.so.2`** — and no GL of any kind. Its strings include:

```
/dev/dri/card0
lv_linux_drm_create
lv_linux_drm_find_device_path
lv_linux_drm_set_file
lv_linux_drm_set_rotation
```

So the vendor's own UI renders **in software** into **DRM dumb buffers** and
scans out on `card0`, with **rotation handled in the presenter**. That is the
template, and it is proof the approach works on this exact panel.

### Revised approach: a `drm_direct` video plugin

Write a new `video_plugin` that bypasses SDL's video subsystem:

1. `open("/dev/dri/card0")`, `drmModeGetResources`, pick connector/CRTC/mode.
2. `DRM_IOCTL_MODE_CREATE_DUMB` x2 (double buffer), `MAP_DUMB`, `drmModeAddFB`.
3. Blit the CPC surface into the mapped buffer with the existing swscale
   kernels — they already produce plain pixel output.
4. `drmModePageFlip` to present; handle rotation in the blit (or via a DRM plane
   `rotation` property if the driver exposes one).

This slots into the existing `video_plugin` vtable (`init`/`setpal`/`flip`/
`close`, `flip_b = nullptr`), same shape as the `sdlr_*` plugins.

**This is better than the original plan, not worse.** Software GL via
`kms_swrast` would have rasterised a full-screen textured quad on a 1.6 GHz
core; a dumb-buffer blit removes the rasteriser entirely. For a 2D emulator
scanout that is the right architecture — and it is what the vendor chose.

**SDL is still wanted** for audio (`libasound` present) and input, or those can
go straight to ALSA and **evdev** (`libevdev` present — also the route to the
TCA8418 keyboard, de-risking Phase 3).

### Risk table after Phase 2 investigation

| # | Risk | Status |
| --- | --- | --- |
| 1 | No Vulkan -> SDL_GPU unusable | **Retired** — confirmed; automatic SDL_Renderer fallback exists |
| 2 | KMSDRM absent | **Resolved differently** — `card0` exists, but no GBM, so SDL's KMSDRM is out. Direct DRM is in |
| 3 | Presentation too slow | **Reduced by architecture** — dumb-buffer blit, no rasteriser. Still unmeasured |
| 4 | `MODERN_UI=OFF` does not link | **Now a real problem.** ImGui's only usable backends here are `sdlrenderer3`/`sdlgpu3`, both dependent on SDL video. A `drm_direct` plugin has no ImGui path, so the headless split (P1.5.2) becomes load-bearing — or ImGui must be rendered into the same dumb buffer via a custom backend |
| 5 | QEMU build times | Retired |
| 6 | `char` unsigned on RISC-V | Retired — 1315 tests pass |

**New top risk is #4**, not #3: the emulator screen is straightforward, but the
Dear ImGui UI has no presentation path without SDL video. Options, in order of
increasing effort: ship emulator-only output first (no UI chrome), finish the
P1.5.2 headless/ImGui-free split, or write a minimal ImGui renderer targeting
the dumb buffer.

---

## Hardware facts from the board itself, 2026-09-28

`tools/riscv64/k230.sh` gives a root shell. **No setup was needed:** the stock
image ships OpenSSH with `PermitRootLogin yes`, `PasswordAuthentication yes`,
`PermitEmptyPasswords yes`, root has an empty password, and
`/etc/init.d/S50sshd` starts it at boot. Only the IP was missing
(`192.168.1.182` over Wi-Fi). `telnetd` (`S50telnet`) and a configfs
ADB/MTP USB gadget (`S41adb_mtp`, `usb/bin/adbd`, VID:PID `0x29F1:0x0105`,
manual start) are also available if the network is not.

`Linux canaan 6.6.36 #2 SMP riscv64`, Buildroot 2025.02.1.

### Display — every earlier inference confirmed

```
/dev/dri/card0          (226,0) — and NOTHING else; no renderD128
/sys/class/drm/card0-DSI-1/status = connected
/sys/class/drm/card0-DSI-1/modes  = 568x1232      (the only mode)
driver -> bus/platform/drivers/canaan-drm
```

A display-only KMS driver: it can set a mode and scan out, it cannot render.
`drm_direct` with dumb buffers is therefore the correct and only design, and
**rotation must happen in our blit** — there is no landscape mode to select.

### Three findings that change the plan

**1. Linux sees ONE core.** `nproc` = 1. The 800 MHz little core is running
RT-Smart, not Linux. So there is no second Linux thread to hide render cost on —
emulation and presentation share a single 1.6 GHz core. This raises the
importance of a cheap present path (and vindicates dropping software GL).

**2. The ISA includes the Vector extension.**

```
rv64imafdcv_zicbom_zicboz_zicntr_zicsr_zifencei_zihpm_zba_zbb_zbs_svpbmt
        ^ v = RVV               ^ zba/zbb/zbs = bit manipulation
```

**`v` means RVV is available**, plus the Zb* bitmanip extensions. The blit and
the 2x scaler kernels are exactly the kind of code RVV accelerates. GCC 14.2 is
on the target. Worth building with `-march=rv64gcv` and measuring — potentially
the single largest performance lever, and it costs a compiler flag before any
hand-written intrinsics.

**3. The launcher owns the display.** `k230_phone_ui` (pid 222) is running, plus
a `k230_meshtastic_probe` daemon. It holds DRM master, so it must be stopped
(`/etc/init.d/S99zz_k230_phone_ui stop`) before anything else can modeset.
Reversible.

### Audio: ready

```
card 0: K230I2SINNO [K230_I2S_INNO], device 0: Audio 9140e000.inno_codec-0
```

ALSA enumerates a playback device, and `libasound.so.2` plus `/usr/share/alsa`
are in the rootfs. Phase 4 audio is low risk.

### Input: the keyboard is NOT an evdev device

```
/proc/bus/input/devices:
  N: Name="K230 PMU Power Key"   H: Handlers=kbd event0
  N: Name="goodix_ts"            H: Handlers=kbd mouse0 event1
```

Only the power key and the touchscreen. **There is no TCA8418 evdev node.**
Confirmed by driver binding:

```
/sys/bus/i2c/devices/0-0037  name=gc2093  driver=<none>
/sys/bus/i2c/devices/0-0038  name=aht20   driver=<none>
/sys/bus/i2c/devices/1-005d  name=nottingham driver=gtx8_i2c
/proc/interrupts: 110  gpio-k230  Edge  k230-phone-tca8418-irq
```

The TCA8418 interrupt is wired and named, but **no kernel driver is bound** — so
the launcher reads the keyboard matrix from userspace over I2C (`/dev/i2c-*`),
driven by that GPIO IRQ. Likewise `aht20` and `gc2093` are userspace-driven.

**Phase 3 impact:** we cannot just read evdev for the keyboard. Either
reimplement the TCA8418 userspace scan (I2C address `0x34`, IRQ on `GPIO42`,
reset on `GPIO43` per the pinmap) and feed it into the CPC key matrix, or write
a `uinput` bridge daemon that turns TCA8418 scans into a virtual evdev keyboard
so SDL/our input layer sees a normal keyboard. The bridge is the cleaner split
and is reusable outside konCePCja.

Touch is a normal evdev device, so ImGui/pointer input is easy by comparison.

### Risk table

| # | Risk | Status |
| --- | --- | --- |
| 1 | No Vulkan / SDL_GPU | Retired |
| 2 | KMSDRM / display path | **Resolved** — `canaan-drm`, dumb buffers, 568x1232, rotate in blit |
| 3 | Presentation too slow | Open. Single core hurts; RVV may more than compensate. Measure |
| 4 | ImGui has no present path without SDL video | **Top risk.** Unchanged |
| 5 | QEMU build times | Retired |
| 6 | `char` unsigned | Retired |
| 7 | **Keyboard is not evdev** | **New.** Userspace TCA8418 scan or a uinput bridge |

---

## Phase 2 PROVEN ON HARDWARE — `drm_direct` works, Risk #3 closed

`tools/riscv64/drm_probe.c` (149 lines) was cross-compiled and run on the board.
It creates dumb buffers, modesets, page-flips, and measures the real blit cost.
**The panel displayed CPU-written test bars.**

### Rotation is done in HARDWARE — correcting the earlier entry

`tools/riscv64/drm_planes.c` enumerated 7 planes. The **primary** plane (id 34,
`type=1`) has:

```
rotation = 8      [rotate-0=0 rotate-90=1 rotate-180=2 rotate-270=3 ...]
```

The enum values are **bit positions**, so 8 = `1<<3` = **rotate-270**. The other
six planes are rotate-0. The display engine therefore rotates for us and the
CRTC expects a **landscape** source. That is exactly what the first attempt hit:

```
drm_framebuffer_check_src_coords: Invalid source coordinates
  1232.000000x568.000000+0.000000+0.000000 (fb 568x1232)   -> ret=-28 (ENOSPC)
```

**Correction:** the Phase 2 replan said "rotation must happen in our blit".
Wrong — allocate the dumb buffer as **1232x568 landscape** and rotation is free.
That is also precisely **2.169:1**, the aspect
`video_persisted_window_size_is_sane()` already accepts (0.9 .. 2.2). No guard
change needed.

It further improves the blit: writing landscape rows is sequential, whereas a
CPU-rotated blit wrote columns with a 4928-byte stride between consecutive
pixels — cache-hostile on a single core.

### Measurements (board, 120 frames, `-O2 -static`)

```
connector 54   mode 568x1232@52   crtc 52
dumb buffers   1232x568 landscape, pitch 4928, 2733 KiB each (x2)

A) bare flips      120 in 2.299s = 52.2 FPS (19.16 ms/flip)
B) blit+flip       120 in 2.297s = 52.2 FPS (19.14 ms/frame)
   blit alone      2.26 ms/frame   (11.3% of a 20 ms CPC budget)
```

**Presentation is vsync-bound, not CPU-bound.** Bare flips and blit+flip are
identical, so the blit vanishes into the panel's own 19.16 ms. Roughly **88% of
the frame budget remains for emulation**.

**Risk #3 is closed.** Scaled 768x270 -> 1232x433 presentation costs 2.26 ms on
this hardware.

### RVV is NOT a lever here — correcting an earlier claim

```
-O2               blit 2.26 ms/frame
-O2 -march=rv64gcv blit 2.43 ms/frame     (marginally worse)
```

The earlier note calling `-march=rv64gcv` "potentially the single largest
performance lever" was wrong. This blit is **memory-bound** — a scaled copy with
an indexed gather into an uncached dumb buffer — so vector width does not help.
Cost of finding out: one compiler flag.

### Panel runs at 52 Hz, CPC at 50 Hz

A 4% mismatch. One repeated/dropped frame every ~25 is imperceptible, and the
existing triple-buffer decoupling (plan 2026-06-21) already tolerates a display
clock that differs from the emulation clock. Note, not a blocker.

### Toolchain constraint: static linking is mandatory

Board glibc is **2.33** (Buildroot); the Debian trixie riscv64 container is
**2.41**. Dynamically linked binaries fail with
`version 'GLIBC_2.34' not found`. Either build `-static` (works, `libdrm.a` is
packaged) or build against a sysroot copied from the board. **For konCePCja,
plan a static build** — SDL is already static in this project.

### Also observed: the Wi-Fi driver oopses (not ours)

`dmesg` shows a repeating kernel oops in the RTL8189FS driver
(`rtw_lps_state_chk` -> `SetHwReg` -> `LPS_Leave` -> `rtw_set_ps_mode`), i.e.
Wi-Fi power-save handling. The board keeps running. Disabling Wi-Fi power save
would likely avoid it. Unrelated to this port, but relevant if Wi-Fi drops.

### Risk table

| # | Risk | Status |
| --- | --- | --- |
| 1 | No Vulkan / SDL_GPU | Retired |
| 2 | Display path | **Retired** — `drm_direct` proven on hardware |
| 3 | Presentation too slow | **Retired** — 2.26 ms/frame, 11.3% of budget |
| 4 | ImGui has no present path without SDL video | **Top risk**, untouched |
| 5 | QEMU build times | Retired |
| 6 | `char` unsigned | Retired |
| 7 | Keyboard not evdev | Open — userspace TCA8418 scan or uinput bridge |
| 8 | **glibc 2.33 vs 2.41** | **New, solved** — build static |

### Next

Only Risk #4 and #7 remain. Implement `drm_direct` as a real `video_plugin`
(the probe is the reference), then decide the ImGui story: emulator-only output
first, finish the P1.5.2 headless split, or write a minimal ImGui renderer into
the dumb buffer.

---

## Option 4: keep SDL, present its window surface to DRM — the full-UI route

Goal is the **full ImGui UI**, so the "emulator-only" and "headless split"
options are out (both deliver a UI-less emulator; the split is ~100-200 lines
because `imgui_state.h` is already ImGui-free and only `kon_cpc_ja.cpp` and
`video_host.cpp` include the 76-line `imgui_ui.h`, but cheapness buys nothing
we want). A software ImGui rasteriser is 600-1200 lines plus real perf risk on
one core.

**Option 4 gets full-UI capability at roughly headless-split effort:**

```
SDL_VIDEODRIVER=offscreen + SDL software renderer
  -> imgui_impl_sdlrenderer3 works unchanged
  -> all 8 existing sdlr_* video plugins work unchanged
  -> read SDL_GetWindowSurface() pixels
  -> blit into the DRM dumb buffer, page-flip   (~100 lines, drm_probe.c is the reference)
```

`drm_direct` stops being a new `video_plugin` and becomes a **presenter**.

### Corrections to earlier entries

* **The SDL software renderer IS in our build.** An earlier entry said
  `SDL_VIDEO_RENDER_SW` was `#undef` — that was read from the generated
  `SDL_build_config.h`, which is the wrong file. `src/SDL_internal.h:190` does
  `#if !defined(SDL_VIDEO_RENDER_SW) && !defined(SDL_LEAN_AND_MEAN)` ->
  `#define SDL_VIDEO_RENDER_SW 1`. Neither `SDL_LEAN_AND_MEAN` nor
  `SDL_RENDER_DISABLED` is set, so `SW_RenderDriver` is registered. There is no
  `SDL_RENDER_SW` CMake option because it needs none.
* The `offscreen` driver implements `SDL_OFFSCREEN_CreateWindowFramebuffer`, so
  `SDL_GetWindowSurface()` yields real CPU-readable pixels — the hook KMSDRM
  lacks.

### Evidence so far (riscv64 container)

With `SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=software -O video.scr_style=11`:

* **`mode: gui`** in the startup manifest — **no headless fallback**, so ImGui
  is initialised. (CLAUDE.md warns `SDL_VIDEODRIVER=dummy` falls back to
  headless on macOS because GL init fails; forcing the software renderer avoids
  that.)
* 51 FPS / 102% speed, `render-wait 0.0 ms/f`, no GPU errors.
* `screenshot window` over IPC produced a **correct CPC 6128 boot screen**,
  768x540 RGBA — proof the emulator's pixel path is right on riscv64.

### What is still NOT proven

**That ImGui's draw data rasterises to readable pixels.** The capture at
`video_host.cpp:459` saves `vid` (the CPC surface) from inside the flip handler,
not the composited window — two captures taken before and after issuing
`devtools` were byte-identical. So this test *cannot* show chrome either way;
it is a limitation of the probe, not evidence against option 4.

### Next step

Read `SDL_GetWindowSurface()` directly rather than going through the screenshot
path — which is exactly what the DRM presenter must do anyway. Build konCePCja
static for the board, add the ~100-line presenter, run it on the panel, and
look. That is the real integration and it answers the question directly.

---

## Operational notes for working on this board

### The Wi-Fi link dies on its own (RTL8189FS LPS oops)

The board dropped off the network mid-session and did not return; only a
power-cycle recovered it. `dmesg` carries a repeating kernel oops in the
RTL8189FS power-save teardown:

```
rtw_lps_state_chk -> SetHwReg -> SetHwReg8188F -> SetHwReg8188FS
  -> rtw_hal_set_hwreg -> rtw_set_ps_mode -> LPS_Leave -> lps_ctrl_wk_hdl
```

`/proc/net/rtl8189fs/wlan0/ps_info` shows `LPS mode: MAX` with non-zero
`LPS enter/leave count`, i.e. the faulting path is being exercised routinely.

**`rtw_power_mgnt=0` does not fix a live session.** Writing
`/sys/module/8189fs/parameters/rtw_power_mgnt` and `rtw_ips_mode` succeeds, but
the adapter read those at init — `ps_info` still reports `LPS mode: MAX`
afterwards. They only take effect on module re-init, which drops the link.

**Practical mitigation: never let the link go idle.** LPS engages on idle, so a
few-second ping keeps it out of power-save. `tools/riscv64/keepalive.sh` does
this and reports the moment the board becomes unreachable, so a lost link is
never mistaken for a slow command.

**Consequence for risky operations:** do not rewrite the partition table of the
**mounted** rootfs over SSH. A disconnect between `fdisk` writing the extent and
`resize2fs` finishing corrupts the card. Do the resize offline with the card in
a host reader (nothing mounted, no link to lose, and the verified
`sysimage-sdcard.img` is the fallback).

### Rootfs is small: 600 MB partition, ~133 MB free

The image claims only 728 MB of the 32 GB card. To grow: keep p2's start at
sector 262144 and extend its size from 1228800 to 33554432 sectors (16 GiB),
then `resize2fs`. Check `resize2fs` exists first — Buildroot often omits it.

### Board needs the power key held after every power-cycle

USB power alone leaves the SoC in deep sleep with a black screen. Hold the PMU
power key ~2-3 s. BOOT0 does not do this.

### Two build-script mistakes worth not repeating

1. **Never set `CMAKE_FIND_LIBRARY_SUFFIXES=".a"`** to force a static build. It
   makes every CMake `TryCompile` probe perform a full static link; under QEMU
   the configure phase then runs longer than the entire build (19 minutes in,
   one object file). `gcc -static` already prefers `.a` archives.
2. **Do not `sed`-delete lines from a shell script that uses line
   continuations.** Removing the last `-D...` flag left a dangling `\` that
   swallowed the following `cmake --build` line into the configure command —
   valid shell, wrong program, and `sh -n` passes. Rewrite such scripts whole.

### Building the LilyGO BSP needs a case-sensitive filesystem

Cost 2h47m of build time to learn, and the error names an unrelated package:

```
>>> ncurses 6.4-20230603 Installing to target
install: cannot stat '.../sysroot/usr/share/terminfo/a/ansi': No such file or directory
make[2]: *** [package/pkg-generic.mk:368: .../ncurses.../.stamp_target_installed] Error 1
```

terminfo keys entries by first letter and some names are capitalised (`E/Eterm`),
so on a case-insensitive filesystem `E/` and `e/` collide. ncurses' configure
detects this and stores entries under hex character codes instead — staging ends
up with `6a/`, `4c/`, `6f/` rather than `a/`, `L/`, `o/` — while Buildroot's
target-install step still asks for the letter path. Nothing is wrong with the
tree or the patches.

APFS on this machine is case-insensitive and a Docker bind mount inherits that,
so the build tree now lives on a case-sensitive APFS sparsebundle inside the
workspace (`k230build.sparsebundle`, attached at `/Volumes/k230build`). Sparse,
so it costs only what the build uses, and `hdiutil detach` + `rm -rf` still
purges everything. Verify it rather than assuming — on the host *and* through
Docker, since only the latter is what Buildroot sees:

```
mkdir A && mkdir a && ls -d ?     # must list both
```

The toolchain stays on the plain volume: read-only input, all-lowercase names.

Third build-script mistake worth not repeating, in the same family as the two
above: the container ran `./build_sdcard_image.sh > log 2>&1` and then `echo
"exit=$?" >> log`, so `docker run` returned **0** for a build that had failed
with 2. The background task reported success. Always propagate the real code
(`rc=$?; ...; exit $rc`) — and read the recorded value, not the wrapper's.
