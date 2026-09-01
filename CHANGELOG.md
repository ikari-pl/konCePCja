# Changelog

## [6.2.4](https://github.com/ikari-pl/konCePCja/compare/v6.2.3...v6.2.4) (2026-08-31)


### Bug Fixes

* Windows DPI scaling, config safety, and UI fixes ([#39](https://github.com/ikari-pl/konCePCja/issues/39)) ([e9fe883](https://github.com/ikari-pl/konCePCja/commit/e9fe88385c9c57258693bba4af4c9d29e387e8b4))

## [6.2.3](https://github.com/ikari-pl/konCePCja/compare/v6.2.2...v6.2.3) (2026-08-29)


### Bug Fixes

* **config:** a -O override is one-run intent — never persisted by its own echo ([557c497](https://github.com/ikari-pl/konCePCja/commit/557c49789967628fd7812bd2bd42f63d668d5e14))
* **debug:** conditional breakpoints and watchpoint hits must not lie ([0894b06](https://github.com/ikari-pl/konCePCja/commit/0894b0693ea8e46f1c9ba818e83ed679051a2de6))
* **debug:** harden step-out exit paths and SP wrap ([dbd2356](https://github.com/ikari-pl/konCePCja/commit/dbd2356f1ea195512fb3fd9f6b1dfb26c39241fa))
* **debug:** make `step out` actually stop on the subcycle engine ([d69b512](https://github.com/ikari-pl/konCePCja/commit/d69b51295f99d00caaa64a0bafc7ef698f04df68))
* **debug:** make `step out` actually stop on the subcycle engine ([9aedf4d](https://github.com/ikari-pl/konCePCja/commit/9aedf4dd4943c2f0973412b02bde6bfe67d9beb8))
* **debug:** make the debugger and the restart path tell the truth ([a2749e8](https://github.com/ikari-pl/konCePCja/commit/a2749e87adca6a845c218bc5330b864761b81063))
* **debug:** make the debugger and the restart path tell the truth ([f2b66b3](https://github.com/ikari-pl/konCePCja/commit/f2b66b3c645862fdaee9b2944a33f9a1a531bdf5))
* **debug:** review follow-ups — logical not, PC restore, one function authority ([dd3c5c8](https://github.com/ikari-pl/konCePCja/commit/dd3c5c817b20ca19474d698ea577554b4881914d))
* **debug:** the condition language and probe post-filters must not lie ([fa704f6](https://github.com/ikari-pl/konCePCja/commit/fa704f6ccee6fe73615c12c99def82abf2175d5d))
* **m4:** actually fit the M4 board — the ROM lookup had drifted apart ([0ede93e](https://github.com/ikari-pl/konCePCja/commit/0ede93ebbce9215d4c76f878481bbb8d2f5be262))
* **m4:** actually fit the M4 board — the ROM lookup had drifted apart ([9725f73](https://github.com/ikari-pl/konCePCja/commit/9725f73ebab1ba7bb6c672409e889a213581f78f))
* **m4:** answer commands at coprocessor latency, and blank the whole window while busy ([79ebf64](https://github.com/ikari-pl/konCePCja/commit/79ebf6457cc0b38ed832b2aa36b8ead8ab6ffca0))
* **m4:** fit the host-prepared ROM image, not a fresh read of the file ([d9242e2](https://github.com/ikari-pl/konCePCja/commit/d9242e29f1e0d0ef34ca7f7f13fd6150cb263653))
* **m4:** never splice a dropped command frame onto the next one ([1a32ef2](https://github.com/ikari-pl/konCePCja/commit/1a32ef2fd0c5e96dc52adf69db8b225da059f6f4))
* **review:** address the code review — stale flags, wrong thread, lost audio ([f14a126](https://github.com/ikari-pl/konCePCja/commit/f14a126af204560e6ea6eaf98f7dad5146ab19c5))
* **review:** harden config apply and wait-bp generation edges ([89af327](https://github.com/ikari-pl/konCePCja/commit/89af327c36f6fba6223d01329ca92f14b2934d01))
* **review:** logical not, PC restore on filter-refuse, help/docs parity ([b4eb9da](https://github.com/ikari-pl/konCePCja/commit/b4eb9da313e8747f99eb8e775fae411f47c42932))

## [6.2.2](https://github.com/ikari-pl/konCePCja/compare/v6.2.1...v6.2.2) (2026-08-02)


### Bug Fixes

* **machine:** one guarded path for rebuilding the emulated machine ([4001747](https://github.com/ikari-pl/konCePCja/commit/4001747db09842bcef03b24774e346a779de3632))
* **review:** correct the archive advance, the reset guard's layer, and the stranded confirmation ([7b13f1f](https://github.com/ikari-pl/konCePCja/commit/7b13f1f472ac5c69d973d689cd3b00b953261197))
* **ui,config:** F-cluster confirmations and reachability, plus an example config ([59dbf35](https://github.com/ikari-pl/konCePCja/commit/59dbf352ef6439fb6bb19b1ce4419277f56aaada))
* **ui,docs:** guard the Options restart, and stop the docs misdirecting ([4dc50f9](https://github.com/ikari-pl/konCePCja/commit/4dc50f9d5cd2e0ef5fa80f4168001085f04e506a))
* **ui,docs:** guard the Options restart, and stop the docs misdirecting ([ef917e1](https://github.com/ikari-pl/konCePCja/commit/ef917e1060d73d9d9d3347f5757afe52e0652354))
* **ui:** confirmations, keyboard and feedback in the F-cluster ([23b83eb](https://github.com/ikari-pl/konCePCja/commit/23b83eb1dd55e63c07090c601f02a6cf4ec370fc))
* **ui:** eject from the menus, and confirm a reset that would lose disk edits ([55889da](https://github.com/ikari-pl/konCePCja/commit/55889da6708e199c47e816cc54e8c041bd2a4a83))

## [6.2.1](https://github.com/ikari-pl/konCePCja/compare/v6.2.0...v6.2.1) (2026-07-31)


### Bug Fixes

* **audio:** release the tape line I/O before SDL_Quit ([97ac97b](https://github.com/ikari-pl/konCePCja/commit/97ac97b40726594099bb57bde8fa1a1a8b6c63cc))
* **audio:** release the tape line I/O before SDL_Quit ([7b95e95](https://github.com/ikari-pl/konCePCja/commit/7b95e953bad025ad46a876bfd83941e7c5e1df1f))
* **build:** stop the binary reporting the wrong commit ([d67129a](https://github.com/ikari-pl/konCePCja/commit/d67129abb31971e00be6a590e73f789728091776))
* expansion ROMs, the configured RAM size, and an honest build hash ([80d202c](https://github.com/ikari-pl/konCePCja/commit/80d202cafafb77462e7c0ef167c4be3bc7256593))
* **ram:** fit the RAM size the machine was configured for ([75470fc](https://github.com/ikari-pl/konCePCja/commit/75470fc97766d462754609389ebae1ac0647bab9))
* **rom:** fit expansion ROM slots in the CPC ([b9fbf16](https://github.com/ikari-pl/konCePCja/commit/b9fbf167f4b8d8097d1353cec2f163a5b0ea3169))
* **rom:** fit only the ROM slots the user asked for ([699faba](https://github.com/ikari-pl/konCePCja/commit/699faba39ca27d96c4f5e5a9998bdb339fc5882a))
* **rom:** repoint the host's paging when a fitted ROM is replaced ([9816b4f](https://github.com/ikari-pl/konCePCja/commit/9816b4f3acca608d1de0b9ea21cbb09a497e4190))
* **ui:** a dropped file is never silently ignored ([58dd71a](https://github.com/ikari-pl/konCePCja/commit/58dd71a7fa02636c7217d92a9fcf2cb07fb82917))
* **ui:** a dropped file is never silently ignored ([23aae2f](https://github.com/ikari-pl/konCePCja/commit/23aae2fb03cfbb167a28514d2b1e3b10255f3fe8))
* **ui:** read the drive's medium, not its sector view ([c3c1b7a](https://github.com/ikari-pl/konCePCja/commit/c3c1b7af0f913644f991e693ae09326d7bea6b47))
* **ui:** read the drive's medium, not its sector view ([4d483ce](https://github.com/ikari-pl/konCePCja/commit/4d483ceb035e12e9d1ef99f8f5b10f4aa79b0be8))

## [6.2.0](https://github.com/ikari-pl/konCePCja/compare/v6.1.1...v6.2.0) (2026-07-30)


### Features

* **dnd:** route flux disk images (.hfe/.scp/.a2r) through drag & drop ([bcc62ad](https://github.com/ikari-pl/konCePCja/commit/bcc62ade9db2276e11b731fcd64a91162909e3ce))
* **ipc:** accept flux disk images in `load`, and .ipf/.raw too ([b4e1d2e](https://github.com/ikari-pl/konCePCja/commit/b4e1d2e43a30328ba0c2095be9e6009c8455dc46))
* route flux disk images (.hfe/.scp/.a2r) through every front door ([63db699](https://github.com/ikari-pl/konCePCja/commit/63db699a330df60aaefbd4dd65042556fbc8d3ea))
* **site:** add per-platform downloads and current version to the homepage ([3aa43df](https://github.com/ikari-pl/konCePCja/commit/3aa43df2abce1288435708aff5214c85b17fa53a))
* **site:** add per-platform downloads and the current version to the homepage ([2056d59](https://github.com/ikari-pl/konCePCja/commit/2056d594fe8b03c5bbaf5cc45d8c97164914eeac))
* **ui:** show flux disk images in the drive-A open dialogs ([9866576](https://github.com/ikari-pl/konCePCja/commit/9866576544d56d0fad69af33d05a6c013cf45157))


### Bug Fixes

* **audio:** stop persisting forced sound-off and wire volume/enable at runtime ([322994d](https://github.com/ikari-pl/konCePCja/commit/322994d0bb9450587aa50a487a652ed049c1acce))
* close the review follow-ups from PR [#16](https://github.com/ikari-pl/konCePCja/issues/16) ([e05386d](https://github.com/ikari-pl/konCePCja/commit/e05386dd5ee409f5065a7332e4fdf316b82eafcf))
* close the review follow-ups from PR [#16](https://github.com/ikari-pl/konCePCja/issues/16) (beads-5a8n, wxy6, rbtp) ([ad039ff](https://github.com/ikari-pl/konCePCja/commit/ad039ffb16bc1fa6dc17b10cc7ccac4b71a92508))
* config truncation, silent audio, and the flat PSG scope ([66abcbc](https://github.com/ikari-pl/konCePCja/commit/66abcbce41aab54819aa0f3284659b5d9b7c6a02))
* **config:** save the config file by editing it, not by replacing it ([2f66887](https://github.com/ikari-pl/konCePCja/commit/2f66887b241b968987b6fbe8d1993789edf2cbc0))
* **devtools:** PSG oscilloscope had no writer — waveforms were always flat ([97c90c1](https://github.com/ikari-pl/konCePCja/commit/97c90c1d335b8c5b51f596d7b745563168da02f9))
* **video:** quiesce the Z80 thread before tearing down video on fullscreen toggle ([f39051a](https://github.com/ikari-pl/konCePCja/commit/f39051a7c1b2dcef75b0a3151c6b5cbe0cccd92e))
* **video:** quiesce the Z80 thread before tearing down video on fullscreen toggle ([9c2b455](https://github.com/ikari-pl/konCePCja/commit/9c2b45586d10e064bbb8c6485fdf2eee6799ca24))


### Performance

* **site:** cache release metadata in sessionStorage to reduce API calls ([36042e5](https://github.com/ikari-pl/konCePCja/commit/36042e55464e09ceeb26782bb2f98d2e89f97fa8))

## [6.1.1](https://github.com/ikari-pl/konCePCja/compare/v6.1.0...v6.1.1) (2026-07-30)


### Bug Fixes

* **devtools:** symbol-table crash and ROM-overlay memory flicker ([d714a31](https://github.com/ikari-pl/konCePCja/commit/d714a31626bb893a7bd64218852141b19b20b62a))
* **devtools:** symbol-table crash and ROM-overlay memory flicker ([47cf19e](https://github.com/ikari-pl/konCePCja/commit/47cf19e584934174f44fe4bcb9ed02a2969ef507))
* **profiles:** 6128plus profile selected model 4, which is not a machine ([80e9841](https://github.com/ikari-pl/konCePCja/commit/80e9841b956c56d5406bbda8020971fb1ec811f5))
* **profiles:** 6128plus profile selected model 4, which is not a machine ([8c1e1bb](https://github.com/ikari-pl/konCePCja/commit/8c1e1bb4193dd4dbf4c9298e2934a2c30193c516))

## [6.1.0](https://github.com/ikari-pl/konCePCja/compare/v6.0.1...v6.1.0) (2026-07-25)


### Features

* **ipc:** human-readable 'env' (CP/M vs BASIC) hint in the status bracket ([694bccc](https://github.com/ikari-pl/konCePCja/commit/694bccc5d4378538c61610e490a93e08a514b76f))
* **ipc:** human-readable 'env' (CP/M vs BASIC) hint in the status bracket ([e4ccd31](https://github.com/ikari-pl/konCePCja/commit/e4ccd313a709e8a27db96252eef849dfcba3e127))
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
