/* Placed at ORIG_SEQIDS (0x01a42344) in the test image instead of the TE function.
 * Verifies the state the C hook hands over: HLT (exit 0) if all checks pass, else
 * ABORT (exit 1) with failure bits in R7.
 * Defsyms (32-bit): STRING, BACK, STACK, CNT (_lookup_count), NAME (_last_name). */
        .section .text
        .global _check
_check:
        R7 = 0;
        R1.L = STRING;  R1.H = STRING;  CC = R0 == R1;  IF CC JUMP 1f;  BITSET (R7, 0);
1:      R2 = P0;
        R1.L = 0x5678;  R1.H = 0xbeef;  CC = R2 == R1;  IF CC JUMP 2f;  BITSET (R7, 1);
2:      R2 = RETS;
        R1.L = BACK;    R1.H = BACK;    CC = R2 == R1;  IF CC JUMP 3f;  BITSET (R7, 2);
3:      R2 = SP;
        R1.L = STACK;   R1.H = STACK;   CC = R2 == R1;  IF CC JUMP 4f;  BITSET (R7, 3);
4:      P1.L = CNT;     P1.H = CNT;     R2 = [P1];
        R1 = 1;                         CC = R2 == R1;  IF CC JUMP 5f;  BITSET (R7, 4);
5:      P1.L = NAME;    P1.H = NAME;                    /* last_name == "pattern" */
        P2.L = _expect; P2.H = _expect;
6:      R1 = B[P1++] (Z);
        R2 = B[P2++] (Z);
        CC = R1 == R2;
        IF !CC JUMP 7f;
        CC = R1 == 0;
        IF !CC JUMP 6b;
        JUMP 8f;
7:      BITSET (R7, 5);
8:      CC = R7 == 0;
        IF !CC JUMP 9f;
        HLT;                            /* simulator exit 0 = pass */
9:      ABORT;                          /* simulator exit 1 = fail; R7 holds the failure bits */

        .section .rodata
_expect:
        .ascii "pattern"
        .byte 0
