; =============================================================================
; si_rsx.asm — the resident RSX module for SERIAL.COM (CP/M 3 / CP/M Plus)
; =============================================================================
;
; A CP/M 3 Resident System Extension that filters the character-I/O BDOS calls
; DR Graph's GSX plotter driver (DDHP7470.PRL) makes, routing them to the
; Amstrad SI card's Z80-DART instead of the console/printer. This is the
; CP/M-Plus-native, persistent replacement for the retired engine=0 host hooks
; and for the base-page $0006 hook (which CP/M Plus's banked SCB reverts).
;
; This file is JUST the resident module: the 27-byte RSX prefix followed by the
; filter body. It is assembled by scripts/build_serial_rsx.py at org 0x0100 into
; a PRL, whose reloc bitmap flags the single absolute reference (start's JP
; operand). The LOADER relocates it to the top of the TPA and chains it at
; $0005; because remove=00 it persists across every subsequent transient load.
;
; The transient body that programs the DART baud (the q9kx fix) and prints the
; banner lives in src/si_serial.asm; GENCOM attaches THIS module to it.
;
; ── Position-independence contract ───────────────────────────────────────────
; Every branch in the filter is a RELATIVE jr; the ONLY absolute datum is
; `start: jp ftest`. So the PRL relocation bitmap has exactly one bit set (the
; high byte of that operand, code offset +8). build_serial_rsx.py double-
; assembles to VERIFY nothing else moved — add an absolute ref here and the
; build will flag it.
; =============================================================================

; ── SI card Z80-DART, channel A ──────────────────────────────────────────────
DART_HI:     equ 0xFA
DART_DATA:   equ 0xDC            ; port $FADC — data
DART_CTRL:   equ 0xDE            ; port $FADE — command/status (RR0 on read)
RR0_RXRDY:   equ 0x01            ; RR0 bit 0 — an Rx character is available
RR0_TXEMP:   equ 0x04            ; RR0 bit 2 — the Tx buffer is empty

; ── BDOS character-I/O functions we intercept ────────────────────────────────
; We deliberately do NOT touch C=6 (Direct Console I/O). It is the CONSOLE, and
; GSX apps (DR Graph) poll it for keystrokes with E=$FF — intercepting it steals
; every keypress and wedges the app at its menu. The plotter only needs the AUX
; path (C=3 in / C=4 out) plus C=5 (LIST) for CR/LF; if the far end ever needs
; flow control it is on the AUX status calls (C=7/C=8), not the console.
FN_AUXIN:    equ 3               ; RDR   — blocking read  (plotter reply)
FN_AUXOUT:   equ 4               ; PUN   — write          (HP-GL data + queries)
FN_LISTOUT:  equ 5               ; LST   — write          (CR/LF)

JP_OP:       equ 0xC3

             org 0x0100

; =============================================================================
; 27-byte CP/M 3 RSX prefix (Programmer's Guide §4.4.1).
; Only remove / nonbank / name are ours to set; the LOADER fills serial, next's
; operand and prev at load time, and relocates start's operand via the bitmap.
; =============================================================================
rsx_base:
serial:      defb 0,0,0,0,0,0            ; +00 serial#  (loader-filled)
start:       jp   ftest                  ; +06 entry: chain head jumps here
                                          ;     (operand is THE relocated datum)
next:        defb JP_OP                   ; +09 chain JP opcode ...
             defw 0                       ; +0A ... operand -> prev head (loader)
prev:        defw 0                       ; +0C preceding module / loc 6 (loader)
remove:      defb 0x00                    ; +0E 00 = persist across program loads
nonbank:     defb 0x00                    ; +0F 00 = load on the banked CPC 6128
rname:       defb 'SERIAL  '              ; +10 8 chars, space-padded
rloader:     defb 0x00                    ; +18 00 (the real LOADER carries FF)
             defb 0, 0                    ; +19 reserved

; ── BDOS filter entry (+$1B). C = fn, E = char / sub-code, as for CALL 5. ─────
; Convention-compliant return on interception: A = result, L = A, H = B = 0.
; Pass-through is a RELATIVE jr into the prefix's chain JP (never absolute).
ftest:
             ld   a, c
             cp   FN_AUXOUT
             jr   z, f_tx                ; C=4 AUX-OUT  — HP-GL data
             cp   FN_LISTOUT
             jr   z, f_tx                ; C=5 LIST     — CR/LF
             cp   FN_AUXIN
             jr   z, f_rx                ; C=3 RDR      — reply (blocking)
             jr   next                   ; not ours: relative chain into the JP

             ; ── send E to DART channel A (wait for the Tx buffer) ──
f_tx:
             push bc
             ld   b, DART_HI
.txw:
             ld   c, DART_CTRL
             in   a, (c)                 ; RR0
             and  RR0_TXEMP
             jr   z, .txw
             ld   c, DART_DATA
             ld   a, e
             out  (c), a
             pop  bc
             ld   a, e                   ; result byte (BA=HL=A convention)
             ld   l, a
             ld   h, 0
             ld   b, h
             ret

             ; ── blocking receive from DART -> A, 7-bit (plotter reply) ──
f_rx:
             push bc
             ld   b, DART_HI
.rxw:
             ld   c, DART_CTRL
             in   a, (c)
             and  RR0_RXRDY
             jr   z, .rxw
             ld   c, DART_DATA
             in   a, (c)
             and  0x7F
             pop  bc
             ld   l, a
             ld   h, 0
             ld   b, h
             ret

rsx_end:
