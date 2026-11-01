/* bar_tasklog: resident timing logger for Beetle Adventure Racing (J)
 *
 * Loaded by the patched entry point to 0x80700000 (needs the Expansion Pak; the game never touches the
 * upper 4 MiB). Hooks four libultra functions in the boot segment and logs, with the COP0 Count:
 *   osSpTaskLoad(tp)      kind 1, payload = task type << 8 | task flags
 *   osSpTaskStartGo       kind 2 (the SP_STATUS write that clears HALT)
 *   send_mesg(event*8)    kind 3, payload = event << 8 | extra (SP: SP_STATUS bits, DP: DPC_STATUS, VI: line/2)
 *   osSpTaskYield         kind 4 (the SP_STATUS write that sets SIG0)
 * Entry: bits 31..16 = Count delta since the previous entry / 16 (saturating), 15..12 kind, 11..0 payload.
 * v3: only the graphics task's load/go are logged (the audio task's are not), only the SP/DP/VI events are
 * logged, and the SP event carries the type of the task that was loaded last in bits 5..6 of the extra byte
 * (bits 0..4: halt, broke, sig0 = yield requested, sig1 = yielded, sig2 = done). Logging starts at the first
 * graphics task that takes more than 0.5 VI (the intro; the title frames take 0.02 VI) after the 1000th VI,
 * and when the 32 KiB buffer is full it is written to the cartridge SRAM (0x08000000) and the console hangs
 * (also at the 7000th VI if the trigger never fired, so a run always ends visibly).
 */

#define STATE       0xA0700F00
#define ST_ENABLED  0
#define ST_WPTR     4
#define ST_LAST     8
#define ST_VICOUNT  12
#define ST_SAVE6    16
#define ST_SAVE7    20
#define ST_SAVE8    24
#define ST_CURTYPE  28
#define ST_GO       32
#define ST_DUMPED   36
#define LOGBUF      0xA0710000
#define LOGBYTES    0x8000
#define MIN_VI      1000
#define END_VI      7000
#define TRIG_TICKS  390625

#define SP_STATUS   0xA4040010
#define DPC_STATUS  0xA410000C
#define VI_CURRENT  0xA4400010

    .set noreorder
    .set noat
    .section .text
    .globl _start
_start:
init:
    /* original entry: clear bss 0x80022670 .. +0xFB20, sp = 0x80022778, jump to 0x80004c60 */
    lui     $t0, 0x8002
    addiu   $t0, $t0, 0x2670
    ori     $t1, $zero, 0xfb20
1:  addiu   $t1, $t1, -8
    sw      $zero, 0($t0)
    sw      $zero, 4($t0)
    bnez    $t1, 1b
    addiu   $t0, $t0, 8

    /* state */
    lui     $t0, %hi(STATE)
    ori     $t0, $t0, %lo(STATE)
    sw      $zero, ST_ENABLED($t0)
    sw      $zero, ST_WPTR($t0)
    sw      $zero, ST_VICOUNT($t0)
    sw      $zero, ST_CURTYPE($t0)
    sw      $zero, ST_DUMPED($t0)

    /* hooks: j/jal into this blob */
    lui     $t1, 0x8000
    /* osSpTaskLoad 0x80005B8C: j hookA ; nop */
    la      $t2, hookA
    srl     $t2, $t2, 2
    lui     $t3, 0x0fff
    ori     $t3, $t3, 0xffff
    and     $t2, $t2, $t3
    lui     $t3, 0x0800
    or      $t2, $t2, $t3
    sw      $t2, 0x5b8c($t1)
    sw      $zero, 0x5b90($t1)
    cache   0x19, 0x5b8c($t1)
    cache   0x10, 0x5b8c($t1)
    /* send_mesg 0x8000629C: j hookC ; nop */
    la      $t2, hookC
    srl     $t2, $t2, 2
    lui     $t3, 0x0fff
    ori     $t3, $t3, 0xffff
    and     $t2, $t2, $t3
    lui     $t3, 0x0800
    or      $t2, $t2, $t3
    sw      $t2, 0x629c($t1)
    sw      $zero, 0x62a0($t1)
    cache   0x19, 0x629c($t1)
    cache   0x10, 0x629c($t1)
    /* osSpTaskStartGo 0x80005D1C: jal hookB (a0 = 0x125 in the delay slot) */
    la      $t2, hookB
    srl     $t2, $t2, 2
    lui     $t3, 0x0fff
    ori     $t3, $t3, 0xffff
    and     $t2, $t2, $t3
    lui     $t3, 0x0c00
    or      $t2, $t2, $t3
    sw      $t2, 0x5d1c($t1)
    cache   0x19, 0x5d1c($t1)
    cache   0x10, 0x5d1c($t1)
    /* osSpTaskYield 0x80007868: jal hookD (a0 = 0x400) */
    la      $t2, hookD
    srl     $t2, $t2, 2
    lui     $t3, 0x0fff
    ori     $t3, $t3, 0xffff
    and     $t2, $t2, $t3
    lui     $t3, 0x0c00
    or      $t2, $t2, $t3
    sw      $t2, 0x7868($t1)
    cache   0x19, 0x7868($t1)
    cache   0x10, 0x7868($t1)

    lui     $sp, 0x8002
    addiu   $sp, $sp, 0x2778
    lui     $t2, 0x8000
    addiu   $t2, $t2, 0x4c60
    jr      $t2
    nop

/* ---- log_entry: $t0 = kind<<12 | payload; clobbers t2-t7; ra preserved by the caller in t8 ---- */
log_entry:
    lui     $t2, %hi(STATE)
    ori     $t2, $t2, %lo(STATE)
    lw      $t3, ST_ENABLED($t2)
    nop
    beqz    $t3, 9f
    nop
    mfc0    $t4, $9
    lw      $t5, ST_LAST($t2)
    sw      $t4, ST_LAST($t2)
    subu    $t5, $t4, $t5
    srl     $t5, $t5, 4
    lui     $t6, 1
    sltu    $t7, $t5, $t6
    bnez    $t7, 1f
    nop
    ori     $t5, $zero, 0xffff
1:  sll     $t5, $t5, 16
    or      $t5, $t5, $t0
    lw      $t6, ST_WPTR($t2)
    lui     $t7, %hi(LOGBUF)
    ori     $t7, $t7, %lo(LOGBUF)
    addu    $t7, $t7, $t6
    sw      $t5, 0($t7)
    addiu   $t6, $t6, 4
    sw      $t6, ST_WPTR($t2)
    li      $t7, LOGBYTES
    bne     $t6, $t7, 9f
    nop
    j       dump
    nop
9:  jr      $ra
    nop

/* ---- hookA: osSpTaskLoad entry (replaced: addiu sp,sp,-40 ; sw ra,36(sp)) ---- */
hookA:
    addiu   $sp, $sp, -40
    sw      $ra, 36($sp)
    lw      $t0, 0($a0)         /* type */
    lw      $t1, 4($a0)         /* flags */
    lui     $t2, %hi(STATE)
    ori     $t2, $t2, %lo(STATE)
    sw      $t0, ST_CURTYPE($t2)
    ori     $t3, $zero, 1
    bne     $t0, $t3, 1f
    nop
    sll     $t0, $t0, 8
    andi    $t1, $t1, 0xff
    or      $t0, $t0, $t1
    andi    $t0, $t0, 0x0fff
    ori     $t0, $t0, 0x1000
    jal     log_entry
    nop
1:  lw      $ra, 36($sp)
    lui     $t0, 0x8000
    ori     $t0, $t0, 0x5b94
    jr      $t0
    nop

/* ---- hookB: replaces the jal __osSpSetStatus in osSpTaskStartGo (a0 = 0x125) ---- */
hookB:
    move    $t8, $ra
    lui     $t2, %hi(STATE)
    ori     $t2, $t2, %lo(STATE)
    lw      $t3, ST_CURTYPE($t2)
    ori     $t4, $zero, 1
    bne     $t3, $t4, 1f
    nop
    mfc0    $t4, $9
    sw      $t4, ST_GO($t2)
    ori     $t0, $zero, 0x2000
    jal     log_entry
    nop
1:  move    $ra, $t8
    lui     $t6, 0xa404
    sw      $a0, 0x10($t6)
#ifdef IDLE_CPU
    /* experiment: keep the CPU off the bus while the graphics task runs (spin on SP_STATUS until the RSP
     * halts; the scheduler thread is blocked, so the audio task cannot yield the graphics task) */
    lw      $t3, ST_CURTYPE($t2)
    ori     $t4, $zero, 1
    bne     $t3, $t4, 3f
    nop
    lw      $t3, ST_ENABLED($t2)
    nop
    beqz    $t3, 3f
    nop
    /* the status write takes effect a little later on the console: wait 0.01 VI on the Count first, then
     * for HALT to be set again; log the end of the wait (kind 5) */
    mfc0    $t3, $9
    lui     $t4, 0x0000
    ori     $t4, $t4, 7812
    addu    $t3, $t3, $t4
4:  mfc0    $t4, $9
    subu    $t4, $t4, $t3
    bltz    $t4, 4b
    nop
2:  lw      $t4, 0x10($t6)
    nop
    andi    $t4, $t4, 1
    beqz    $t4, 2b
    nop
    move    $t8, $ra
    ori     $t0, $zero, 0x5000
    jal     log_entry
    nop
    move    $ra, $t8
3:
#endif
    jr      $ra
    nop

/* ---- hookD: replaces the jal __osSpSetStatus in osSpTaskYield (a0 = 0x400) ---- */
hookD:
    move    $t8, $ra
    ori     $t0, $zero, 0x4000
    jal     log_entry
    nop
    move    $ra, $t8
    lui     $t6, 0xa404
    jr      $ra
    sw      $a0, 0x10($t6)

/* ---- hookC: send_mesg entry in the exception handler (a0 = event*8); replaced: lui t2,0x8003 ; addiu t2,t2,0xc70 ---- */
hookC:
    lui     $t2, %hi(STATE)
    ori     $t2, $t2, %lo(STATE)
    sw      $t6, ST_SAVE6($t2)
    sw      $t7, ST_SAVE7($t2)
    sw      $t8, ST_SAVE8($t2)
    move    $t8, $ra
    srl     $t0, $a0, 3         /* event */
    sll     $t0, $t0, 8
    ori     $t0, $t0, 0x3000
    /* extra byte per event */
    ori     $t3, $zero, 0x20
    bne     $a0, $t3, 2f
    ori     $t3, $zero, 0x48
    lui     $t4, %hi(SP_STATUS)
    lw      $t4, %lo(SP_STATUS)($t4)
    nop
    andi    $t5, $t4, 0x3       /* halt, broke */
    srl     $t4, $t4, 5
    andi    $t4, $t4, 0x1c      /* sig0 (yield), sig1 (yielded), sig2 (task done) */
    or      $t5, $t5, $t4
    lw      $t4, ST_CURTYPE($t2)
    nop
    andi    $t4, $t4, 0x3
    sll     $t4, $t4, 5
    or      $t5, $t5, $t4
    or      $t0, $t0, $t5
    /* trigger: a done graphics task that ran more than TRIG_TICKS, after MIN_VI */
    lw      $t4, ST_ENABLED($t2)
    nop
    bnez    $t4, 5f
    andi    $t4, $t5, 0x70      /* type 1 (bits 5..6 = 01) and sig2 */
    ori     $t6, $zero, 0x30
    bne     $t4, $t6, 5f
    nop
    lw      $t4, ST_VICOUNT($t2)
    ori     $t6, $zero, MIN_VI
    sltu    $t6, $t4, $t6
    bnez    $t6, 5f
    nop
    mfc0    $t6, $9
    lw      $t7, ST_GO($t2)
    nop
    subu    $t6, $t6, $t7
    lui     $t7, %hi(TRIG_TICKS)
    ori     $t7, $t7, %lo(TRIG_TICKS)
    sltu    $t7, $t6, $t7
    bnez    $t7, 5f
    nop
    b       enable
    nop
2:  bne     $a0, $t3, 3f
    ori     $t3, $zero, 0x38
    lui     $t4, %hi(DPC_STATUS)
    lw      $t4, %lo(DPC_STATUS)($t4)
    nop
    andi    $t4, $t4, 0xff
    b       5f
    or      $t0, $t0, $t4
3:  bne     $a0, $t3, 6f
    nop
    /* VI: count frames; at END_VI dump whatever was logged */
    lw      $t4, ST_VICOUNT($t2)
    lui     $t5, %hi(VI_CURRENT)
    lw      $t5, %lo(VI_CURRENT)($t5)
    addiu   $t4, $t4, 1
    sw      $t4, ST_VICOUNT($t2)
    srl     $t5, $t5, 1
    andi    $t5, $t5, 0xff
    or      $t0, $t0, $t5
    ori     $t5, $zero, END_VI
    bne     $t4, $t5, 5f
    nop
    j       dump
    nop
6:  /* any other event: not logged */
    b       7f
    nop
enable:
    /* header = magic, VI count, Count, version */
    lw      $t4, ST_VICOUNT($t2)
    lui     $t6, %hi(LOGBUF)
    ori     $t6, $t6, %lo(LOGBUF)
    lui     $t7, 0x4241
    ori     $t7, $t7, 0x524c
    sw      $t7, 0($t6)
    sw      $t4, 4($t6)
    mfc0    $t7, $9
    sw      $t7, 8($t6)
#ifdef IDLE_CPU
    ori     $t7, $zero, 4
#else
    ori     $t7, $zero, 3
#endif
    sw      $t7, 12($t6)
    mfc0    $t7, $9
    sw      $t7, ST_LAST($t2)
    ori     $t7, $zero, 16
    sw      $t7, ST_WPTR($t2)
    ori     $t7, $zero, 1
    sw      $t7, ST_ENABLED($t2)
5:  jal     log_entry
    nop
7:  move    $ra, $t8
    lui     $t2, %hi(STATE)
    ori     $t2, $t2, %lo(STATE)
    lw      $t6, ST_SAVE6($t2)
    lw      $t7, ST_SAVE7($t2)
    lw      $t8, ST_SAVE8($t2)
    lui     $t2, 0x8003
    addiu   $t2, $t2, 0x0c70
    lui     $t3, 0x8000
    ori     $t3, $t3, 0x62a4
    jr      $t3
    nop

/* ---- dump: write the buffer to SRAM (3 x 32 KiB, last chunk first) and hang ---- */
dump:
    mfc0    $t0, $12
    li      $t1, ~1
    and     $t0, $t0, $t1
    mtc0    $t0, $12            /* interrupts off */
    nop
    nop
    lui     $t0, 0xa460
    /* domain 2 timing for SRAM */
    ori     $t1, $zero, 5
    sw      $t1, 0x24($t0)
    ori     $t1, $zero, 0x0c
    sw      $t1, 0x28($t0)
    ori     $t1, $zero, 0x0d
    sw      $t1, 0x2c($t0)
    ori     $t1, $zero, 2
    sw      $t1, 0x30($t0)
    ori     $t4, $zero, 0       /* one 32 KiB chunk */
1:  lw      $t1, 0x10($t0)
    nop
    andi    $t1, $t1, 3
    bnez    $t1, 1b
    nop
    sll     $t5, $t4, 15        /* chunk * 32 KiB */
    lui     $t1, 0x0071
    addu    $t1, $t1, $t5
    sw      $t1, 0x00($t0)      /* PI_DRAM_ADDR */
    lui     $t1, 0x0800
    addu    $t1, $t1, $t5
    sw      $t1, 0x04($t0)      /* PI_CART_ADDR */
    ori     $t1, $zero, 0x7fff
    sw      $t1, 0x08($t0)      /* PI_RD_LEN: RDRAM -> cart */
2:  lw      $t1, 0x10($t0)
    nop
    andi    $t1, $t1, 3
    bnez    $t1, 2b
    nop
    bnez    $t4, 1b
    addiu   $t4, $t4, -1
    /* done marker for an emulator watch */
    lui     $t2, %hi(STATE)
    ori     $t2, $t2, %lo(STATE)
    lui     $t3, 0xd0d0
    sw      $t3, ST_ENABLED($t2)
3:  b       3b
    nop
