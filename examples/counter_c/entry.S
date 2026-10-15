/* Trampoline for the C hook: save every register the C function may clobber but the
 * TE code needs, call count_lookup(name) in counter.c, restore, tail-jump to the
 * original TE function (RETS still points back into SeqManager).
 * R0 at entry = const te_string *name (the argument the TE call site passes).
 * Assembly calling C must reserve the 12-byte outgoing arg area (see te246.h). */

        .section .text.entry
        .global _hook_seqids
_hook_seqids:
        [--SP] = RETS;
        [--SP] = P1;
        [--SP] = P0;
        [--SP] = R2;
        [--SP] = R1;
        [--SP] = R0;
        SP += -12;
        CALL _count_lookup;
        SP += 12;
        R0 = [SP++];
        R1 = [SP++];
        R2 = [SP++];
        P0 = [SP++];
        P1 = [SP++];
        RETS = [SP++];
        JUMP.L ORIG_SEQIDS
