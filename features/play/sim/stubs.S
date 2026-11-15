/* Stand-ins at TE addresses for the playprobe bench (assembled per address, -defsym FN=, PRE=).
 * Each forwards to a C function in driver.c (same ABI; P1 is scratch). PRE = 1: the real entry
 * bytes of a hooked function first (LINK 0 / UNLINK: the hook replays them, then jumps past). */
        .section .text
        .global _stub
_stub:
.if PRE
        LINK 0x0;
        UNLINK;
.endif
        P1.L = FN;  P1.H = FN;
        JUMP (P1);
