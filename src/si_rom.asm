; =============================================================================
; RetroFun.PL Serial Interface — CPC Expansion ROM (V1)
; =============================================================================
;
; Background ROM for the emulated Amstrad Serial Interface (AMSIf).
; Prints a banner during cold boot; does nothing else in V1.
;
; Build:
;   z80asm -o src/si_rom.bin src/si_rom.asm
;   dd if=src/si_rom.bin of=rom/serial.rom bs=16384 count=1 conv=sync
;
; The resulting rom/serial.rom is loaded at runtime by SIRomManager.
;
; CPC ROM header format (9 bytes):
;   +$00  type        0=foreground, 1=background, 2=extension
;   +$01  mark        ROM mark number
;   +$02  version     ROM version
;   +$03  modification level
;   +$04  name_table  pointer (LE) to RSX command name table
;   +$06  init_entry  JP to initialisation routine
;
; Init calling convention (firmware -> ROM):
;   A  = ROM select number (the slot this ROM occupies)
;   C  = ROM type (from +$00)
;   DE = address of first free byte in lower RAM
;   HL = address of last free byte in lower RAM
;
;   The ROM MUST preserve DE and HL (free RAM bounds) unless it claims
;   RAM by adjusting them.  It MUST return with carry set to indicate
;   success.  Clobbering HL without restoring it moves the firmware's
;   free-RAM pointer to a garbage address -> crash during later ROM
;   inits or BASIC startup.
;
; CP/M compatibility:
;   CP/M Plus re-scans ROMs during its own boot.  At that point the
;   firmware jump block (RST $08 vectors at $BB00-$BFFF) may not be
;   valid.  We guard the TXT_OUTPUT call by checking that $BB5A starts
;   with $CF (RST $08).  If it doesn't, we skip the banner.
;
; V2 roadmap:
;   - RSX commands (|BAUD, |SERIAL, etc.) via KL_LOG_EXT ($BCD1)
;   - Firmware vector interception for serial I/O
;   - The reserved 7 bytes at +$09 become JP entries for RSX dispatch
; =============================================================================

; --- Firmware entry points ---
TXT_OUTPUT: equ 0xBB5A         ; Print character in A to text VDU
RST08_OP:   equ 0xCF           ; Opcode for RST $08 (firmware jump block)

            org 0xC000

; =============================================================================
; ROM header
; =============================================================================
rom_type:   defb 0x01          ; background ROM
rom_mark:   defb 0x01
rom_ver:    defb 0x01
rom_mod:    defb 0x00
            defw name_table    ; pointer to RSX name table
            jp   rom_init      ; +$06: init entry point

; +$09: reserved — V2 will place RSX entry JPs here
            defb 0, 0, 0, 0, 0, 0, 0

; =============================================================================
; rom_init — called by firmware during ROM scan
;
; We must preserve all registers the firmware cares about (especially
; HL and DE — free RAM bounds).  Only flags may be altered on return.
; =============================================================================
rom_init:
            push hl            ; save free RAM pointer (critical!)
            push de            ; save free RAM base
            push af            ; save ROM number / type

            ; --- Guard: is the firmware jump block set up? ---
            ; Read the first byte of TXT_OUTPUT.  If it's RST $08 ($CF),
            ; the vector is live and we can safely call it.  Under CP/M
            ; re-init this byte may be garbage -> skip the banner.
            ld   a, (TXT_OUTPUT)
            cp   RST08_OP
            jr   nz, .done     ; jump block not ready -> skip banner

            ; --- Print banner string ---
            ld   hl, banner
.print_loop:
            ld   a, (hl)
            or   a
            jr   z, .done
            call TXT_OUTPUT
            inc  hl
            jr   .print_loop

.done:
            pop  af
            pop  de
            pop  hl            ; restore free RAM pointer
            scf                ; carry = success
            ret

; =============================================================================
; Banner string (null-terminated)
; =============================================================================
banner:     defb "RetroFun.PL serial interface v1", 13, 10, 0

; =============================================================================
; RSX command name table
;
; Empty in V1 — single 0x00 means "no commands".
; V2 will add entries here: each command name in uppercase with bit 7
; set on the last character, terminated by 0x00.
; =============================================================================
name_table: defb 0x00
