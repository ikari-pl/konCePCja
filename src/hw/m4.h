/* m4.h — the M4 Board as a Device. THE SPEC: docs/hardware/m4-device.md.
 * A command/response coprocessor on the expansion port: OUT &FE00 accumulates
 * command bytes, OUT &FCxx latches the frame for the host to execute, and the
 * result is read back through a RAM overlay on the M4 ROM's &E800 window. The
 * board's STM32 is genuinely asynchronous, so the Device latches + reports busy
 * and the host executes the filesystem/network work between frames (§3). */
#ifndef KONCPC_HW_M4_H
#define KONCPC_HW_M4_H

#include <stddef.h>
#include <stdint.h>

#include <cstdint>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

enum : std::uint16_t {
  M4_RESPONSE_SIZE = 0x600,
  M4_CONFIG_SIZE = 128,
  M4_FRAME_MAX = 4096
};

typedef struct M4Regs {
  uint8_t plugged;
  uint8_t slot;          /* upper-ROM slot the M4 ROM answers in (default 6) */
  uint8_t busy;          /* a command is latched, awaiting the host */
  uint16_t cmd_count;    /* accumulator length */
  uint16_t response_len; /* bytes in the response window */
  uint16_t last_cmd;     /* last latched 16-bit command */
} M4Regs;

/* The latched command frame the host drains and executes (spec §3). */
typedef struct M4Pending {
  uint16_t cmd;                /* buf[1] | buf[2]<<8 */
  uint16_t len;                /* frame length */
  uint8_t frame[M4_FRAME_MAX]; /* the accumulated bytes */
} M4Pending;

size_t m4_state_size(void);
Device m4_init(void* storage);
void m4_peek(const Device* dev, M4Regs* out);

/* The M4's 16K upper ROM (caller-owned live wiring, like every ROM). */
void m4_attach_rom(const Device* dev, const uint8_t* rom16k, size_t len);
void m4_set_slot(const Device* dev, int slot);
void m4_set_plugged(const Device* dev, int on);

/* Deferred bridge (spec §3): the ONLY path filesystem/network results enter.
 * m4_pending_command removes and returns the latched frame (0 if none);
 * m4_complete_response fills the &E800 window and clears busy;
 * m4_write_config fills the &F400 config overlay (C_CONFIG results). */
int m4_pending_command(const Device* dev, M4Pending* out);
void m4_complete_response(const Device* dev, const uint8_t* buf, uint16_t len);
void m4_write_config(const Device* dev, const uint8_t* buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_M4_H */
