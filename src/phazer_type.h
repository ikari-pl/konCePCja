/* phazer_type.h — which light-gun peripheral is plugged into the joystick
 * port (config surface). The gun itself is emulated by the sub-cycle
 * light-gun Device (src/hw/light_gun); this type only names the selection
 * for the config store, the menus and the host input mapping. */

#pragma once

#include <cstdint>
#include <string>

class PhazerType {
 public:
  enum Value : std::uint8_t {
    None = 0,
    AmstradMagnumPhaser = 1,
    TrojanLightPhazer = 2,
    LastPhazerType = 3,
  };
  PhazerType() = default;
  constexpr PhazerType(Value val) : value(val) {}

  std::string ToString() {
    switch (value) {
      case None:
        return "off";
      case AmstradMagnumPhaser:
        return "Amstrad Magnum Phaser";
      case TrojanLightPhazer:
        return "Trojan Light Phazer";
      default:
        return "Unimplemented";
    }
  }

  // Cycle through the guns (the F-key toggle order).
  PhazerType Next() {
    auto new_value = value + 1;
    if (new_value == LastPhazerType) return {None};
    return {Value(new_value)};
  }

  operator Value() const { return value; };

  // if (phazer_type) — "is any gun plugged?"
  operator bool() const { return value != None; };

 private:
  Value value;
};
