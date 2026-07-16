; =============================================================================
; RetroFun.PL Serial Interface — CPC Expansion ROM (V2)
; =============================================================================
;
; Background ROM for the emulated Amstrad Serial Interface (AMSIf).
;
; V1: Print banner during boot.
; V2: Register RSX |SERIAL. When called, hooks BDOS entry at $0005 to
;     intercept serial I/O through the Z80 DART chip at $FADx, enabling
;     GSX applications like DR Graph to output HP-GL data to the plotter.
;
; Build:
;   z80asm -o src/si_rom.bin src/si_rom.asm
;   dd if=src/si_rom.bin of=rom/serial.rom bs=16384 count=1 conv=sync
;
; Usage:
;   At BASIC or CP/M prompt: |SERIAL
;   Then run DR Graph normally.
;
; =============================================================================
; Architecture:
;
;   rom_init ($C010):
;     Called once at firmware boot AND once at CP/M boot (CPC re-scans ROMs).
;     TXT_OUTPUT ($BB5A) stays $CF under CP/M Plus — firmware jump block
;     survives. Prints banner + hint both times.
;
;   RSX |SERIAL:
;     Registered automatically by the firmware via name_table pointer at +$04.
;     Callable from both BASIC and CP/M (RSX command chain survives CP/M boot).
;
;   serial_handler:
;     Called when user types |SERIAL. At this point $0005 is fully set up.
;     Copies hook_template to RAM at HOOK_RAM ($0040) via LDIR (ROM is paged
;     in during RSX dispatch), patches the passthrough JP, redirects $0005.
;
;   hook_template (runs from RAM — ROM NOT paged in during CP/M execution):
;     Intercepts BDOS 5 (L_WRITE): sends byte in E to DART TX ($FADC).
;     Intercepts BDOS 3 (A_READ):  reads byte from DART RX ($FADC) into A.
;     All other BDOS calls pass through to the original BDOS entry.
;     Position-independent (relative branches only). ~30 bytes.
;
; BDOS hook layout at HOOK_RAM ($0040):
;   +0  LD A,C                  check function code
;   +1  CP 5 / JR Z .list_out   L_WRITE: plotter output
;   +5  CP 3 / JR Z .aux_in     A_READ:  plotter input
;   +9  JP <orig_bdos>           passthrough (address patched at +10)
;   +12 .list_out: wait TX, OUT ($FADC), RET
;   +.. .aux_in:  wait RX, IN ($FADC), AND $7F, RET
; =============================================================================

; --- Firmware ---
TXT_OUTPUT:  equ 0xBB5A
RST08_OP:    equ 0xCF
JP_OP:       equ 0xC3

; --- DART ---
DART_HI:     equ 0xFA
DART_DATA:   equ 0xDC           ; Channel A Data  ($FADC)
DART_CTRL:   equ 0xDE           ; Channel A Status ($FADE)
RR0_RX:      equ 0              ; bit 0: RX available
RR0_TX:      equ 2              ; bit 2: TX empty

; --- CP/M ---
BDOS_ENTRY:  equ 0x0005
BDOS_LWRITE: equ 5
BDOS_AREAD:  equ 3

; --- RAM for hook code ($0040-$006F — RST area, unused under CP/M) ---
HOOK_RAM:    equ 0x0040

             org 0xC000

; =============================================================================
; ROM header
; =============================================================================
             defb 0x01          ; +$00: background ROM
             defb 0x01          ; +$01: mark
             defb 0x02          ; +$02: version (V2)
             defb 0x00          ; +$03: modification
             defw name_table    ; +$04: RSX name table (auto-registered by firmware)
             jp   rom_init      ; +$06: init entry point

; +$09: reserved
             defb 0, 0, 0, 0, 0, 0, 0

; =============================================================================
; rom_init — called by firmware at boot AND by CP/M Plus at CP/M boot
;
; Calling convention (CPC background ROM):
;   DE = first free byte in lower RAM (must preserve)
;   HL = last free byte in lower RAM  (must preserve)
;   Carry must be set on return.
; =============================================================================
rom_init:
             push hl
             push de
             push af
             push bc

             ; TXT_OUTPUT ($BB5A) contains RST $08 ($CF) in both firmware and
             ; CP/M Plus (firmware jump block survives). Print banner both times.
             ld   a, (TXT_OUTPUT)
             cp   RST08_OP
             jr   nz, .exit

             ld   hl, banner
.print_loop:
             ld   a, (hl)
             or   a
             jr   z, .exit
             call TXT_OUTPUT
             inc  hl
             jr   .print_loop

.exit:
             pop  bc
             pop  af
             pop  de
             pop  hl
             scf
             ret

; =============================================================================
; Banner — printed on firmware boot and CP/M boot
; =============================================================================
banner:      defb "RetroFun serial IF v2  |SERIAL to enable", 13, 10, 0

; =============================================================================
; RSX name table — firmware auto-registers this via +$04 header pointer
;
; Format: name bytes (last byte has bit 7 set), then JP to handler, then $00.
;
; |SERIAL — installs BDOS hook for serial I/O via DART chip
; =============================================================================
name_table:
             ; "SERIAL": S=$53 E=$45 R=$52 I=$49 A=$41, L|$80=$CC
             defb 0x53, 0x45, 0x52, 0x49, 0x41, 0xCC
             jp   serial_handler
             defb 0x00          ; end of table

; =============================================================================
; serial_handler — RSX |SERIAL handler
;
; Called when user types |SERIAL (from BASIC or CP/M prompt).
; The ROM is paged in by the firmware RSX dispatcher, so hook_template
; (in ROM at $C0xx) is reachable by LDIR.
; At this point $0005 is guaranteed to contain JP <bdos_address>.
;
; Installs BDOS hook: copies hook_template to HOOK_RAM, patches passthrough
; JP, redirects $0005 to HOOK_RAM.  Idempotent — safe to call twice.
; =============================================================================
serial_handler:
             push de
             push bc

             ; Is $0005 a JP instruction?
             ld   a, (BDOS_ENTRY)
             cp   JP_OP
             jr   nz, .done              ; not JP — nothing to do

             ; Already hooked? HOOK_RAM < $0100 so high byte of address = $00
             ld   hl, (BDOS_ENTRY + 1)
             ld   a, h
             or   a
             jr   nz, .do_hook           ; high byte != 0 → not our hook
             ld   a, l
             cp   HOOK_RAM & 0xFF
             jr   z, .done               ; already hooked — skip

.do_hook:
             ; HL = original BDOS address — preserve across LDIR
             push hl

             ; Copy hook template to RAM
             ld   de, HOOK_RAM
             ld   hl, hook_template
             ld   bc, hook_end - hook_template
             ldir

             ; Patch passthrough JP at offset 10 from HOOK_RAM
             ; (passthru_jp opcode at +9, address bytes at +10/+11)
             pop  hl                     ; original BDOS address
             ld   (HOOK_RAM + 10), hl

             ; Redirect $0005 → HOOK_RAM
             ld   a, JP_OP
             ld   (BDOS_ENTRY), a
             ld   hl, HOOK_RAM
             ld   (BDOS_ENTRY + 1), hl

             ; Print confirmation (TXT_OUTPUT still valid under CP/M Plus)
             ld   hl, msg_activated
.print_msg:
             ld   a, (hl)
             or   a
             jr   z, .done
             call TXT_OUTPUT
             inc  hl
             jr   .print_msg

.done:
             pop  bc
             pop  de
             scf
             ret

; =============================================================================
; Confirmation message
; =============================================================================
msg_activated: defb "Serial port activated.", 13, 10, 0

; =============================================================================
; hook_template — copied to RAM at HOOK_RAM ($0040) by serial_handler
;
; Runs from RAM (ROM is NOT paged in during CP/M execution).
; Position-independent: only relative branches used.
;
; The passthrough JP address is patched at offset 10 (HOOK_RAM+10) after copy.
;
; Offset map (verify against hook_end - hook_template):
;   0:  LD A,C        ($79)           — 1 byte
;   1:  CP 5          ($FE,$05)       — 2 bytes
;   3:  JR Z +offset  ($28,$xx)       — 2 bytes  → .list_out
;   5:  CP 3          ($FE,$03)       — 2 bytes
;   7:  JR Z +offset  ($28,$xx)       — 2 bytes  → .aux_in
;   9:  JP $0000      ($C3,$00,$00)   — 3 bytes  passthru (addr patched at +10)
;  12:  .list_out ...
; =============================================================================
hook_template:
             ld   a, c
             cp   BDOS_LWRITE          ; C=5?
             jr   z, .list_out
             cp   BDOS_AREAD           ; C=3?
             jr   z, .aux_in
passthru_jp:
             jp   0x0000               ; patched: original BDOS address

; --- BDOS 5 (L_WRITE): send byte in E to DART Channel A ---
.list_out:
             ld   a, e                 ; byte to send
             push bc
             ld   b, DART_HI           ; B=$FA (DART port high byte)
.lwait:
             ld   c, DART_CTRL         ; C=$DE → port $FADE = Ch.A Status
             in   c, (c)               ; read RR0 into C (B kept as $FA)
             bit  RR0_TX, c            ; bit 2: TX empty?
             jr   z, .lwait
             ld   c, DART_DATA         ; C=$DC → port $FADC = Ch.A Data
             out  (c), a               ; send byte
             pop  bc
             ret

; --- BDOS 3 (A_READ): read byte from DART Channel A into A ---
.aux_in:
             push bc
             ld   b, DART_HI           ; B=$FA
.rwait:
             ld   c, DART_CTRL         ; C=$DE → port $FADE = Ch.A Status
             in   a, (c)               ; read RR0 into A (triggers rx_poll)
             bit  RR0_RX, a            ; bit 0: RX available?
             jr   z, .rwait
             ld   c, DART_DATA         ; C=$DC → port $FADC = Ch.A Data
             in   a, (c)               ; read byte
             and  0x7F                 ; 7-bit ASCII (HP-GL convention)
             pop  bc
             ret

hook_end:
