; LR2021 PRAM blob: FM by writing the synthesizer directly (pll.c). Separate image from pram.bin.
; Same blob format as sweep.S: magic, entry count, entry table; entry 0 runs at activation.
        .arc_attribute Tag_ARC_ABI_rf16, 1
        .section .text
        .global _start
_start:
        .word   0x600DB002
        .word   0x00000004
        .word   install                 ; 0
        .word   pll_cmd                 ; 1: FSK / table FM / sweep (pll.c)
        .word   hello                   ; 2
        .word   afsk_cmd                ; 3: AFSK, e.g. APRS (pll.c)
        .word   0
install:
        mov_s   r0, 2
        j_s     [blink]
; 2: returns 'P' 'L' 'L' '1'
hello:
        ld_s    r3, [r2, 4]
        mov_s   r12, 0x50
        stb_s   r12, [r3, 0]
        mov_s   r12, 0x4C
        stb_s   r12, [r3, 1]
        stb_s   r12, [r3, 2]
        mov_s   r12, 0x31
        stb_s   r12, [r3, 3]
        mov_s   r12, 4
        st_s    r12, [r2, 0]
        mov_s   r0, 3
        j_s     [blink]
