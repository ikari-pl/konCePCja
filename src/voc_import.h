// voc_import.h — clean-room Creative Voice File (.voc) → TZX converter.
//
// Written from the Creative Voice File spec (Sound Blaster SDK): 26-byte
// header, then typed blocks each carrying a 24-bit little-endian size.
// The output feeds the sub-cycle tape deck, which plays TZX block 0x15
// (Direct Recording) sample-exact — each data bit IS the line level.

#ifndef VOC_IMPORT_H
#define VOC_IMPORT_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Convert a whole .voc image to a complete TZX image ("ZXTape!\x1a", version
// 1.20): a leading 2000 ms pause block, then one 0x15 Direct Recording block
// per run of sound samples; VOC silence blocks become additional 0x20 pause
// blocks between the runs. Returns an empty vector on malformed input (bad
// magic, truncated block, non-PCM codec, mid-file sample-rate change).
std::vector<uint8_t> voc_to_tzx(const uint8_t* data, size_t len);

#endif
