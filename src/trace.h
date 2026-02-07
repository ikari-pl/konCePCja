#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

// Instruction trace ring buffer for Z80 execution recording.
// Each entry captures the CPU state at the point of instruction execution.

struct TraceEntry {
    uint16_t pc;
    uint8_t opcode[4];     // up to 4 bytes for longest Z80 instruction
    uint8_t opcode_len;
    uint8_t a, f;
    uint16_t bc, de, hl, sp;
};

class TraceRecorder {
public:
    // Enable tracing with a ring buffer of given size (entry count)
    void enable(int buffer_size = 65536);

    // Disable tracing and free buffer
    void disable();

    // Record one instruction at current PC (called from z80_execute loop)
    void record(uint16_t pc, uint8_t a, uint8_t f,
                uint16_t bc, uint16_t de, uint16_t hl, uint16_t sp);

    // Dump trace to file as text log
    bool dump(const std::string& path) const;

    // Get trace as string (for IPC response)
    std::string to_string(int max_lines = 0) const;

    // Is tracing active?
    bool is_active() const { return active.load(); }

    // "trace on_crash <path>" — auto-dump on breakpoint/exit
    void set_crash_path(const std::string& path);
    std::string crash_path() const { return crash_dump_path; }
    void dump_if_crash() const;

    int entry_count() const;

private:
    std::vector<TraceEntry> buffer;
    int head = 0;       // next write position
    int count = 0;      // number of valid entries (up to buffer.size())
    std::atomic<bool> active{false};
    std::string crash_dump_path;
};

// Global trace recorder instance
extern TraceRecorder g_trace;
