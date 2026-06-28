    .section .rodata.jit_lx7_mov_rr, "a", @progbits
    .align 4
    .global jit_lx7_mov_rr_a3_a10
    .type jit_lx7_mov_rr_a3_a10, @object

/*
 * 8x8 table of assembler-encoded MOV.N instructions for the JIT x86 GPR
 * register range.  Rows are destination a3..a10, columns are source a3..a10.
 */
jit_lx7_mov_rr_a3_a10:
    mov.n a3, a3
    mov.n a3, a4
    mov.n a3, a5
    mov.n a3, a6
    mov.n a3, a7
    mov.n a3, a8
    mov.n a3, a9
    mov.n a3, a10

    mov.n a4, a3
    mov.n a4, a4
    mov.n a4, a5
    mov.n a4, a6
    mov.n a4, a7
    mov.n a4, a8
    mov.n a4, a9
    mov.n a4, a10

    mov.n a5, a3
    mov.n a5, a4
    mov.n a5, a5
    mov.n a5, a6
    mov.n a5, a7
    mov.n a5, a8
    mov.n a5, a9
    mov.n a5, a10

    mov.n a6, a3
    mov.n a6, a4
    mov.n a6, a5
    mov.n a6, a6
    mov.n a6, a7
    mov.n a6, a8
    mov.n a6, a9
    mov.n a6, a10

    mov.n a7, a3
    mov.n a7, a4
    mov.n a7, a5
    mov.n a7, a6
    mov.n a7, a7
    mov.n a7, a8
    mov.n a7, a9
    mov.n a7, a10

    mov.n a8, a3
    mov.n a8, a4
    mov.n a8, a5
    mov.n a8, a6
    mov.n a8, a7
    mov.n a8, a8
    mov.n a8, a9
    mov.n a8, a10

    mov.n a9, a3
    mov.n a9, a4
    mov.n a9, a5
    mov.n a9, a6
    mov.n a9, a7
    mov.n a9, a8
    mov.n a9, a9
    mov.n a9, a10

    mov.n a10, a3
    mov.n a10, a4
    mov.n a10, a5
    mov.n a10, a6
    mov.n a10, a7
    mov.n a10, a8
    mov.n a10, a9
    mov.n a10, a10

    .size jit_lx7_mov_rr_a3_a10, . - jit_lx7_mov_rr_a3_a10
