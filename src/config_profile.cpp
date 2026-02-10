#include "config_profile.h"
#include "koncepcja.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

extern t_CPC CPC;

ConfigProfileManager g_profile_manager;

static const std::vector<std::string> builtin_names = {
    "cpc464", "cpc664", "cpc6128", "6128plus"
};

void ConfigProfileManager::set_profile_dir(const std::string& dir) {
    profile_dir_ = dir;
}

std::string ConfigProfileManager::profile_dir() const {
    if (!profile_dir_.empty()) return profile_dir_;
    const char* home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (!home) return ".koncepcja/profiles";
    return (fs::path(home) / ".koncepcja" / "profiles").string();
}

std::string ConfigProfileManager::profile_path(const std::string& name) const {
    return (fs::path(profile_dir()) / (name + ".kpf")).string();
}

bool ConfigProfileManager::is_builtin(const std::string& name) const {
    return std::find(builtin_names.begin(), builtin_names.end(), name) != builtin_names.end();
}

bool ConfigProfileManager::valid_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    }
    return true;
}

ConfigProfile ConfigProfileManager::builtin_profile(const std::string& name) {
    ConfigProfile p;
    if (name == "cpc464") {
        p.model = 0; p.ram_size = 64;
    } else if (name == "cpc664") {
        p.model = 1; p.ram_size = 64;
    } else if (name == "cpc6128") {
        p.model = 2; p.ram_size = 128;
    } else if (name == "6128plus") {
        p.model = 4; p.ram_size = 128;
    }
    return p;
}

std::vector<std::string> ConfigProfileManager::list() const {
    std::vector<std::string> names = builtin_names;
    std::string dir = profile_dir();
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".kpf") {
                std::string n = entry.path().stem().string();
                if (std::find(names.begin(), names.end(), n) == names.end()) {
                    names.push_back(n);
                }
            }
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string ConfigProfileManager::current() const {
    return current_name_;
}

std::string ConfigProfileManager::load(const std::string& name) {
    if (!valid_name(name)) return "invalid profile name";

    ConfigProfile p;
    if (is_builtin(name)) {
        // Check if user has an override file
        std::string path = profile_path(name);
        std::error_code ec;
        if (fs::exists(path, ec)) {
            auto err = read_profile(path, p);
            if (!err.empty()) return err;
        } else {
            p = builtin_profile(name);
        }
    } else {
        std::string path = profile_path(name);
        auto err = read_profile(path, p);
        if (!err.empty()) return err;
    }

    // Apply to CPC struct
    CPC.model = p.model;
    CPC.ram_size = p.ram_size;
    CPC.speed = p.speed;
    CPC.scr_scale = p.scr_scale;
    CPC.scr_oglscanlines = p.scr_scanlines;
    CPC.snd_enabled = p.snd_enabled;
    CPC.snd_playback_rate = p.snd_playback_rate;
    CPC.snd_bits = p.snd_bits;
    CPC.snd_stereo = p.snd_stereo;
    CPC.snd_volume = p.snd_volume;
    CPC.joystick_emulation = p.joystick_emulation;

    current_name_ = name;
    return "";
}

std::string ConfigProfileManager::save(const std::string& name) {
    if (!valid_name(name)) return "invalid profile name";

    std::string dir = profile_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return "cannot create profile directory: " + ec.message();

    ConfigProfile p;
    p.model = CPC.model;
    p.ram_size = CPC.ram_size;
    p.speed = CPC.speed;
    p.scr_scale = CPC.scr_scale;
    p.scr_scanlines = CPC.scr_oglscanlines;
    p.snd_enabled = CPC.snd_enabled;
    p.snd_playback_rate = CPC.snd_playback_rate;
    p.snd_bits = CPC.snd_bits;
    p.snd_stereo = CPC.snd_stereo;
    p.snd_volume = CPC.snd_volume;
    p.joystick_emulation = CPC.joystick_emulation;

    auto err = write_profile(profile_path(name), p);
    if (!err.empty()) return err;

    current_name_ = name;
    return "";
}

std::string ConfigProfileManager::remove(const std::string& name) {
    if (!valid_name(name)) return "invalid profile name";
    if (is_builtin(name)) return "cannot delete built-in profile";

    std::string path = profile_path(name);
    std::error_code ec;
    if (!fs::exists(path, ec)) return "profile not found";
    if (!fs::remove(path, ec)) return "failed to delete: " + ec.message();

    if (current_name_ == name) current_name_.clear();
    return "";
}

// --- INI writer ---
std::string ConfigProfileManager::write_profile(const std::string& path, const ConfigProfile& p) {
    std::ofstream f(path);
    if (!f.is_open()) return "cannot open file for writing";

    f << "; konCePCja profile\n";
    f << "[general]\n";
    f << "model = " << p.model << "\n";
    f << "ram_size = " << p.ram_size << "\n";
    f << "speed = " << p.speed << "\n";
    f << "[display]\n";
    f << "scale = " << p.scr_scale << "\n";
    f << "scanlines = " << p.scr_scanlines << "\n";
    f << "[sound]\n";
    f << "enabled = " << p.snd_enabled << "\n";
    f << "playback_rate = " << p.snd_playback_rate << "\n";
    f << "bits = " << p.snd_bits << "\n";
    f << "stereo = " << p.snd_stereo << "\n";
    f << "volume = " << p.snd_volume << "\n";
    f << "[input]\n";
    f << "joystick = " << p.joystick_emulation << "\n";

    if (!f.good()) return "write error";
    return "";
}

// --- INI reader ---
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string ConfigProfileManager::read_profile(const std::string& path, ConfigProfile& p) {
    std::ifstream f(path);
    if (!f.is_open()) return "cannot open profile file";

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val_str = line.substr(eq + 1);
        // Strip inline comments
        auto comment = val_str.find(';');
        if (comment != std::string::npos) val_str = val_str.substr(0, comment);
        val_str = trim(val_str);

        if (val_str.empty()) continue;

        unsigned int val;
        try {
            val = static_cast<unsigned int>(std::stoul(val_str));
        } catch (...) {
            continue; // skip unparseable values
        }

        if (key == "model") p.model = val;
        else if (key == "ram_size") p.ram_size = val;
        else if (key == "speed") p.speed = val;
        else if (key == "scale") p.scr_scale = val;
        else if (key == "scanlines") p.scr_scanlines = val;
        else if (key == "enabled") p.snd_enabled = val;
        else if (key == "playback_rate") p.snd_playback_rate = val;
        else if (key == "bits") p.snd_bits = val;
        else if (key == "stereo") p.snd_stereo = val;
        else if (key == "volume") p.snd_volume = val;
        else if (key == "joystick") p.joystick_emulation = val;
    }
    return "";
}
