; Deterministic target for the Peekaboo validation runner. The host resolves all xdef labels
; through the sibling ELF and stops on checkpoint labels before their first instruction executes.

        section code,code
        xdef    _start
        xdef    ptv_checkpoint_cpu
        xdef    ptv_checkpoint_copper
        xdef    ptv_checkpoint_blitter
        xdef    ptv_checkpoint_alignment
        xdef    blit_d

CUSTOM          equ     $dff000
COLOR00         equ     $180
COP1LCH         equ     $080
DMACON          equ     $096
BLTCON0         equ     $040
BLTCON1         equ     $042
BLTAFWM         equ     $044
BLTALWM         equ     $046
BLTAPTH         equ     $050
BLTBPTH         equ     $04c
BLTDPTH         equ     $054
BLTSIZE         equ     $058
BLTCMOD         equ     $060
BLTBMOD         equ     $062
BLTAMOD         equ     $064
BLTDMOD         equ     $066

M_COMMAND_SEQ   equ     12
M_COMMAND       equ     16
M_SCENARIO      equ     18
M_CHECKPOINT_SEQ equ    20
M_CHECKPOINT    equ     24
M_STATUS        equ     26
M_GUEST_FRAME   equ     28

CMD_RUN         equ     3
CMD_CONTINUE    equ     4
CMD_EXIT        equ     5
STATUS_READY    equ     1
STATUS_RUNNING  equ     2
STATUS_PASS     equ     3
STATUS_DONE     equ     5

_start:
        lea     ptv_mailbox,a5
        move.w  #STATUS_READY,M_STATUS(a5)
poll:
        cmp.w   #CMD_EXIT,M_COMMAND(a5)
        beq.s   exit
        cmp.w   #CMD_RUN,M_COMMAND(a5)
        bne.s   poll
        move.w  #STATUS_RUNNING,M_STATUS(a5)
        move.w  M_SCENARIO(a5),d0
        cmp.w   #1,d0
        beq     scenario_cpu
        cmp.w   #2,d0
        beq     scenario_copper
        cmp.w   #3,d0
        beq     scenario_blitter
        cmp.w   #4,d0
        beq     scenario_alignment
        bra     publish

publish:
        addq.l  #1,M_CHECKPOINT_SEQ(a5)
        move.w  #STATUS_PASS,M_STATUS(a5)
wait_continue:
        cmp.w   #CMD_CONTINUE,M_COMMAND(a5)
        bne.s   wait_continue
        clr.w   M_COMMAND(a5)
        move.w  #STATUS_READY,M_STATUS(a5)
        bra     poll
exit:
        move.w  #STATUS_DONE,M_STATUS(a5)
        moveq   #0,d0
        rts

scenario_cpu:
        move.w  #1,M_CHECKPOINT(a5)
        addq.l  #1,M_CHECKPOINT_SEQ(a5)
        move.w  #STATUS_PASS,M_STATUS(a5)
        move.l  #$01020304,d0
        move.l  #$11121314,d1
        move.l  #$21222324,d2
        move.l  #$31323334,d3
        move.l  #$41424344,d4
        move.l  #$51525354,d5
        move.l  #$61626364,d6
        move.l  #$71727374,d7
ptv_checkpoint_cpu:
        bra     wait_continue

scenario_copper:
        lea     CUSTOM,a6
        lea     copper_list,a0
        move.l  a0,COP1LCH(a6)
        move.w  #$8280,DMACON(a6)
.wait_old_frame:
        move.w  6(a6),d0
        lsr.w   #8,d0
        cmp.w   #$90,d0
        blo.s   .wait_old_frame
.wait_new_frame:
        move.w  6(a6),d0
        lsr.w   #8,d0
        cmp.w   #$20,d0
        bhs.s   .wait_new_frame
.wait_copper:
        move.w  6(a6),d0
        lsr.w   #8,d0
        cmp.w   #$90,d0
        blo.s   .wait_copper
        move.w  #2,M_CHECKPOINT(a5)
        addq.l  #1,M_CHECKPOINT_SEQ(a5)
        move.w  #STATUS_PASS,M_STATUS(a5)
ptv_checkpoint_copper:
        bra     wait_continue

scenario_blitter:
        lea     CUSTOM,a6
.busy: move.w  2(a6),d0
        btst    #14,d0
        bne.s   .busy
        move.w  #$8240,DMACON(a6)       ; Set master and blitter DMA
        move.w  #$0d3c,BLTCON0(a6)      ; USEA | USEB | USED, A XOR B minterm
        move.w  #0,BLTCON1(a6)
        move.w  #$ffff,BLTAFWM(a6)
        move.w  #$ffff,BLTALWM(a6)
        clr.w   BLTAMOD(a6)
        clr.w   BLTBMOD(a6)
        clr.w   BLTDMOD(a6)
        lea     blit_a,a0
        move.l  a0,BLTAPTH(a6)
        lea     blit_b,a0
        move.l  a0,BLTBPTH(a6)
        lea     blit_d,a0
        move.l  a0,BLTDPTH(a6)
        move.w  #$0041,BLTSIZE(a6)
.wait_done:
        move.w  2(a6),d0
        btst    #14,d0
        bne.s   .wait_done
        move.w  #3,M_CHECKPOINT(a5)
        addq.l  #1,M_CHECKPOINT_SEQ(a5)
        move.w  #STATUS_PASS,M_STATUS(a5)
ptv_checkpoint_blitter:
        bra     wait_continue

scenario_alignment:
        lea     CUSTOM,a6
        addq.l  #1,M_GUEST_FRAME(a5)
        move.w  #$00f,COLOR00(a6)
        move.w  #4,M_CHECKPOINT(a5)
        addq.l  #1,M_CHECKPOINT_SEQ(a5)
        move.w  #STATUS_PASS,M_STATUS(a5)
ptv_checkpoint_alignment:
        bra     wait_continue

        section "ValidationChipData",data_c
        xdef    ptv_mailbox
ptv_mailbox:
        dc.l    $50545631
        dc.w    1,44
        dc.l    $20260812
        dc.l    0
        dc.w    0,0
        dc.l    0
        dc.w    0,0
        dc.l    0,0,0,0

copper_list:
        dc.w    $2c01,$fffe
        dc.w    COLOR00,$000f
        dc.w    $8001,$fffe
        dc.w    COLOR00,$0f00
        dc.w    $ffff,$fffe
blit_a: dc.w    $aaaa
blit_b: dc.w    $cccc
blit_d: dc.w    0
