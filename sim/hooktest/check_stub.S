/* Placed at ORIG_SEQIDS (0x01a42344) in the test image instead of the TE function.
 * Verifies the state the hook hands over: HLT (exit 0) if all checks pass, else ABORT (exit 1)
 * with the failure bits in R7. */
        .section .text
        .global _check
_check:
        R7 = 0;
        R1.L = 0x1234;  R1.H = 0xcafe;  CC = R0 == R1;  IF CC JUMP 1f;  BITSET (R7, 0);
1:      R2 = P0;
        R1.L = 0x5678;  R1.H = 0xbeef;  CC = R2 == R1;  IF CC JUMP 2f;  BITSET (R7, 1);
2:      R2 = RETS;
        R1.L = 0x0024;  R1.H = 0x01f0;  CC = R2 == R1;  IF CC JUMP 3f;  BITSET (R7, 2);
3:      R2 = SP;
        R1.L = 0xfffc;  R1.H = 0x01f7;  CC = R2 == R1;  IF CC JUMP 4f;  BITSET (R7, 3);
4:      P1.L = 0x001c;  P1.H = 0x01e0;  R2 = [P1];      /* hook counter */
        R1 = 1;                         CC = R2 == R1;  IF CC JUMP 5f;  BITSET (R7, 4);
5:      CC = R7 == 0;
        IF !CC JUMP 6f;
        HLT;                            /* simulator exit 0 = pass */
6:      ABORT;                          /* simulator exit 1 = fail; R7 holds the failure bits */
