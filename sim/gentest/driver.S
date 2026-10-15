/* Generated-trampoline test driver (features/midi_filter). Sends three te_bytes
 * messages through the hook: note-on (passes), CC 12 (dropped by the filter: the
 * dispatcher stub must not run), pitch bend (passes). After each call it checks
 * SP/FP came back unchanged and R4-R7 are intact, then verifies counters.
 * HLT (exit 0) = pass, ABORT (exit 1) = fail, failure bits in R7:
 *   0 SP/FP after call 1   1 SP/FP after call 2 (filtered path)   2 SP/FP after call 3
 *   3 dispatcher calls==2  4 last msg == V3   5 init_runs==1   6 passed==2
 *   7 dropped==1   8 R4-R6 preserved (callee-saved)
 * Defsyms: STACK, HOOK, V1-V3, MB1-MB3, SCOUNT, SLAST, INIT, PASSED, DROPPED. */
        .section .text
        .global _start

        .macro msg v, mb, b0, b1, b2
        P1.L = \v;      P1.H = \v;
        P0.L = \mb;     P0.H = \mb;
        R1 = \b0;       B[P0] = R1;
        R1 = \b1;       B[P0 + 0x1] = R1;
        R1 = \b2;       B[P0 + 0x2] = R1;
        R2 = P0;        R2 += 0x3;
        R1 = 0;         [P1] = R1;
        [P1 + 0x4] = P0;
        [P1 + 0x8] = R2;
        [P1 + 0xc] = R2;
        R0 = P1;
        P2.L = HOOK;    P2.H = HOOK;
        CALL (P2);
        .endm

        .macro spcheck bit
        R1.L = STACK;   R1.H = STACK;
        R2 = SP;        CC = R2 == R1;  IF !CC JUMP 9f;
        R2 = FP;        CC = R2 == R1;  IF CC JUMP 8f;
9:      BITSET (R7, \bit);
8:
        .endm

        .macro expect addr, val, bit
        P1.L = \addr;   P1.H = \addr;   R2 = [P1];
        R1 = \val;      CC = R2 == R1;  IF CC JUMP 7f;
        BITSET (R7, \bit);
7:
        .endm

_start:
        SP.L = STACK;   SP.H = STACK;
        FP = SP;
        R7 = 0;
        R4 = 0x44;      R5 = 0x55;      R6 = 0x66;

        msg V1, MB1, 0x90, 60, 100
        spcheck 0
        msg V2, MB2, 0xb0, 12, 45
        spcheck 1
        msg V3, MB3, 0xe0, 0x21, 0x40
        spcheck 2

        expect SCOUNT, 2, 3
        P1.L = SLAST;   P1.H = SLAST;   R2 = [P1];
        R1.L = V3;      R1.H = V3;      CC = R2 == R1;  IF CC JUMP 1f;
        BITSET (R7, 4);
1:      expect INIT, 1, 5
        expect PASSED, 2, 6
        expect DROPPED, 1, 7
        R1 = 0x44;      CC = R4 == R1;  IF !CC JUMP 2f;
        R1 = 0x55;      CC = R5 == R1;  IF !CC JUMP 2f;
        R1 = 0x66;      CC = R6 == R1;  IF CC JUMP 3f;
2:      BITSET (R7, 8);
3:      CC = R7 == 0;
        IF !CC JUMP 4f;
        HLT;
4:      ABORT;
