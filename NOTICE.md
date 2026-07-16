# NOTICE — konCePCja provenance & third-party attributions

## Provenance

konCePCja is a clean, hardware-inspired emulator of the Amstrad CPC — modelled
from documented silicon behaviour and hardware specifications
(`docs/hardware/`), device by device, not derived from other emulators.

The project began, years ago, as a fork of **Caprice32** (© the Caprice32
authors, GPLv2). Over the course of development every inherited Caprice32 file
was superseded by a clean-room implementation authored here and then deleted —
a process tracked in full in [`docs/replacement-ledger.md`](docs/replacement-ledger.md)
("the replacement ledger"). No file inherited from Caprice32 survives in this
source tree. That completed cutover is what allows this project to be offered
under the [konCePCja Source License 1.0.0](LICENSE.md) — a source-available
license (based on PolyForm Internal Use 1.0.0 with an added Personal & Hobby Use
grant) — rather than the GPLv2 of its origin.

The full development history — including the fork era — is preserved
unrewritten in the provenance repository,
[ikari-pl/koncepcja_v5](https://github.com/ikari-pl/koncepcja_v5): it records
the true authorship and dates of every step of the construction.

## Third-party components (vendored, under their own licenses)

These are declared dependencies — relocated verbatim under `vendor/`, not
authored here, and not covered by this project's source license. Each is used
under its own upstream license:

| Component | Path | Upstream | License |
|---|---|---|---|
| Dear ImGui | `vendor/imgui/` | https://github.com/ocornut/imgui | MIT |
| SDL3 | `vendor/SDL/` | https://github.com/libsdl-org/SDL | zlib |
| msf_gif | `vendor/msf_gif/` | https://github.com/notnullnotvoid/msf_gif | MIT OR Unlicense |
| ImGuiColorTextEdit | `vendor/ImGuiColorTextEdit/` | https://github.com/santaclose/ImGuiColorTextEdit (fork of BalazsJako/ImGuiColorTextEdit) | MIT |
| portable-file-dialogs | `vendor/portable-file-dialogs/` | https://github.com/samhocevar/portable-file-dialogs | WTFPL |

See [`vendor/README.md`](vendor/README.md) for details and conventions.

All vendored components above are permissively licensed (MIT / zlib / WTFPL /
Unlicense) and compatible with redistribution. The project carries **no**
copyleft (GPL) or redistribution-restricted third-party code: IPF flux support
is a clean-room decoder (`src/ipf_decode`), and the 2x pixel-scaling kernels
(`src/scalers`) were re-authored clean-room from public algorithm descriptions —
neither derives from the GPL/non-commercial libraries that once provided them.

## System libraries (dynamically linked at build time)

Not bundled; provided by the host/toolchain and used under their own licenses:
libpng, zlib, FreeType, libjpeg-turbo, libcurl.

## Amstrad ROMs

The ROM images in `rom/` are © Amstrad plc and Locomotive Software Ltd,
redistributed with permission and retained under their own terms — **not**
covered by this project's source license. See
[`rom/ROM-LICENSE.txt`](rom/ROM-LICENSE.txt).
