; =============================================================================
; SERIAL.COM — HP-GL star test via BDOS 5 (L_WRITE)
; =============================================================================
;
; Sends a 5-pointed star as HP-GL to the plotter via BDOS function 5 (L_WRITE).
; Tests the emulator-level BDOS plotter hook end-to-end without DR Graph.
;
; After running, open the serial panel in the emulator and export the SVG.
;
; Build:
;   z80asm -o SERIAL.COM src/serial_com.asm
; =============================================================================

BDOS_ENTRY:  equ 0x0005

             org 0x0100

start:
             ; Print status
             ld   de, msg_start
             ld   c, 9
             call BDOS_ENTRY

             ; Send HP-GL star byte by byte via BDOS 5 (L_WRITE)
             ld   hl, hpgl_star
.send_loop:
             ld   a, (hl)
             or   a
             jr   z, .done
             ld   e, a
             ld   c, 5
             push hl
             call BDOS_ENTRY
             pop  hl
             inc  hl
             jr   .send_loop

.done:
             ld   de, msg_done
             ld   c, 9
             call BDOS_ENTRY
             ret

msg_start:   defb "Sending HP-GL star to plotter...", 13, 10, "$"
msg_done:    defb "Done. Export SVG from the serial panel.", 13, 10, "$"

; 5-pointed star: center (5183,3981), radius 2500
; Outer vertices at 90/162/234/306/18 deg, connected every other one:
;   P0(5183,6481) -> P2(3713,1959) -> P4(7561,4753)
;   -> P1(2805,4753) -> P3(6653,1959) -> P0(5183,6481)
hpgl_star:   defb "IN;SP1;"
             defb "PU5183,6481;"
             defb "PD3713,1959;"
             defb "PD7561,4753;"
             defb "PD2805,4753;"
             defb "PD6653,1959;"
             defb "PD5183,6481;"
             defb "SP0;", 13, 10, 0
