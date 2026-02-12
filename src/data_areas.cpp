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
    // O(log N) binary search using map ordering by start address
    auto it = areas_.upper_bound(addr);
    if (it == areas_.begin()) return nullptr;
    --it;
    if (addr >= it->second.start && addr <= it->second.end) {
        return &it->second;
    }
    return nullptr;
}

std::string DataAreaManager::format_at(uint16_t addr, const uint8_t* mem, size_t mem_size,
                                       int* bytes_consumed) const {
    const DataArea* area = find(addr);
    if (!area) {
        if (bytes_consumed) *bytes_consumed = 0;
        return {};
    }

    std::ostringstream oss;
    char buf[8];
    int consumed = 0;

    switch (area->type) {
        case DataType::BYTES: {
            oss << "db ";
            int remaining = static_cast<int>(area->end) - static_cast<int>(addr) + 1;
            int count = std::min(remaining, 8);
            for (int i = 0; i < count; i++) {
                if (static_cast<size_t>(i) >= mem_size) break;
                if (i > 0) oss << ",";
                snprintf(buf, sizeof(buf), "$%02X", mem[i]);
                oss << buf;
                consumed++;
            }
            break;
        }
        case DataType::WORDS: {
            oss << "dw ";
            int remaining_bytes = static_cast<int>(area->end) - static_cast<int>(addr) + 1;
            int word_count = std::min(remaining_bytes / 2, 4);
            for (int i = 0; i < word_count; i++) {
                size_t off = static_cast<size_t>(i * 2);
                if (off + 1 >= mem_size) break;
                if (i > 0) oss << ",";
                uint16_t w = static_cast<uint16_t>(mem[off] | (mem[off + 1] << 8));
                snprintf(buf, sizeof(buf), "$%04X", w);
                oss << buf;
                consumed += 2;
            }
            break;
        }
        case DataType::TEXT: {
            oss << "db ";
            int remaining = static_cast<int>(area->end) - static_cast<int>(addr) + 1;
            int count = std::min(remaining, 64);
            bool in_string = false;
            bool first = true;
            for (int i = 0; i < count; i++) {
                if (static_cast<size_t>(i) >= mem_size) break;
                uint8_t c = mem[i];
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
                consumed++;
            }
            if (in_string) oss << "\"";
            break;
        }
    }

    if (bytes_consumed) *bytes_consumed = consumed;
    return oss.str();
}
