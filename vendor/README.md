# Vendored dependencies

konCePCja's doctrine (`docs/replacement-ledger.md`) draws a hard line between
Caprice32 inheritance — which gets replaced and deleted — and genuine
third-party libraries, which are **declared dependencies, not authorship
debt**. The latter live here, verbatim, with their upstream provenance
intact. They are relocated, not rewritten.

| Component | Path | Upstream | License | Why it's here |
|---|---|---|---|---|
| Dear ImGui | `vendor/imgui/` | https://github.com/ocornut/imgui | MIT | Immediate-mode GUI toolkit backing the whole ImGui UI + DevTools surface |
| SDL3 | `vendor/SDL/` | https://github.com/libsdl-org/SDL | zlib | Windowing/input/audio/GPU backend (git submodule; local patches recorded under `vendor/sdl-patches/`) |
| msf_gif | `vendor/msf_gif/` | https://github.com/notnullnotvoid/msf_gif | MIT OR Unlicense | Single-header GIF encoder used by the session GIF recorder (`src/gif_recorder.cpp`) |
| ImGuiColorTextEdit | `vendor/ImGuiColorTextEdit/` | https://github.com/santaclose/ImGuiColorTextEdit (fork of https://github.com/BalazsJako/ImGuiColorTextEdit) | MIT | Colorizing text editor widget backing the DevTools Z80 assembler editor (`src/devtools_ui.cpp`); konCePCja adds a `Z80Assembly()` language definition downstream in `LanguageDefinitions.cpp`, following the file's own pattern |
| portable-file-dialogs | `vendor/portable-file-dialogs/` | https://github.com/samhocevar/portable-file-dialogs | WTFPL | Native OS file-open/save dialogs used by the DevTools menus (`src/devtools_ui.cpp`, `src/kon_cpc_ja.cpp`) |

## What does NOT belong here

The SPS Decoder Library (capsimg), once vendored under `src/capsimg/` for
IPF flux decoding, is **gone**: its non-commercial license was incompatible
with redistribution, so it was replaced by the clean-room decoder in
`src/ipf_decode.{h,cpp}` (built solely from `docs/hardware/ipf-format.md`)
and deleted — see the `ipf.{cpp,h}` row in the replacement ledger and
`NOTICE.md`.

`src/argparse.{cpp,h}` is **not** a third-party library despite the name and
despite being listed as one in an earlier pass of the replacement ledger.
Investigation during the 2026-07-13 vendor relocation sweep found it to be
bespoke command-line parsing code — first authored directly in this
repository on 2017-03-06/07 by a Caprice32-era contributor, built on the
standard POSIX `getopt_long()`, and deeply coupled to project-specific types
(`CapriceArgs`, `KONCPC_KEYS`, `video_plugin_list`). There is no upstream
project it was copied from, no separate license, no attribution to lose —
moving it here under a fabricated provenance would misrepresent authorship.
It stays in `src/` and is tracked under the replacement ledger's §2 (host
layer, Caprice32-era code awaiting re-authoring), not as a vendored
dependency.

## Conventions

- Vendored files keep their original logic untouched; only a short
  attribution/SPDX header is added where the file didn't already carry one.
- `#include` paths from consumers were kept stable (e.g. `#include
  "msf_gif.h"`, `#include "TextEditor.h"`) by adding the vendor subdirectory
  to the compiler's include path (`-isystem` in the makefile,
  `target_include_directories` in `CMakeLists.txt`) rather than rewriting
  every include string across the tree.
- `makefile`'s `VENDORED_EXCLUDE` keeps vendored `.cpp`/`.h` files out of
  `clang-format`/`clang-tidy` sweeps; most of them are excluded automatically
  now simply by living outside `$(SRCDIR)`.
