#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct PokeValue {
    uint16_t address;
    uint8_t value;          // value to write (if 256 in file, default to 0)
    uint8_t original_value; // for unapply (stored when applied)
    bool needs_input;       // true if value was 256 in .POK (ask user)
};

struct Poke {
    std::string description;
    std::vector<PokeValue> values;
    bool applied = false;
};

struct PokeGame {
    std::string title;
    std::vector<Poke> pokes;
};

class PokeManager {
public:
    std::string load(const std::string& path);
    std::string load_from_string(const std::string& content);
    const std::vector<PokeGame>& games() const;

    using WriteFn = void(*)(uint16_t addr, uint8_t val);
    using ReadFn = uint8_t(*)(uint16_t addr);

    int apply(size_t game_idx, size_t poke_idx, WriteFn write_fn, ReadFn read_fn);
    int apply_all(size_t game_idx, WriteFn write_fn, ReadFn read_fn, int* total_values = nullptr);
    int unapply(size_t game_idx, size_t poke_idx, WriteFn write_fn);
    void clear();

private:
    std::vector<PokeGame> games_;
    std::string parse_pok(const std::string& content);
};

extern PokeManager g_poke_manager;
