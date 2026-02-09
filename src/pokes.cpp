#include "pokes.h"
#include <fstream>
#include <sstream>
#include <algorithm>

PokeManager g_poke_manager;

std::string PokeManager::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "cannot open file";
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return load_from_string(content);
}

std::string PokeManager::load_from_string(const std::string& content) {
    games_.clear();
    return parse_pok(content);
}

const std::vector<PokeGame>& PokeManager::games() const {
    return games_;
}

void PokeManager::clear() {
    games_.clear();
}

std::string PokeManager::parse_pok(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    PokeGame* current_game = nullptr;
    Poke* current_poke = nullptr;

    while (std::getline(stream, line)) {
        // Strip trailing CR if present
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Skip empty lines
        if (line.empty()) continue;

        char prefix = line[0];
        std::string rest = line.substr(1);

        if (prefix == 'N') {
            // New game
            PokeGame game;
            game.title = rest;
            games_.push_back(std::move(game));
            current_game = &games_.back();
            current_poke = nullptr;
        } else if (prefix == 'M') {
            // New poke (cheat) within current game
            if (!current_game) return "M line before any N line";
            Poke poke;
            poke.description = rest;
            current_game->pokes.push_back(std::move(poke));
            current_poke = &current_game->pokes.back();
        } else if (prefix == 'Z' || prefix == 'Y') {
            // Poke value line
            if (!current_poke) return "Z/Y line before any M line";
            std::istringstream vals(rest);
            unsigned int addr, value, orig;
            if (!(vals >> addr >> value >> orig)) {
                return "invalid poke value line: " + line;
            }
            PokeValue pv;
            pv.address = static_cast<uint16_t>(addr);
            if (value == 256) {
                pv.value = 0;       // default for "ask user"
                pv.needs_input = true;
            } else {
                pv.value = static_cast<uint8_t>(value);
                pv.needs_input = false;
            }
            pv.original_value = static_cast<uint8_t>(orig);
            current_poke->values.push_back(pv);
            // Y = last value in this poke, Z = more follow
            if (prefix == 'Y') {
                current_poke = nullptr;
            }
        } else {
            // Unknown prefix, skip (be lenient)
        }
    }

    if (games_.empty()) return "no games found in file";
    return ""; // success
}

int PokeManager::apply(size_t game_idx, size_t poke_idx, WriteFn write_fn, ReadFn read_fn) {
    if (game_idx >= games_.size()) return -1;
    auto& game = games_[game_idx];
    if (poke_idx >= game.pokes.size()) return -1;
    auto& poke = game.pokes[poke_idx];
    if (poke.applied) return 0;

    for (auto& val : poke.values) {
        // Save the current value for unapply
        val.original_value = read_fn(val.address);
        write_fn(val.address, val.value);
    }
    poke.applied = true;
    return static_cast<int>(poke.values.size());
}

int PokeManager::apply_all(size_t game_idx, WriteFn write_fn, ReadFn read_fn, int* total_values) {
    if (game_idx >= games_.size()) return -1;
    auto& game = games_[game_idx];
    int pokes_applied = 0;
    int values_total = 0;
    for (size_t i = 0; i < game.pokes.size(); i++) {
        int n = apply(game_idx, i, write_fn, read_fn);
        if (n > 0) {
            pokes_applied++;
            values_total += n;
        }
    }
    if (total_values) *total_values = values_total;
    return pokes_applied;
}

int PokeManager::unapply(size_t game_idx, size_t poke_idx, WriteFn write_fn) {
    if (game_idx >= games_.size()) return -1;
    auto& game = games_[game_idx];
    if (poke_idx >= game.pokes.size()) return -1;
    auto& poke = game.pokes[poke_idx];
    if (!poke.applied) return -1;

    for (const auto& val : poke.values) {
        write_fn(val.address, val.original_value);
    }
    poke.applied = false;
    return static_cast<int>(poke.values.size());
}
