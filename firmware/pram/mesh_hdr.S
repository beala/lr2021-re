; LR2021 PRAM blob: MeshCore repeater on the chip (mesh.c, selfspi.c).
; Same blob format as sweep.S: magic, entry count, entry table; entry 0 runs at activation.
        .arc_attribute Tag_ARC_ABI_rf16, 1
        .section .text
        .global _start
_start:
        .word   0x600DB002              ; magic
        .word   0x00000003              ; 3 entries
        .word   install                 ; 0
        .word   rpt_cmd                 ; 1 (mesh.c): repeater
        .word   hello                   ; 2
        .word   0

install:
        mov_s   r0, 2
        j_s     [blink]

; 2: returns 'M' 'S' 'H' '1'
hello:
        ld_s    r3, [r2, 4]
        mov_s   r12, 0x4D
        stb_s   r12, [r3, 0]
        mov_s   r12, 0x53
        stb_s   r12, [r3, 1]
        mov_s   r12, 0x48
        stb_s   r12, [r3, 2]
        mov_s   r12, 0x31
        stb_s   r12, [r3, 3]
        mov_s   r12, 4
        st_s    r12, [r2, 0]
        mov_s   r0, 3
        j_s     [blink]
