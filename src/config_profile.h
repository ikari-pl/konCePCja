#pragma once
#include <string>
#include <vector>

struct ConfigProfile {
    unsigned int model = 2;
    unsigned int ram_size = 128;
    unsigned int speed = 4;
    unsigned int scr_scale = 2;
    unsigned int scr_scanlines = 0;
    unsigned int snd_enabled = 1;
    unsigned int snd_playback_rate = 2;
    unsigned int snd_bits = 1;
    unsigned int snd_stereo = 1;
    unsigned int snd_volume = 80;
    unsigned int joystick_emulation = 0;
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
    static std::string write_profile(const std::string& path, const ConfigProfile& p);
    static std::string read_profile(const std::string& path, ConfigProfile& p);

private:
    std::string profile_dir() const;
    std::string profile_path(const std::string& name) const;
    bool is_builtin(const std::string& name) const;
    static ConfigProfile builtin_profile(const std::string& name);
    static bool valid_name(const std::string& name);

    std::string profile_dir_;
    std::string current_name_;
};

extern ConfigProfileManager g_profile_manager;
