/* light_gun.h — the CPC light gun (Amstrad Magnum Phaser / Trojan Light
 * Phazer) as a Device. THE SPEC: docs/hardware/light-gun-device.md.
 *
 * A photo-sensor + trigger: when the beam sweeps under the aimed point and the
 * trigger is held, the gun pulses the 6845 LPEN pin (PenBus.strobe) and the
 * CRTC latches its refresh address into R16/R17. The gun holds its aim in CRTC
 * beam space (a displayed scanline + an active character column); the bridge
 * maps the host mouse into that space. */
#ifndef KONCPC_HW_LIGHT_GUN_H
#define KONCPC_HW_LIGHT_GUN_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LightGunRegs {
  uint8_t type;      /* 0=off, 1=Magnum Phaser, 2=Trojan Light Phazer */
  uint8_t pressed;   /* trigger held */
  uint16_t aim_line; /* aimed scanline (matched vs vid.frame_line) */
  uint16_t aim_col;  /* aimed active char column */
  uint8_t plugged;   /* == type != 0 */
} LightGunRegs;

size_t light_gun_state_size(void);
Device light_gun_init(void* storage);
void light_gun_peek(const Device* dev, LightGunRegs* out);

/* type 0 unplugs the gun (dormant in recompose_active). */
void light_gun_set_type(const Device* dev, int type);
void light_gun_set_aim(const Device* dev, uint16_t line, uint16_t col);
void light_gun_set_trigger(const Device* dev, int pressed);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_LIGHT_GUN_H */
