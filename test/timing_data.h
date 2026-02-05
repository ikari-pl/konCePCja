/* konCePCja - Timing Test Data
 *
 * Based on official documentation:
 * - Z80 User Manual (UM0080)
 * - CPC Wiki CRTC/Gate Array docs
 * - gem-knight/references/
 *
 * CPC timing notes:
 * - Z80 runs at 4 MHz but Gate Array stretches all instructions to 1µs multiples
 * - Effective speed is ~3.3 MHz due to bus arbitration
 * - All T-states are rounded up to next multiple of 4
 */

#ifndef TIMING_DATA_H
#define TIMING_DATA_H

#include "../src/types.h"

// ─────────────────────────────────────────────────
// Z80 Instruction Timing (T-states -> CPC cycles)
// ─────────────────────────────────────────────────
// CPC stretches all instructions to multiples of 4 T-states (1µs)
// Formula: cpc_cycles = ((t_states + 3) / 4) * 4

struct Z80InstructionTiming {
    byte opcode;
    byte prefix;      // 0x00=none, 0xCB, 0xDD, 0xED, 0xFD
    byte t_states;    // Original Z80 T-states
    byte cpc_cycles;  // CPC-adjusted cycles (multiple of 4)
    const char* mnemonic;
};

// Common instructions with their timings
static const Z80InstructionTiming z80_timing_table[] = {
    // No prefix instructions
    { 0x00, 0x00,  4,  4, "NOP" },
    { 0x01, 0x00, 10, 12, "LD BC,nn" },
    { 0x02, 0x00,  7,  8, "LD (BC),A" },
    { 0x03, 0x00,  6,  8, "INC BC" },
    { 0x04, 0x00,  4,  4, "INC B" },
    { 0x05, 0x00,  4,  4, "DEC B" },
    { 0x06, 0x00,  7,  8, "LD B,n" },
    { 0x07, 0x00,  4,  4, "RLCA" },
    { 0x08, 0x00,  4,  4, "EX AF,AF'" },
    { 0x09, 0x00, 11, 12, "ADD HL,BC" },
    { 0x0A, 0x00,  7,  8, "LD A,(BC)" },
    { 0x0B, 0x00,  6,  8, "DEC BC" },
    { 0x10, 0x00, 13,/* 8 if no jump */ 16, "DJNZ d (taken)" },
    { 0x18, 0x00, 12, 12, "JR d" },
    { 0x20, 0x00, 12,/* 7 if no jump */ 12, "JR NZ,d (taken)" },
    { 0x21, 0x00, 10, 12, "LD HL,nn" },
    { 0x22, 0x00, 16, 16, "LD (nn),HL" },
    { 0x23, 0x00,  6,  8, "INC HL" },
    { 0x2A, 0x00, 16, 16, "LD HL,(nn)" },
    { 0x31, 0x00, 10, 12, "LD SP,nn" },
    { 0x32, 0x00, 13, 16, "LD (nn),A" },
    { 0x3A, 0x00, 13, 16, "LD A,(nn)" },
    { 0x3E, 0x00,  7,  8, "LD A,n" },
    { 0x40, 0x00,  4,  4, "LD B,B" },
    { 0x41, 0x00,  4,  4, "LD B,C" },
    { 0x46, 0x00,  7,  8, "LD B,(HL)" },
    { 0x70, 0x00,  7,  8, "LD (HL),B" },
    { 0x76, 0x00,  4,  4, "HALT" },
    { 0x77, 0x00,  7,  8, "LD (HL),A" },
    { 0x78, 0x00,  4,  4, "LD A,B" },
    { 0x7E, 0x00,  7,  8, "LD A,(HL)" },
    { 0x80, 0x00,  4,  4, "ADD A,B" },
    { 0x86, 0x00,  7,  8, "ADD A,(HL)" },
    { 0xAF, 0x00,  4,  4, "XOR A" },
    { 0xC0, 0x00, 11,/* 5 if no ret */ 12, "RET NZ (taken)" },
    { 0xC1, 0x00, 10, 12, "POP BC" },
    { 0xC3, 0x00, 10, 12, "JP nn" },
    { 0xC5, 0x00, 11, 12, "PUSH BC" },
    { 0xC6, 0x00,  7,  8, "ADD A,n" },
    { 0xC9, 0x00, 10, 12, "RET" },
    { 0xCA, 0x00, 10, 12, "JP Z,nn" },
    { 0xCD, 0x00, 17, 20, "CALL nn" },
    { 0xD3, 0x00, 11, 12, "OUT (n),A" },
    { 0xD9, 0x00,  4,  4, "EXX" },
    { 0xDB, 0x00, 11, 12, "IN A,(n)" },
    { 0xE1, 0x00, 10, 12, "POP HL" },
    { 0xE3, 0x00, 19, 20, "EX (SP),HL" },
    { 0xE5, 0x00, 11, 12, "PUSH HL" },
    { 0xE9, 0x00,  4,  4, "JP (HL)" },
    { 0xEB, 0x00,  4,  4, "EX DE,HL" },
    { 0xF1, 0x00, 10, 12, "POP AF" },
    { 0xF3, 0x00,  4,  4, "DI" },
    { 0xF5, 0x00, 11, 12, "PUSH AF" },
    { 0xFB, 0x00,  4,  4, "EI" },

    // ED prefix instructions
    { 0x42, 0xED, 15, 16, "SBC HL,BC" },
    { 0x43, 0xED, 20, 20, "LD (nn),BC" },
    { 0x44, 0xED,  8,  8, "NEG" },
    { 0x45, 0xED, 14, 16, "RETN" },
    { 0x46, 0xED,  8,  8, "IM 0" },
    { 0x47, 0xED,  9, 12, "LD I,A" },
    { 0x4B, 0xED, 20, 20, "LD BC,(nn)" },
    { 0x4D, 0xED, 14, 16, "RETI" },
    { 0x56, 0xED,  8,  8, "IM 1" },
    { 0x5E, 0xED,  8,  8, "IM 2" },
    { 0xA0, 0xED, 16, 16, "LDI" },
    { 0xA1, 0xED, 16, 16, "CPI" },
    { 0xA2, 0xED, 16, 16, "INI" },
    { 0xA3, 0xED, 16, 16, "OUTI" },
    { 0xB0, 0xED, 21,/* 16 if BC=0 */ 24, "LDIR (continuing)" },
    { 0xB1, 0xED, 21,/* 16 if BC=0 */ 24, "CPIR (continuing)" },
    { 0xB2, 0xED, 21,/* 16 if B=0 */ 24, "INIR (continuing)" },
    { 0xB3, 0xED, 21,/* 16 if B=0 */ 24, "OTIR (continuing)" },

    // CB prefix instructions (bit operations)
    { 0x00, 0xCB,  8,  8, "RLC B" },
    { 0x06, 0xCB, 15, 16, "RLC (HL)" },
    { 0x40, 0xCB,  8,  8, "BIT 0,B" },
    { 0x46, 0xCB, 12, 12, "BIT 0,(HL)" },
    { 0x80, 0xCB,  8,  8, "RES 0,B" },
    { 0x86, 0xCB, 15, 16, "RES 0,(HL)" },
    { 0xC0, 0xCB,  8,  8, "SET 0,B" },
    { 0xC6, 0xCB, 15, 16, "SET 0,(HL)" },

    // DD prefix (IX instructions)
    { 0x21, 0xDD, 14, 16, "LD IX,nn" },
    { 0x22, 0xDD, 20, 20, "LD (nn),IX" },
    { 0x23, 0xDD, 10, 12, "INC IX" },
    { 0x2A, 0xDD, 20, 20, "LD IX,(nn)" },
    { 0x46, 0xDD, 19, 20, "LD B,(IX+d)" },
    { 0x70, 0xDD, 19, 20, "LD (IX+d),B" },
    { 0x86, 0xDD, 19, 20, "ADD A,(IX+d)" },
    { 0xE1, 0xDD, 14, 16, "POP IX" },
    { 0xE3, 0xDD, 23, 24, "EX (SP),IX" },
    { 0xE5, 0xDD, 15, 16, "PUSH IX" },
    { 0xE9, 0xDD,  8,  8, "JP (IX)" },

    {0, 0, 0, 0, nullptr}  // Sentinel
};

// ─────────────────────────────────────────────────
// CPC Gate Array Timing
// ─────────────────────────────────────────────────

// Interrupt generation: every 52 HSYNCs (scanlines)
#define GA_INTERRUPT_SCANLINES 52

// Scanline timing at 4 MHz
#define SCANLINE_CYCLES 256        // 64 µs * 4 = 256 T-states (before stretching)
#define SCANLINE_MICROSECONDS 64   // 64 µs per scanline

// Screen timing (PAL)
#define SCREEN_SCANLINES 312       // Total scanlines per frame
#define SCREEN_VISIBLE_SCANLINES 272  // Visible area
#define SCREEN_VBLANK_SCANLINES 40    // Vertical blanking
#define FRAME_RATE_HZ 50           // PAL refresh rate
#define FRAME_MICROSECONDS 20000   // 20 ms per frame

// CRTC default register values (standard screen)
#define CRTC_R0_DEFAULT 63   // Horizontal Total - 1
#define CRTC_R1_DEFAULT 40   // Horizontal Displayed
#define CRTC_R2_DEFAULT 46   // Horizontal Sync Position
#define CRTC_R3_DEFAULT 0x8E // Sync Widths (VSYNC=8, HSYNC=14)
#define CRTC_R4_DEFAULT 38   // Vertical Total - 1
#define CRTC_R5_DEFAULT 0    // Vertical Total Adjust
#define CRTC_R6_DEFAULT 25   // Vertical Displayed
#define CRTC_R7_DEFAULT 30   // Vertical Sync Position
#define CRTC_R9_DEFAULT 7    // Maximum Raster Address

// ─────────────────────────────────────────────────
// Utility macros
// ─────────────────────────────────────────────────

// Convert Z80 T-states to CPC cycles (round up to multiple of 4)
#define Z80_TO_CPC_CYCLES(t) (((t) + 3) & ~3)

// Convert microseconds to CPC cycles (at 4 MHz)
#define US_TO_CYCLES(us) ((us) * 4)

// Convert CPC cycles to microseconds
#define CYCLES_TO_US(c) ((c) / 4)

#endif // TIMING_DATA_H
