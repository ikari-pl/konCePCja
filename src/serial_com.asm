; =============================================================================
; SERIAL.COM — CP/M Plus BDOS hook installer for RetroFun serial interface
; =============================================================================
;
; Run from the CP/M A> prompt before DR Graph to route BDOS 5/3 through
; the Z80 DART chip at $FADx (emulated Amstrad Serial Interface).
;
; Usage: SERIAL  (from A>)
;
; Effect:
;   Copies hook code to $0040-$006E and redirects $0005 to $0040.
;   BDOS 5 (L_WRITE): sends byte in E to DART Channel A ($FADC)
;   BDOS 3 (A_READ):  reads byte from DART Channel A ($FADC) into A
;   All other BDOS functions pass through unchanged.
;
; Idempotent — safe to run twice.
;
; Build:
;   z80asm -o SERIAL.COM src/serial_com.asm
; =============================================================================

JP_OP:       equ 0xC3
BDOS_ENTRY:  equ 0x0005
HOOK_RAM:    equ 0x0040

DART_HI:     equ 0xFA
DART_DATA:   equ 0xDC           ; Channel A Data  ($FADC)
DART_CTRL:   equ 0xDE           ; Channel A Status ($FADE)
RR0_TX:      equ 2              ; bit 2: TX empty
RR0_RX:      equ 0              ; bit 0: RX available

BDOS_LWRITE: equ 5
BDOS_AREAD:  equ 3

             org 0x0100         ; CP/M .COM load address

; --- Entry point ---
start:
             ; Check $0005 contains JP
             ld   a, (BDOS_ENTRY)
             cp   JP_OP
             jp   nz, .err_no_bdos

             ; Already hooked? Our hook sits at HOOK_RAM ($0040): high byte = $00
             ld   hl, (BDOS_ENTRY + 1)
             ld   a, h
             or   a
             jr   nz, .do_hook      ; high byte non-zero → not our hook
             ld   a, l
             cp   HOOK_RAM & 0xFF
             jp   z, .already       ; exactly $0040 → already installed

.do_hook:
             ; HL = original BDOS address — must survive the LDIR
             push hl

             ; Copy hook code to HOOK_RAM ($0040)
             ld   de, HOOK_RAM
             ld   hl, hook_code
             ld   bc, hook_code_end - hook_code
             ldir

             ; Patch passthrough JP address at HOOK_RAM+10
             pop  hl                ; original BDOS address
             ld   (HOOK_RAM + 10), hl

             ; Redirect $0005 → HOOK_RAM
             ld   a, JP_OP
             ld   (BDOS_ENTRY), a
             ld   hl, HOOK_RAM
             ld   (BDOS_ENTRY + 1), hl

             ; Print success (BDOS 9, C_WRITESTR, $-terminated string in DE)
             ; $0005 now points to our hook, which passes C=9 through unchanged.
             ld   de, msg_ok
             ld   c, 9
             call BDOS_ENTRY
             ret

.already:
             ld   de, msg_already
             ld   c, 9
             call BDOS_ENTRY
             ret

.err_no_bdos:
             ld   de, msg_fail
             ld   c, 9
             call BDOS_ENTRY
             ret

; --- Messages ($-terminated for BDOS 9) ---
msg_ok:      defb "Serial port activated.", 13, 10, "$"
msg_already: defb "Already active.", 13, 10, "$"
msg_fail:    defb "BDOS not found.", 13, 10, "$"

; =============================================================================
; hook_code — copied to HOOK_RAM ($0040) at runtime
;
; Position-independent (relative branches only).
; passthru JP opcode at offset 9, address bytes at offset 10 (patched above).
;
; Offset map (same as si_rom.asm hook_template):
;   0:  LD A,C      (1)
;   1:  CP 5        (2)
;   3:  JR Z +off   (2)  → .list_out
;   5:  CP 3        (2)
;   7:  JR Z +off   (2)  → .aux_in
;   9:  JP $0000    (3)  passthru (address patched at +10)
;  12:  .list_out
;  ...  .aux_in
; =============================================================================
hook_code:
             ld   a, c
             cp   BDOS_LWRITE       ; C=5?
             jr   z, .list_out
             cp   BDOS_AREAD        ; C=3?
             jr   z, .aux_in
             jp   0x0000            ; passthru — address patched at HOOK_RAM+10

; --- BDOS 5 (L_WRITE): send byte in E to DART Channel A ---
.list_out:
             ld   a, e
             push bc
             ld   b, DART_HI        ; B=$FA
.lwait:
             ld   c, DART_CTRL      ; C=$DE → port $FADE
             in   c, (c)            ; read RR0
             bit  RR0_TX, c         ; TX empty?
             jr   z, .lwait
             ld   c, DART_DATA      ; C=$DC → port $FADC
             out  (c), a
             pop  bc
             ret

; --- BDOS 3 (A_READ): read byte from DART Channel A into A ---
.aux_in:
             push bc
             ld   b, DART_HI        ; B=$FA
.rwait:
             ld   c, DART_CTRL      ; C=$DE → port $FADE
             in   a, (c)            ; read RR0 (triggers rx_poll in emulator)
             bit  RR0_RX, a         ; RX available?
             jr   z, .rwait
             ld   c, DART_DATA      ; C=$DC → port $FADC
             in   a, (c)            ; read byte
             and  0x7F              ; 7-bit ASCII
             pop  bc
             ret

hook_code_end:
