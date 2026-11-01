; LR2021 PRAM blob: text on a waterfall (text.c). Same blob format as sweep.S.
        .arc_attribute Tag_ARC_ABI_rf16, 1
        .section .text
        .global _start
_start:
        .word   0x600DB002              ; magic
        .word   0x00000003              ; 3 entries
        .word   install                 ; 0
        .word   text_cmd                ; 1 (text.c): draw one scanline of text
        .word   hello                   ; 2
        .word   0

install:
        mov_s   r0, 2
        j_s     [blink]

; 2: returns 'T' 'X' 'T' '1'
hello:
        ld_s    r3, [r2, 4]
        mov_s   r12, 0x54
        stb_s   r12, [r3, 0]
        mov_s   r12, 0x58
        stb_s   r12, [r3, 1]
        mov_s   r12, 0x54
        stb_s   r12, [r3, 2]
        mov_s   r12, 0x31
        stb_s   r12, [r3, 3]
        mov_s   r12, 4
        st_s    r12, [r2, 0]
        mov_s   r0, 3
        j_s     [blink]
