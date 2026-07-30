#pragma once
#include <string>
#include <vector>

struct ConfigProfile {
  unsigned int model = 2;
  unsigned int ram_size = 128;
  unsigned int speed = 4;
  unsigned int frameskip = 0;
  unsigned int scr_scale = 2;
  unsigned int scr_scanlines = 0;
  unsigned int snd_enabled = 1;
  unsigned int snd_playback_rate = 2;
  unsigned int snd_bits = 1;
  unsigned int snd_stereo = 1;
  unsigned int snd_volume = 80;
  unsigned int joystick_emulation = 0;
  unsigned int keyboard_support_mode = 0;
};

class ConfigProfileManager {
 public:
  // Set the directory where profiles are stored (for testing)
  void set_profile_dir(const std::string& dir);

  std::vector<std::string> list() const;
  std::string current() const;
  std::string load(const std::string& name);
  std::string save(const std::string& name);
  std::string remove(const std::string& name);

  // For testing: load/save without touching the global CPC struct
  static std::string write_profile(const std::string& path,
                                   const ConfigProfile& p);
  static std::string read_profile(const std::string& path, ConfigProfile& p);

  // The settings for a named built-in machine. Public because it is a pure
  // value producer and the model numbers it hands out are worth pinning in a
  // test: "6128plus" shipped p.model = 4, which is not a valid model at all
  // (the range is 0..3), so it read past chROMFile[4] and left the ASIC off.
  static ConfigProfile builtin_profile(const std::string& name);

  // Clamp every field into the range the emulator actually accepts, matching
  // the read_clamped() bounds the main config path applies in
  // loadConfiguration(). A .kpf is user-editable and load() writes straight
  // into the global CPC struct, so unvalidated values would otherwise reach
  // array indices and RAM sizing directly.
  static void sanitize(ConfigProfile& p);

 private:
  std::string profile_dir() const;
  std::string profile_path(const std::string& name) const;
  bool is_builtin(const std::string& name) const;
  static bool valid_name(const std::string& name);

  std::string profile_dir_;
  std::string current_name_;
};

extern ConfigProfileManager g_profile_manager;
