#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

enum class DataType { BYTES, WORDS, TEXT };

struct DataArea {
    uint16_t start;
    uint16_t end;      // inclusive
    DataType type;
    std::string label;  // optional
};

class DataAreaManager {
public:
    void mark(uint16_t start, uint16_t end, DataType type, const std::string& label = "");
    void clear(uint16_t start);
    void clear_all();
    std::vector<DataArea> list() const;

    // Query: is this address inside a data area?
    const DataArea* find(uint16_t addr) const;

    // Format a data area line for disassembly output.
    // mem points to data starting at addr (relative indexing: mem[0] is the byte at addr).
    // Returns empty string if addr is not in a data area.
    // If bytes_consumed is non-null, stores the number of bytes this line covers.
    std::string format_at(uint16_t addr, const uint8_t* mem, size_t mem_size,
                          int* bytes_consumed = nullptr) const;

private:
    std::map<uint16_t, DataArea> areas_;  // keyed by start address
};

extern DataAreaManager g_data_areas;
