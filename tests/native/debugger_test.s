; Amiga M68000 Debugger Test Program
; Specifically designed to test:
; - CPU Tracing (Subroutines, Loops)
; - Copper Tracing (WAITs, Color changes)
; - Blitter Tracing (DMA operations with multiple sources)
; - Frame Scrubber (Visual changes across frames)
; - Disassembly Profiler (Hotspot generation)

    section "Code",code

; Custom Register Offsets
CUSTOM      EQU $DFF000
DMACON      EQU $096
DMACONR     EQU $002
VHPOSR      EQU $004
COLOR00     EQU $180
COP1LCH     EQU $080
INTENA      EQU $09A
INTREQ      EQU $09C

; Blitter Registers
BLTCON0     EQU $040
BLTCON1     EQU $042
BLTAFWM     EQU $044
BLTALWM     EQU $046
BLTAPTH     EQU $050
BLTBPTH     EQU $04C
BLTDPTH     EQU $054
BLTAMOD     EQU $064
BLTBMOD     EQU $062
BLTDMOD     EQU $066
BLTSIZE     EQU $058

; CIA A for Mouse Button (Exit)
CIAA_PRA    EQU $BFE001

Start:
    ; 1. Initialization
    lea     CUSTOM,a6
    
    ; Save old DMA state for clean exit
    move.w  DMACONR(a6),d7
    or.w    #$8000,d7

    ; Disable interrupts and DMA
    move.w  #$7FFF,INTENA(a6)
    move.w  #$7FFF,INTREQ(a6)
    move.w  #$7FFF,DMACON(a6)

    ; 2. Set up Copper list
    lea     CopperList,a0
    move.l  a0,COP1LCH(a6)
    move.w  #$8080,DMACON(a6)   ; Enable Copper DMA
    move.w  #$8200,DMACON(a6)   ; Enable Master DMA
    move.w  #$8040,DMACON(a6)   ; Enable Blitter DMA

MainLoop:
    ; 3. Wait for Vertical Blank
    bsr.s   WaitVBlank

    ; 4. Update Frame Counter
    addq.l  #1,FrameCounter
    
    ; 5. Blitter Test: Complex Blit (A XOR B -> D)
    ; This provides source/dest tracing and logic function verification
    bsr.s   DoBlitOperation

    ; 6. Disassembly Profiler Test: Heavy hotspot
    move.w  #15,d0
.prof_loop:
    bsr.s   ProfileTask
    dbf     d0,.prof_loop

    ; 7. Frame Scrubber Test: Update DynamicColor
    move.l  FrameCounter,d0
    lsr.l   #2,d0
    and.w   #$000F,d0
    move.w  d0,d1
    lsl.w   #4,d1
    or.w    d1,d0
    lsl.w   #4,d1
    or.w    d1,d0
    move.w  d0,DynamicColorValue

    ; 8. Exit Check: Check Left Mouse Button
    btst    #6,CIAA_PRA
    bne.s   MainLoop

    ; --- Cleanup and Exit ---
    move.w  #$7FFF,DMACON(a6)   ; Disable all DMA
    move.l  #$00000000,COP1LCH(a6)
    move.w  d7,DMACON(a6)       ; Restore old DMA
    move.w  #$C000,INTENA(a6)   ; Re-enable Master Interrupts
    moveq   #0,d0
    rts

WaitVBlank:
.wait:
    move.l  VHPOSR(a6),d0
    and.l   #$1FF00,d0
    cmp.l   #300<<8,d0
    bne.s   .wait
    rts

ProfileTask:
    move.w  #100,d1
.loop:
    mulu.w  d1,d1
    subq.w  #1,d1
    bpl.s   .loop
    rts

DoBlitOperation:
    ; Wait for previous blit
.wait_blit:
    btst    #6,DMACONR(a6)
    bne.s   .wait_blit

    ; Multi-source blit: D = A XOR B
    ; CON0: $BC5A (Use A, B, D channels; Minterm $5A)
    move.w  #$BC5A,BLTCON0(a6)
    move.w  #$0000,BLTCON1(a6)
    move.w  #$FFFF,BLTAFWM(a6)
    move.w  #$FFFF,BLTALWM(a6)
    
    lea     SourceA,a0
    move.l  a0,BLTAPTH(a6)
    lea     SourceB,a0
    move.l  a0,BLTBPTH(a6)
    lea     BlitBuffer,a0
    move.l  a0,BLTDPTH(a6)
    
    move.w  #0,BLTAMOD(a6)
    move.w  #0,BLTBMOD(a6)
    move.w  #0,BLTDMOD(a6)
    
    ; 32 lines, 2 words wide
    move.w  #(32<<6)|(2),BLTSIZE(a6)
    rts

    cnop 0,4
FrameCounter:
    dc.l    0

; --- Chip RAM Data ---

    section "ChipData",data_c

    cnop 0,4
CopperList:
    dc.w    $0100,$0200
    dc.w    $008e,$2c81
    dc.w    $0090,$f4c1
    dc.w    $0092,$0038
    dc.w    $0094,$00d0
    dc.w    $0180,$0000
    
    dc.w    $3c07,$fffe, $0180,$0F00
    dc.w    $4c07,$fffe, $0180,$0FF0
    dc.w    $5c07,$fffe, $0180,$00F0
    dc.w    $6c07,$fffe, $0180,$00FF
    dc.w    $7c07,$fffe, $0180,$000F
    dc.w    $8c07,$fffe, $0180,$0F0F

    dc.w    $ac07,$fffe
    dc.w    $0180
DynamicColorValue:
    dc.w    $0555

    dc.w    $ffdf,$fffe
    dc.w    $2c07,$fffe
    dc.w    $0180,$0000
    dc.w    $ffff,$fffe

    cnop 0,4
SourceA:
    dcb.w   64,$AAAA            ; Alternating bits
SourceB:
    dcb.w   64,$CCCC            ; Alternating bit pairs
BlitBuffer:
    dcb.b   2048,0              ; Destination
