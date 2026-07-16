// vjoystick_map.h - Pure (ImGui/SDL-free) mapping for the virtual joystick and
// the gamepad/joystick input path.  Given a target CPC joystick (0 or 1) and a
// bitmask of the directions/fires currently held, it returns the list of
// CPC_J{n}_* keys that must be pressed into keyboard-matrix row 9 (joy 0) or
// row 6 (joy 1).  Kept header-only and dependency-light so it can be unit tested
// without ImGui or a live emulator (see test/vjoystick_map.cpp).
#pragma once

#include <array>
#include <cstdint>

#include "keyboard.h"  // CPC_KEYS

namespace vjoy {

// Bitmask of virtual-joystick controls currently held.  Diagonals are simply
// two direction bits set at once (e.g. VJOY_UP | VJOY_RIGHT).
enum VJoyButton : std::uint8_t {
  VJOY_UP = 1u << 0,
  VJOY_DOWN = 1u << 1,
  VJOY_LEFT = 1u << 2,
  VJOY_RIGHT = 1u << 3,
  VJOY_FIRE1 = 1u << 4,
  VJOY_FIRE2 = 1u << 5,
  VJOY_ALL = VJOY_UP | VJOY_DOWN | VJOY_LEFT | VJOY_RIGHT | VJOY_FIRE1 |
             VJOY_FIRE2,
};

// Up to six CPC_KEYS to hold for the current frame.  Fixed-size array, no heap.
struct VJoyKeys {
  std::array<CPC_KEYS, 6> keys{};
  int count = 0;
};

// Pure: map (target joystick, held-button mask) -> the CPC_J{n}_* keys to hold.
// target_joy != 1 selects joystick 0 (CPC_J0_*); target_joy == 1 selects
// joystick 1 (CPC_J1_*).  A diagonal naturally yields two direction keys.
inline VJoyKeys vjoy_active_keys(int target_joy, unsigned mask) {
  const bool j1 = (target_joy == 1);
  VJoyKeys out;
  auto push = [&](CPC_KEYS k0, CPC_KEYS k1) {
    out.keys[out.count++] = j1 ? k1 : k0;
  };
  if (mask & VJOY_UP) push(CPC_J0_UP, CPC_J1_UP);
  if (mask & VJOY_DOWN) push(CPC_J0_DOWN, CPC_J1_DOWN);
  if (mask & VJOY_LEFT) push(CPC_J0_LEFT, CPC_J1_LEFT);
  if (mask & VJOY_RIGHT) push(CPC_J0_RIGHT, CPC_J1_RIGHT);
  if (mask & VJOY_FIRE1) push(CPC_J0_FIRE1, CPC_J1_FIRE1);
  if (mask & VJOY_FIRE2) push(CPC_J0_FIRE2, CPC_J1_FIRE2);
  return out;
}

// All six CPC_J{n}_* keys for a target — used to release every bit when the
// window closes or a device is unplugged.
inline VJoyKeys vjoy_all_keys(int target_joy) {
  return vjoy_active_keys(target_joy, VJOY_ALL);
}

}  // namespace vjoy
