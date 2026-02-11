#include "data_areas.h"
#include <algorithm>
#include <cstdio>
#include <sstream>

DataAreaManager g_data_areas;

void DataAreaManager::mark(uint16_t start, uint16_t end, DataType type, const std::string& label) {
    // Remove any existing areas that overlap with the new one
    auto it = areas_.begin();
    while (it != areas_.end()) {
        if (it->second.start <= end && it->second.end >= start) {
            it = areas_.erase(it);
        } else {
            ++it;
        }
    }
    areas_[start] = DataArea{start, end, type, label};
}

void DataAreaManager::clear(uint16_t start) {
    areas_.erase(start);
}

void DataAreaManager::clear_all() {
    areas_.clear();
}

std::vector<DataArea> DataAreaManager::list() const {
    std::vector<DataArea> result;
    result.reserve(areas_.size());
    for (const auto& kv : areas_) {
        result.push_back(kv.second);
    }
    return result;
}

const DataArea* DataAreaManager::find(uint16_t addr) const {
    // Check all areas to see if addr falls within any of them
    for (const auto& kv : areas_) {
        if (addr >= kv.second.start && addr <= kv.second.end) {
            return &kv.second;
        }
    }
    return nullptr;
}

std::string DataAreaManager::format_at(uint16_t addr, const uint8_t* mem, size_t mem_size) const {
    const DataArea* area = find(addr);
    if (!area) return {};

    std::ostringstream oss;
    char buf[8];

    switch (area->type) {
        case DataType::BYTES: {
            // Up to 8 bytes per line
            oss << "db ";
            int remaining = static_cast<int>(area->end) - static_cast<int>(addr) + 1;
            int count = std::min(remaining, 8);
            for (int i = 0; i < count; i++) {
                uint16_t a = static_cast<uint16_t>(addr + i);
                if (a >= mem_size) break;
                if (i > 0) oss << ",";
                snprintf(buf, sizeof(buf), "$%02X", mem[a]);
                oss << buf;
            }
            break;
        }
        case DataType::WORDS: {
            // Up to 4 words per line
            oss << "dw ";
            int remaining_bytes = static_cast<int>(area->end) - static_cast<int>(addr) + 1;
            int word_count = std::min(remaining_bytes / 2, 4);
            if (word_count == 0 && remaining_bytes >= 2) word_count = 1;
            for (int i = 0; i < word_count; i++) {
                uint16_t a = static_cast<uint16_t>(addr + i * 2);
                if (static_cast<size_t>(a + 1) >= mem_size) break;
                if (i > 0) oss << ",";
                uint16_t w = static_cast<uint16_t>(mem[a] | (mem[a + 1] << 8));
                snprintf(buf, sizeof(buf), "$%04X", w);
                oss << buf;
            }
            break;
        }
        case DataType::TEXT: {
            // Emit printable chars as string, non-printable as hex
            oss << "db ";
            int remaining = static_cast<int>(area->end) - static_cast<int>(addr) + 1;
            int count = std::min(remaining, 64);  // reasonable line limit
            bool in_string = false;
            bool first = true;
            for (int i = 0; i < count; i++) {
                uint16_t a = static_cast<uint16_t>(addr + i);
                if (a >= mem_size) break;
                uint8_t c = mem[a];
                if (c >= 0x20 && c < 0x7F) {
                    if (!in_string) {
                        if (!first) oss << ",";
                        oss << "\"";
                        in_string = true;
                    }
                    oss << static_cast<char>(c);
                } else {
                    if (in_string) {
                        oss << "\"";
                        in_string = false;
                    }
                    if (!first) oss << ",";
                    snprintf(buf, sizeof(buf), "$%02X", c);
                    oss << buf;
                }
                first = false;
            }
            if (in_string) oss << "\"";
            break;
        }
    }

    return oss.str();
}
