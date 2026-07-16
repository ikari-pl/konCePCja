# Greaseweazle host protocol — summary notes

Working summary of the Greaseweazle USB CDC-ACM protocol, for the
[USB↔CPC floppy bridge](usb-fdd-bridge-plan.md) firmware and the emulator's
planned `gw_host` module. **Normative source**: `inc/cdc_acm_protocol.h` of
[github.com/keirf/greaseweazle-firmware](https://github.com/keirf/greaseweazle-firmware)
(Keir Fraser; "free and unencumbered software released into the public
domain" — the Unlicense; license verified via the GitHub license API for both
the firmware and host-tools repos, retrieved 2026-07). Flux-encoding details
cross-checked against `src/floppy.c` of the same repo. Anything ambiguous
here must be resolved against those files, not this summary.

Transport: USB CDC-ACM (a plain serial device). Commands are short binary
frames: `cmd, length, args...`; the device answers `cmd, ack` (ack = 0 →
okay), possibly followed by a data stream.

---

## 1. Command set

| Opcode | Command | Purpose (bridge-relevant notes) |
|---|---|---|
| 0 | `GET_INFO` | 32-byte info block: firmware version, hardware model/submodel, USB speed, **sample frequency** (the tick rate all flux values are expressed in), buffer sizes |
| 1 | `UPDATE` | Firmware update entry |
| 2 | `SEEK` | Seek to cylinder (signed; supports double-step etc. host-side) |
| 3 | `HEAD` | Select side 0/1 (no-op for single-head 3" mechs) |
| 4 | `SET_PARAMS` | Set `gw_delay` group: select/step/seek settle/motor-on/watchdog delays — how host tools tune SRT-class timing per drive |
| 5 | `GET_PARAMS` | Read the same |
| 6 | `MOTOR` | Motor on/off for a unit |
| 7 | `READ_FLUX` | Stream flux (§3): args = tick limit, max index pulses |
| 8 | `WRITE_FLUX` | Write a flux stream (index-cued or free); status via opcode 9 |
| 9 | `GET_FLUX_STATUS` | ACK of the last read/write stream operation |
| 11 | `SWITCH_FW_MODE` | Bootloader/normal |
| 12 | `SELECT` | Assert a drive select |
| 13 | `DESELECT` | Release select |
| 14 | `SET_BUS_TYPE` | 1 = IBM/PC, 2 = Shugart (**the CPC bus is Shugart**) |
| 15 | `SET_PIN` / 20 `GET_PIN` | Raw pin poke/peek — `GET_PIN 34` is the planned RDY disc-change poll |
| 16 | `RESET` | Reset device state (deselect, motor off, defaults) |
| 17 | `ERASE_FLUX` | Erase pass (write path, M4) |
| 18 | `SOURCE_BYTES` / 19 `SINK_BYTES` | Bandwidth self-test streams |
| 21 | `TEST_MODE` | Factory/board test |
| 22 | `NOCLICK_STEP` | Step outward at cylinder 0 without banging the stop |

## 2. Info structures

- `gw_info` (from `GET_INFO`): firmware major/minor, `hw_model`/`hw_submodel`
  (the Adafruit RP2040 implementation reports model 8 — see plan §3.2),
  `usb_speed`, `sample_freq`. The stock STM32F103 firmware samples at
  **72 MHz** (`SAMPLE_MHZ = 72` in `src/floppy.c`) → 13.9 ns ticks; hosts
  must always divide by the *reported* `sample_freq`, never assume it.
- `gw_drive_info`: flags (cylinder-valid, motor-on, flippy), current
  cylinder.
- `gw_delay`: select/step/seek-settle/motor/watchdog/write timings (µs/ms
  units — see header).

## 3. Flux stream wire format (READ_FLUX / WRITE_FLUX)

Verified against `rdata_encode_flux()` in `src/floppy.c` (ibid.):

- Interval values are in device sample ticks between successive flux
  transitions.
- **1 byte**: values 1..249 sent literally.
- **2 bytes**: values 250..1524 — first byte 250..254, second byte extends
  (`value = 250 + (b0 − 250) × 255 + (b1 − 1)`).
- **Escape `0xFF`** introduces an opcode:
  - `FLUXOP_INDEX` (1) + 28-bit value: an index pulse occurred this many
    ticks into the *current* interval — this is the per-revolution framing
    the SCP writer and `flux_decode_track_rev` consumers rely on.
  - `FLUXOP_SPACE` (2) + 28-bit value: advance the time cursor without a
    transition (long unformatted gaps; also used to express intervals
    ≥ 1525 ticks, followed by a small literal remainder).
  - `FLUXOP_ASTABLE` (3) + 28-bit period: regular transitions (write path).
- **28-bit values** are four bytes, each with bit 0 forced to 1 (so no byte
  is 0x00): `b0 = 1|(N<<1)`, `b1 = 1|(N>>6)`, `b2 = 1|(N>>13)`,
  `b3 = 1|(N>>20)`.
- A read stream ends with a terminating **0x00** byte; the host then issues
  `GET_FLUX_STATUS` for the definitive ACK.

Bandwidth consequence for CPC DD media: worst-case 4 µs intervals are 288
ticks at 72 MHz → 2 bytes each → ≤ 500 KB/s ≈ 4 Mbit/s, comfortably inside
USB Full Speed (the plan's §3.1 math).

## 4. ACK codes

| Code | Name | Bridge diagnosis |
|---|---|---|
| 0 | `ACK_OKAY` | — |
| 1 | `ACK_BAD_COMMAND` | protocol bug |
| 2 | `ACK_NO_INDEX` | no disc / dead spindle / **dead belt** (EME mechs — plan §10.1) |
| 3 | `ACK_NO_TRK0` | head stuck / stepper fault / cable |
| 4 | `ACK_FLUX_OVERFLOW` | host stalled the stream (should be unreachable at DD) |
| 5 | `ACK_FLUX_UNDERFLOW` | write stream starved (M4) |
| 6 | `ACK_WRPROT` | write-protected media / WP jumper open |
| 7 | `ACK_NO_UNIT` | select not configured |
| 8 | `ACK_NO_BUS` | `SET_BUS_TYPE` missing |
| 9 | `ACK_BAD_UNIT` / 10 `ACK_BAD_PIN` / 11 `ACK_BAD_CYLINDER` | argument errors |
| 12 | `ACK_OUT_OF_SRAM` / 13 `ACK_OUT_OF_FLASH` | resource exhaustion |

## 5. Canonical host-side sequences (what `gw_host` implements)

**Attach/probe**: open CDC → `RESET` → `GET_INFO` (note `sample_freq`) →
`SET_BUS_TYPE` Shugart → `SET_PARAMS` (conservative EME step/settle delays,
bench-tuned at M1).

**Fetch cylinder N (live passthrough)**: `SELECT unit` → `MOTOR on` →
`SEEK N` → `READ_FLUX {max_index = 3}` (2 full index-to-index revolutions) →
`GET_FLUX_STATUS` → split stream at `FLUXOP_INDEX` marks → rescale ticks to
the consumer's unit → hand to the FDC flux path (plan §7.2) → idle-timeout
worker eventually drops MOTOR/SELECT.

**Disc-change poll**: motor off, `GET_PIN 34` (RDY) at ~2 Hz; transition →
invalidate (plan §7.2); semantics on EME mechs needs-bench-confirmation
(plan §10 Q1).
