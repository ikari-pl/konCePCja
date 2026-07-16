; =============================================================================
; si_serial.asm — SERIAL.COM transient body (CP/M 3 / CP/M Plus)
; =============================================================================
;
; The transient half of SERIAL.COM. GENCOM attaches the resident RSX filter
; (src/si_rsx.asm) to this body; scripts/build_serial_rsx.py builds the combined
; .COM. When the user types SERIAL, the CCP recognises the RSX command header,
; makes the LOADER resident, relocates+chains the RSX (persists: remove=00), and
; THEN runs this body at $0100. So by the time we execute, the BDOS filter is
; already live — our only jobs are:
;
;   1. Program the SI card's 8253 baud generator so the DART Tx clocks at the
;      HP7470's DIP line rate (the q9kx fix). On real hardware software must set
;      the 8253 to match the plotter; DDHP7470.PRL goes through the BDOS and
;      never does, so "activate serial" must. Both ends time a bit as
;      divisor*128 master cycles, so the 8253 reload must equal the plotter
;      divisor the host derives from peripheral.serial baud: 2e6/(baud*16) ->
;      9600 baud = 13.
;   2. Print a confirmation and return to the CCP (RET; we never touch SP, and
;      the RSX stays resident behind us).
; =============================================================================

PRINTSTR:    equ 9               ; BDOS 9: print '$'-terminated string
BDOS:        equ 0x0005

; ── SI card 8253 baud generator (counter 0 clocks the DART Tx) ───────────────
PIT_CTRL:    equ 0xFBDF          ; 8253 control word
PIT_CTR0:    equ 0xFBDC          ; counter 0 data (LSB then MSB)
PIT_CW_CTR0: equ 0x36            ; ctr0, R/L LSB+MSB, mode 3 (square wave), binary
BAUD_DIV_LO: equ 13              ; low  byte of the reload (9600 baud -> 13)
BAUD_DIV_HI: equ 0               ; high byte

             org 0x0100

start:
             ; Clock the DART to the plotter's line rate (three atomic OUTs).
             ld   bc, PIT_CTRL
             ld   a, PIT_CW_CTR0
             out  (c), a                  ; latch counter-0 mode
             ld   bc, PIT_CTR0
             ld   a, BAUD_DIV_LO
             out  (c), a                  ; reload LSB
             ld   a, BAUD_DIV_HI
             out  (c), a                  ; reload MSB

             ; Announce and return to the CCP (RSX already resident behind us).
             ld   de, msg_ok
             ld   c, PRINTSTR
             call BDOS
             ret

msg_ok:      defb "Serial RSX active (AUX/LIST/RDR/DCIO -> DART @9600).", 13, 10, "$"
