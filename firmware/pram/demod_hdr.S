; LR2021 PRAM blob: on-chip demodulation (demod.c, afsk_rx.h). Separate image from pram.bin.
; Same blob format as sweep.S: magic, entry count, entry table; entry 0 runs at activation.
        .arc_attribute Tag_ARC_ABI_rf16, 1
        .section .text
        .global _start
_start:
        .word   0x600DB002
        .word   0x00000007
        .word   install                 ; 0
        .word   peek_cmd                ; 1: read words
        .word   hello                   ; 2
        .word   poke_cmd                ; 3: write a word
        .word   aprs_cmd                ; 4: APRS / AFSK 1200 receiver, blocking
        .word   install                 ; 5: (unused)
        .word   bg_cmd                  ; 6: APRS receiver in the background (IRQ 42)
        .word   0
install:
        mov_s   r0, 2
        j_s     [blink]
; 2: returns 'D' 'E' 'M' '1'
hello:
        ld_s    r3, [r2, 4]
        mov_s   r12, 0x44
        stb_s   r12, [r3, 0]
        mov_s   r12, 0x45
        stb_s   r12, [r3, 1]
        mov_s   r12, 0x4D
        stb_s   r12, [r3, 2]
        mov_s   r12, 0x31
        stb_s   r12, [r3, 3]
        mov_s   r12, 4
        st_s    r12, [r2, 0]
        mov_s   r0, 3
        j_s     [blink]

; IRQ 42 (RX FIFO full) in the background mode: save what a C call may clobber, call bg_isr.
; IRQ_CTRL is 0, so nothing but PC and STATUS32 is saved by the hardware (the ROM's priority 1
; handlers save the same set by hand, e.g. 0x7A90).
        .global isr42
isr42:
        st.aw   blink, [sp, -4]
        st.aw   r0, [sp, -4]
        st.aw   r1, [sp, -4]
        st.aw   r2, [sp, -4]
        st.aw   r3, [sp, -4]
        st.aw   r10, [sp, -4]
        st.aw   r11, [sp, -4]
        st.aw   r12, [sp, -4]
        st.aw   r30, [sp, -4]
        lr      r0, [lp_end]
        st.aw   r0, [sp, -4]
        lr      r0, [lp_start]
        st.aw   r0, [sp, -4]
        st.aw   lp_count, [sp, -4]
        bl      bg_isr
        ld.ab   r0, [sp, 4]
        mov     lp_count, r0
        ld.ab   r0, [sp, 4]
        sr      r0, [lp_start]
        ld.ab   r0, [sp, 4]
        sr      r0, [lp_end]
        ld.ab   r30, [sp, 4]
        ld.ab   r12, [sp, 4]
        ld.ab   r11, [sp, 4]
        ld.ab   r10, [sp, 4]
        ld.ab   r3, [sp, 4]
        ld.ab   r2, [sp, 4]
        ld.ab   r1, [sp, 4]
        ld.ab   r0, [sp, 4]
        ld.ab   blink, [sp, 4]
        rtie
