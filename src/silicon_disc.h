#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

// DK'Tronics Silicon Disc: 256K battery-backed RAM
// Occupies expansion banks 4-7 (4 banks x 64K each)
// Memory persists across emulator resets (simulating battery backup)

constexpr int SILICON_DISC_BANKS = 4;        // banks 4-7
constexpr int SILICON_DISC_FIRST_BANK = 4;   // first bank index
constexpr size_t SILICON_DISC_BANK_SIZE = 65536;
constexpr size_t SILICON_DISC_SIZE = SILICON_DISC_BANKS * SILICON_DISC_BANK_SIZE; // 256K

struct SiliconDisc {
    bool enabled;
    uint8_t* data;       // 256K buffer, NOT cleared on reset

    SiliconDisc() : enabled(false), data(nullptr) {}

    // Get pointer to a specific bank's 64K region (bank_index 0-3, mapped from expansion banks 4-7)
    uint8_t* bank_ptr(int bank_index) const {
        if (!data || bank_index < 0 || bank_index >= SILICON_DISC_BANKS) return nullptr;
        return data + (bank_index * SILICON_DISC_BANK_SIZE);
    }

    // Check if an expansion bank number falls in the Silicon Disc range
    bool owns_bank(int expansion_bank) const {
        return enabled && expansion_bank >= SILICON_DISC_FIRST_BANK &&
               expansion_bank < SILICON_DISC_FIRST_BANK + SILICON_DISC_BANKS;
    }
};

// Allocate/free Silicon Disc buffer
void silicon_disc_init(SiliconDisc& sd);
void silicon_disc_free(SiliconDisc& sd);

// Clear Silicon Disc contents (like removing the battery)
void silicon_disc_clear(SiliconDisc& sd);

// Save/load Silicon Disc contents to/from file
bool silicon_disc_save(const SiliconDisc& sd, const std::string& path);
bool silicon_disc_load(SiliconDisc& sd, const std::string& path);

extern SiliconDisc g_silicon_disc;
