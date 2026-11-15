/* Stand-ins at TE addresses for the l1dump bench (assembled per address, -defsym WHICH=n).
 * The TE function stand-ins forward to C functions in driver.c (same ABI; P1 is scratch);
 * the hook targets carry their real first instruction(s) so the trampolines can replay them. */
        .section .text
        .global _stub
_stub:
.if WHICH == 0                  /* 0x019d2d1e boot_done target: empty in TE */
        RTS;
.endif
.if WHICH == 1                  /* 0x019d2d1c main_tick target: empty in TE */
        RTS;
.endif
.if WHICH == 2                  /* 0x019b2674 TE frame render(buf): nothing to draw */
        RTS;
.endif
.if WHICH == 3                  /* 0x019b6c74 on_key: real entry bytes, the hook replays them */
        LINK 0x0;
        UNLINK;
        RTS;
.endif
.if WHICH == 4                  /* 0x01a5e3b0 TE_NEW(size) -> driver te_new */
        P1.L = TE_NEW;  P1.H = TE_NEW;
        JUMP (P1);
.endif
.if WHICH == 5                  /* 0x01aa6e10 TE_MEMCPY(dst, src, n) -> driver te_memcpy */
        P1.L = TE_MEMCPY;  P1.H = TE_MEMCPY;
        JUMP (P1);
.endif
.if WHICH == 6                  /* 0x01aa185c TE_DCACHE_FLUSH(start, end, inv) -> driver te_flush */
        P1.L = TE_FLUSH;  P1.H = TE_FLUSH;
        JUMP (P1);
.endif
