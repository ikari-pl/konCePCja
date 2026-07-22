# Changelog

## [6.1.0](https://github.com/ikari-pl/konCePCja/compare/v6.0.1...v6.1.0) (2026-07-22)


### Features

* **ipc:** input state readback — 'input state [row]' (Phase 5) ([fb879a3](https://github.com/ikari-pl/konCePCja/commit/fb879a3f1bebf806a71b8fb573818879522abb11))
* **ipc:** input state readback — 'input state [row]' (Phase 5) ([06b1ca4](https://github.com/ikari-pl/konCePCja/commit/06b1ca4395493cfcf94af625d64c442118f659a6))
* **ipc:** key modifiers + hold timing — 'input key [hold=N]' + 'input chord' (Phase 3) ([7460324](https://github.com/ikari-pl/konCePCja/commit/746032494ca1d82103e5d55a69bfd58b8ed463f8))
* **ipc:** key modifiers + hold timing — 'input key [hold=N]' + 'input chord' (Phase 3) ([d36f2b4](https://github.com/ikari-pl/konCePCja/commit/d36f2b40566e8e2a1355f2b8fb472d0111335979))
* **ipc:** light-gun input surface — 'input gun move/trigger' (Phase 2) ([8e545cb](https://github.com/ikari-pl/konCePCja/commit/8e545cba3d8ffd093d4c4ed06df9f4f6e6c2af79))
* **ipc:** light-gun input surface — 'input gun move/trigger' (Phase 2) ([e60e1a2](https://github.com/ikari-pl/konCePCja/commit/e60e1a27d86834653a7f4dd3a1f062a079847363))
* **ipc:** unify 'input type' onto AutoTypeQueue (Phase 4) ([4ff5c75](https://github.com/ikari-pl/konCePCja/commit/4ff5c7546a52aa539bfec4b9cba1f76ac9489cff))
* **ipc:** unify 'input type' onto AutoTypeQueue (Phase 4) ([d242e3e](https://github.com/ikari-pl/konCePCja/commit/d242e3efd8d5e2812e129719f7129753b0b69233))


### Bug Fixes

* **ipc:** 'input type' strips single quotes too (+ gtest) ([4f83f05](https://github.com/ikari-pl/konCePCja/commit/4f83f0520d9ce6aa9aa170488907e8a8b337bf29))
* **ipc:** guard key/joy injection against null CPC.InputMapper ([31c4633](https://github.com/ikari-pl/konCePCja/commit/31c4633d60e6261782275925191d0c41b4ac0aed))
* **ipc:** guard key/joy injection against null CPC.InputMapper (beads-p95s) ([3f04ae8](https://github.com/ikari-pl/konCePCja/commit/3f04ae80f3e24047a541948c5495e7f00bd249d1))
* **ipc:** gun input robustness — config clamp, optional coords, null/UB guards ([8807421](https://github.com/ikari-pl/konCePCja/commit/8807421c8bc5c3850ae3ee404fd21705645dcb64))
* **ipc:** input state — defer name map to non-null InputMapper; buf 256 + 503 ([7605809](https://github.com/ikari-pl/konCePCja/commit/76058096d4d2842fc2c49a680b4b29e234bed268))
* **ipc:** tap_scancode blocks on wait_frame_step_done, not a busy-wait ([f28bdb6](https://github.com/ikari-pl/konCePCja/commit/f28bdb63411c4cc9fcca8ea04f15028ac6cc949a))

## [6.0.1](https://github.com/ikari-pl/konCePCja/compare/v6.0.0...v6.0.1) (2026-07-19)


### Bug Fixes

* **light-gun:** tick the gun under Wake + Fast so the LPEN latch works ([7d9ff86](https://github.com/ikari-pl/konCePCja/commit/7d9ff86c776d53fc0ca8a582d7656e5a558e6c92))
* **light-gun:** tick the gun under Wake + Fast; ci(linux): resolve vendored SDL path ([3fe2dc3](https://github.com/ikari-pl/konCePCja/commit/3fe2dc35f62df3e0e42baac290c7f0e3cb446ad0))

## [6.0.0](https://github.com/ikari-pl/konCePCja/compare/v5.10.0...v6.0.0) (2026-07-16)


### Bug Fixes

* **ci:** portability bugs the rebuilt CI exposed on Linux/GCC and MSVC ([59ef5d4](https://github.com/ikari-pl/konCePCja/commit/59ef5d47ee61b932a793bd982e99a7ed2e53ca58))
* **ci:** the last std::memcmp-without-&lt;cstring&gt; (plotter test) — swept, none remain ([18802cc](https://github.com/ikari-pl/konCePCja/commit/18802cc3763f0e258dc2375ab6d1ee92b1807fac))
* **e2e:** engine=1 parity chain — zip attach, scan-gated autotype, printer port, WAITBREAK; drop the engine flag ([a172ea9](https://github.com/ikari-pl/konCePCja/commit/a172ea9c68f3cebe626b25b53e1ccda7df2f2559))
* **e2e:** engine=1 parity chain — zip attach, scan-gated autotype, printer port, WAITBREAK; drop the engine flag ([c6520f9](https://github.com/ikari-pl/konCePCja/commit/c6520f9bbc9e70cefc2f918189bb0a55a900f2d9))


### Miscellaneous

* release 6.0.0 ([6e9381d](https://github.com/ikari-pl/konCePCja/commit/6e9381d331518be0ef7332cbf60617f7f28687e8))
