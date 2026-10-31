#include "trace.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "z80_view.h"

TraceRecorder g_trace;
int g_trace_default_size = 65536;

void TraceRecorder::enable(int buffer_size) {
  buffer.resize(buffer_size);
  head = 0;
  count = 0;
  active.store(true);
}

void TraceRecorder::disable() {
  active.store(false);
  buffer.clear();
  head = 0;
  count = 0;
}

void TraceRecorder::record(uint16_t pc, uint8_t a, uint8_t f, uint16_t bc,
                           uint16_t de, uint16_t hl, uint16_t sp) {
  if (!active.load() || buffer.empty()) return;

  auto& e = buffer[head];
  e.pc = pc;
  e.a = a;
  e.f = f;
  e.bc = bc;
  e.de = de;
  e.hl = hl;
  e.sp = sp;

  // Read opcode bytes (peek without side effects)
  e.opcode[0] = z80_read_mem(pc);

  // Capture opcode prefix bytes for the trace log.  This is a simplified
  // heuristic that records enough to identify the instruction but does NOT
  // determine the full instruction length (which would require a complete
  // decode table).  Variable-length operands (e.g. LD (IX+d),n = 4 bytes
  // after prefix) are not fully captured.  The register dump alongside
  // each trace entry is the authoritative record; the opcode bytes are a
  // convenience for quick identification.
  uint8_t const op0 = e.opcode[0];
  int len = 1;

  if (op0 == 0xCB || op0 == 0xED) {
    e.opcode[1] = z80_read_mem(pc + 1);
    len = 2;
  } else if (op0 == 0xDD || op0 == 0xFD) {
    e.opcode[1] = z80_read_mem(pc + 1);
    if (e.opcode[1] == 0xCB) {
      e.opcode[2] = z80_read_mem(pc + 2);
      e.opcode[3] = z80_read_mem(pc + 3);
      len = 4;
    } else {
      len = 2;
    }
  }
  e.opcode_len = static_cast<uint8_t>(len);

  head = (head + 1) % static_cast<int>(buffer.size());
  if (count < static_cast<int>(buffer.size())) count++;
}

int TraceRecorder::entry_count() const { return count; }

namespace {
void format_entry(const TraceEntry& e, char* buf, size_t bufsz) {
  char opcodes[16] = {};
  int pos = 0;
  for (int i = 0; i < e.opcode_len && i < 4; i++) {
    pos += snprintf(opcodes + pos, sizeof(opcodes) - pos, "%02X", e.opcode[i]);
  }
  snprintf(buf, bufsz,
           "%04X %-8s A=%02X F=%02X BC=%04X DE=%04X HL=%04X SP=%04X", e.pc,
           opcodes, e.a, e.f, e.bc, e.de, e.hl, e.sp);
}
}  // namespace

void TraceRecorder::snapshot(std::vector<TraceEntry>& out) const {
  out.resize(count);
  if (count == 0) return;
  int const n = static_cast<int>(buffer.size());
  int const start = (count < n) ? 0 : head;
  int const first = std::min(count, n - start);
  std::memcpy(out.data(), &buffer[start], first * sizeof(TraceEntry));
  if (first < count) {
    std::memcpy(out.data() + first, &buffer[0],
                (count - first) * sizeof(TraceEntry));
  }
}

bool TraceRecorder::write_entries(const std::vector<TraceEntry>& entries,
                                  const std::string& path) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return false;
  char line[128];
  for (const auto& e : entries) {
    format_entry(e, line, sizeof(line));
    fprintf(f, "%s\n", line);
  }
  fclose(f);
  return true;
}

bool TraceRecorder::dump(const std::string& path) const {
  std::vector<TraceEntry> entries;
  snapshot(entries);
  return write_entries(entries, path);
}

void TraceRecorder::set_crash_path(const std::string& path) {
  crash_dump_path = path;
}

void TraceRecorder::dump_if_crash() const {
  if (!crash_dump_path.empty() && count > 0) {
    dump(crash_dump_path);
  }
}
