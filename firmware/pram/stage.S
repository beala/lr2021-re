; One radix-2 DIT stage of the 256-point FFT, hand-scheduled for the 16-entry register file.
;
;   void fft_stage(s16 *w, u32 half, u32 sh, const s16 *tw);
;     w:   256 complex points, interleaved {s16 re, s16 im} (4 bytes each), in place
;     half: butterfly span of this stage (4..128); twiddle step = count per k = 128 / half
;     sh:  0 or 1, right shift applied to both outputs (block floating point)
;     tw:  128 x {s16 cos, s16 -sin}, Q15
;
; For k = 0..half-1 (twiddle w = tw[k * 128/half]) and i = k, k+2*half, ...:
;   t = b * w >> 15;  a' = (a + t) >> sh;  b' = (a - t) >> sh     (a = w[i], b = w[i + half])
; GCC spills these values to the stack (only r0-r3, r10-r15, r26-r31 exist), so the inner loop
; is written here: 12 registers and the zero-overhead loop (lp). The four multiplies are issued
; back to back and the a values are loaded while they complete; unscaled stages skip the shifts.
;
; Registers in the inner loop:
;   r0 pa   r1 pb   r2 br/a0   r3 bi/a1   r10 tr   r11/r12/r30 products and results
;   r13 wr  r14 wi  r15 sh     blink stride (8 * half bytes)  lp_count = 128 / half
; blink is free to use: it's saved by enter_s and restored by leave_s, and nothing is called.
; (fp can't be used: enter_s/leave_s with fp make it the frame pointer and restore sp from it.)
; Stack frame (after enter_s): [sp+0] w, [sp+4] half, [sp+8] k, [sp+12] tw pointer,
;   [sp+16] tw step in bytes (4 * 128/half), [sp+20] count (128/half)

        .arc_attribute Tag_ARC_ABI_rf16, 1     ; 16-entry register file, matches fft.c (-mrf16)
        .section .text
        .global fft_stage
        .align  4
fft_stage:
        enter_s [r13-r15, blink]
        sub     sp, sp, 24
        st      r0, [sp, 0]
        st      r1, [sp, 4]
        mov     r15, r2                 ; sh
        st      r3, [sp, 12]            ; tw pointer
        mov     r12, 128
        ; count = 128 / half: half is a power of two, so shift by log2(half)
        ffs     r10, r1                 ; index of the lowest set bit = log2(half)
        lsr     r12, r12, r10
        st      r12, [sp, 20]
        asl     r12, r12, 2
        st      r12, [sp, 16]           ; tw step in bytes
        asl     blink, r1, 3            ; stride = 2 * half points * 4 bytes
        mov     r12, 0
        st      r12, [sp, 8]            ; k = 0
.Lk:
        ld      r12, [sp, 12]
        ldh.x   r13, [r12, 0]           ; wr
        ldh.x   r14, [r12, 2]           ; wi
        ld      r10, [sp, 16]
        add     r12, r12, r10
        st      r12, [sp, 12]
        ld      r0, [sp, 0]
        ld      r12, [sp, 8]
        add2    r0, r0, r12             ; pa = w + 4k
        ld      r1, [sp, 4]
        add2    r1, r0, r1              ; pb = pa + 4 * half
        ld      r12, [sp, 20]
        mov     lp_count, r12
        brne    r15, 0, .Lscaled
        ; ---- unscaled butterflies: 22 instructions; multiplies issued together, a loaded meanwhile
        lp      .Lend0
        ldh.x   r2, [r1, 0]             ; br
        ldh.x   r3, [r1, 2]             ; bi
        mpyw    r10, r2, r13            ; br*wr
        mpyw    r11, r3, r14            ; bi*wi
        mpyw    r12, r2, r14            ; br*wi
        mpyw    r30, r3, r13            ; bi*wr
        ldh.x   r2, [r0, 0]             ; a0
        ldh.x   r3, [r0, 2]             ; a1
        sub     r10, r10, r11
        asr     r10, r10, 15            ; tr
        add     r12, r12, r30
        asr     r12, r12, 15            ; ti
        add     r11, r2, r10
        sub     r2, r2, r10
        add     r30, r3, r12
        sub     r3, r3, r12
        sth     r11, [r0, 0]
        sth     r30, [r0, 2]
        sth     r2, [r1, 0]
        sth     r3, [r1, 2]
        add     r0, r0, blink
        add     r1, r1, blink
.Lend0:
        b       .Lnextk
.Lscaled:
        ; ---- scaled butterflies (outputs >> 1): 26 instructions
        lp      .Lend1
        ldh.x   r2, [r1, 0]
        ldh.x   r3, [r1, 2]
        mpyw    r10, r2, r13
        mpyw    r11, r3, r14
        mpyw    r12, r2, r14
        mpyw    r30, r3, r13
        ldh.x   r2, [r0, 0]
        ldh.x   r3, [r0, 2]
        sub     r10, r10, r11
        asr     r10, r10, 15
        add     r12, r12, r30
        asr     r12, r12, 15
        add     r11, r2, r10
        sub     r2, r2, r10
        add     r30, r3, r12
        sub     r3, r3, r12
        asr     r11, r11, 1
        asr     r2, r2, 1
        asr     r30, r30, 1
        asr     r3, r3, 1
        sth     r11, [r0, 0]
        sth     r30, [r0, 2]
        sth     r2, [r1, 0]
        sth     r3, [r1, 2]
        add     r0, r0, blink
        add     r1, r1, blink
.Lend1:
.Lnextk:
        ld      r12, [sp, 8]
        add     r12, r12, 1
        st      r12, [sp, 8]
        ld      r10, [sp, 4]
        cmp     r12, r10
        blo     .Lk
        add     sp, sp, 24
        leave_s [r13-r15, blink, pcl]

; 1,000,000 iterations of a one-NOP zero-overhead loop: 1 cycle each (benchmark, entry 10)
        .global bench_nops
        .align  4                       ; bl targets must be 4-byte aligned
bench_nops:
        mov     lp_count, 1000000
        lp      .Lnops_end
        nop
.Lnops_end:
        j_s     [blink]

; Micro-benchmarks (entry 10, modes 16..22): 1,000,000 iterations of a zero-overhead loop whose
; body is: 16 = nop_s, 17 = 2 x nop_s, 18 = 2 x 32-bit add, 19 = ldh.x from 0x805C00,
; 20 = sth to 0x805C00, 21 = mpyw, 22 = mpy.     void bench_micro(u32 mode);
        .global bench_micro
        .align  4
bench_micro:
        mov     r1, 0x805C00
        mov     r12, 3
        mov     lp_count, 1000000
        breq    r0, 16, .Lm16
        breq    r0, 17, .Lm17
        breq    r0, 18, .Lm18
        breq    r0, 19, .Lm19
        breq    r0, 20, .Lm20
        breq    r0, 21, .Lm21
        breq    r0, 22, .Lm22
        j_s     [blink]
.Lm16:  lp      .Le16
        nop_s
.Le16:  j_s     [blink]
.Lm17:  lp      .Le17
        nop_s
        nop_s
.Le17:  j_s     [blink]
.Lm18:  lp      .Le18
        add     r2, r2, 0x1234
        add     r3, r3, 0x1234
.Le18:  j_s     [blink]
.Lm19:  lp      .Le19
        ldh.x   r2, [r1, 0]
.Le19:  j_s     [blink]
.Lm20:  lp      .Le20
        sth     r12, [r1, 0]
.Le20:  j_s     [blink]
.Lm21:  lp      .Le21
        mpyw    r2, r12, r12
.Le21:  j_s     [blink]
.Lm22:  lp      .Le22
        mpy     r2, r12, r12
.Le22:  j_s     [blink]
