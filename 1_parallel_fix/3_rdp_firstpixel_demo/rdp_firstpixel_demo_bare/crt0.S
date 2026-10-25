/* Minimal start-up: no libdragon runtime. The libdragon compatibility IPL3
 * loads the flat binary to 0x80000400 and jumps here. */
    .set noreorder
    .section .text.start
    .globl _start
_start:
    /* interrupts off */
    mfc0    $t0, $12                /* Status */
    li      $t1, ~1
    and     $t0, $t0, $t1
    mtc0    $t0, $12

    /* Invalidate both caches WITHOUT writing anything back: the IPL3 has just
     * DMA'd this program into RDRAM, and writing back any stale dirty line
     * could overwrite it. (Index_Store_Tag with a zero tag.) */
    mtc0    $zero, $28              /* TagLo */
    mtc0    $zero, $29              /* TagHi */
    lui     $t0, 0x8000
    addiu   $t1, $t0, 0x2000        /* D-cache: 8 KB, 16-byte lines */
1:  cache   0x09, 0($t0)            /* Index_Store_Tag_D */
    addiu   $t0, $t0, 16
    bne     $t0, $t1, 1b
    nop
    lui     $t0, 0x8000
    addiu   $t1, $t0, 0x4000        /* I-cache: 16 KB, 32-byte lines */
5:  cache   0x08, 0($t0)            /* Index_Store_Tag_I */
    addiu   $t0, $t0, 32
    bne     $t0, $t1, 5b
    nop

    /* clear .bss */
    la      $t0, __bss_start
    la      $t1, __bss_end
2:  beq     $t0, $t1, 3f
    nop
    sw      $zero, 0($t0)
    b       2b
    addiu   $t0, $t0, 4
3:
    li      $sp, 0x803FFFF0         /* top of 4 MB RDRAM */
    jal     main
    nop
4:  b       4b
    nop
