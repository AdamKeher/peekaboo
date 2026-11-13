; Does poking VPOSW break PuAE's audio sample rate?
;
; Peekaboo measured a demo whose emulated frames stay a full 313-line PAL frame
; (70,824 CCK) while the core emitted only ~485 audio samples per frame instead
; of 882. UAE recomputes vblank_hz from the observed line count in a branch its
; own source comments with "(someone poked VPOSW)", and the demo's engine does
; exactly that during its system takeover.
;
; This isolates the poke from everything else: set up a steady Paula tone, let it
; run untouched for 150 frames, then write VPOSW once and keep going. Nothing
; else changes. If the samples-per-frame figure drops at frame 150, the poke is
; the cause; if it stays at 882, it is not.

CUSTOM      equ $dff000
DMACON      equ $096
VPOSW       equ $02a
AUD0LC      equ $0a0
AUD0LEN     equ $0a4
AUD0PER     equ $0a6
AUD0VOL     equ $0a8
AUD1LC      equ $0b0
AUD1LEN     equ $0b4
AUD1PER     equ $0b6
AUD1VOL     equ $0b8

SETTLE      equ 150             ; frames of clean tone before the poke

; Beam line the VPOSW write happens on, overridable with vasm -DPOKELINE=n.
; This matters: VPOSW bit 0 is vpos bit 8, so writing $8000 clears it. Poked on a
; line >= 256 that rewrites the vertical counter mid-frame; poked below 256 the
; bit is already clear, so only the LOF flag changes and the counter is untouched.
    ifnd POKELINE
POKELINE    equ 303
    endif

    section code,code

start:
    lea     CUSTOM,a6

    ; Both channels play the same table so the mix is centred and full amplitude.
    move.l  #sine,AUD0LC(a6)
    move.w  #16,AUD0LEN(a6)     ; 16 words = the 32-byte table
    move.w  #254,AUD0PER(a6)    ; 3546895/254/32 = ~436 Hz
    move.w  #64,AUD0VOL(a6)

    move.l  #sine,AUD1LC(a6)
    move.w  #16,AUD1LEN(a6)
    move.w  #254,AUD1PER(a6)
    move.w  #64,AUD1VOL(a6)

    move.w  #$8203,DMACON(a6)   ; SETCLR | DMAEN | AUD1 | AUD0

    move.w  #SETTLE-1,d7
.settle:
    bsr.s   waitvbl
    dbf     d7,.settle

    ; The single write under test. Nothing else in this program touches the beam.
    bsr.s   waitpokeline
    move.w  #$8000,VPOSW(a6)

.forever:
    bsr.s   waitvbl
    bra.s   .forever

; Wait until the beam reaches POKELINE.
waitpokeline:
    move.l  4(a6),d0
    and.l   #$1ff00,d0
    cmp.l   #POKELINE<<8,d0
    bne.s   waitpokeline
    rts

; Wait for line 303, then for it to pass, so one call is one frame.
waitvbl:
.wait_for:
    move.l  4(a6),d0
    and.l   #$1ff00,d0
    cmp.l   #303<<8,d0
    bne.s   .wait_for
.wait_past:
    move.l  4(a6),d0
    and.l   #$1ff00,d0
    cmp.l   #303<<8,d0
    beq.s   .wait_past
    rts

    section data,data_c         ; chip RAM: Paula reads this by DMA
sine:
    dc.b 0,12,24,35,45,53,59,63,64,63,59,53,45,35,24,12
    dc.b 0,-12,-24,-35,-45,-53,-59,-63,-64,-63,-59,-53,-45,-35,-24,-12
