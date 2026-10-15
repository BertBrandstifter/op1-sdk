/* Placed at the TE MIDI dispatcher address (0x01a85c20) in the test image.
 * First instruction = the original entry bytes (LINK 0x10); the hook's replay
 * stub jumps to _dispatch+4, exactly like the real dispatcher flow.
 * Calls 1-2 return cleanly to the driver; call 3 verifies state + counters:
 * HLT (exit 0) if all pass, else ABORT (exit 1) with failure bits in R7:
 *   0 R0=V3   1 RETS=BACK3   2 SP=STACK-0x18   3 FP=STACK-8   4 total==3
 *   5 note_on==1  6 note_off==0  7 cc==1  8 bend==1  9 other==0
 *   10 note==60  11 vel==100  12 cc_num==12  13 cc_val==45  14 bend==0x2021
 * Defsyms (32-bit): STACK, V3, BACK3, SCOUNT, TOTAL, NON, NOFF, CCC, BEND,
 * OTHER, NOTE, VEL, CCN, CCV, BENDV. */
        .section .text
        .global _dispatch
_dispatch:
        LINK 0x10;                              /* original entry bytes 00 e8 04 00 */
        P1.L = SCOUNT;  P1.H = SCOUNT;
        R1 = [P1];      R1 += 1;        [P1] = R1;
        CC = R1 == 3;
        IF CC JUMP _final;
        SP += 0x10;                             /* undo the replayed LINK, return */
        UNLINK;
        RTS;
_final:
        R7 = 0;
        R1.L = V3;      R1.H = V3;      CC = R0 == R1;  IF CC JUMP 1f;  BITSET (R7, 0);
1:      R2 = RETS;
        R1.L = BACK3;   R1.H = BACK3;   CC = R2 == R1;  IF CC JUMP 2f;  BITSET (R7, 1);
2:      R1.L = STACK;   R1.H = STACK;   R1 += -0x18;
        R2 = SP;                        CC = R2 == R1;  IF CC JUMP 3f;  BITSET (R7, 2);
3:      R1.L = STACK;   R1.H = STACK;   R1 += -0x8;
        R2 = FP;                        CC = R2 == R1;  IF CC JUMP 4f;  BITSET (R7, 3);
4:      P1.L = TOTAL;   P1.H = TOTAL;   R2 = [P1];
        R1 = 3;                         CC = R2 == R1;  IF CC JUMP 5f;  BITSET (R7, 4);
5:      P1.L = NON;     P1.H = NON;     R2 = [P1];
        R1 = 1;                         CC = R2 == R1;  IF CC JUMP 6f;  BITSET (R7, 5);
6:      P1.L = NOFF;    P1.H = NOFF;    R2 = [P1];
        R1 = 0;                         CC = R2 == R1;  IF CC JUMP 7f;  BITSET (R7, 6);
7:      P1.L = CCC;     P1.H = CCC;     R2 = [P1];
        R1 = 1;                         CC = R2 == R1;  IF CC JUMP 8f;  BITSET (R7, 7);
8:      P1.L = BEND;    P1.H = BEND;    R2 = [P1];
        R1 = 1;                         CC = R2 == R1;  IF CC JUMP 9f;  BITSET (R7, 8);
9:      P1.L = OTHER;   P1.H = OTHER;   R2 = [P1];
        R1 = 0;                         CC = R2 == R1;  IF CC JUMP 1f;  BITSET (R7, 9);
1:      P1.L = NOTE;    P1.H = NOTE;    R2 = [P1];
        R1 = 60;                        CC = R2 == R1;  IF CC JUMP 2f;  BITSET (R7, 10);
2:      P1.L = VEL;     P1.H = VEL;     R2 = [P1];
        R1 = 100;                       CC = R2 == R1;  IF CC JUMP 3f;  BITSET (R7, 11);
3:      P1.L = CCN;     P1.H = CCN;     R2 = [P1];
        R1 = 12;                        CC = R2 == R1;  IF CC JUMP 4f;  BITSET (R7, 12);
4:      P1.L = CCV;     P1.H = CCV;     R2 = [P1];
        R1 = 45;                        CC = R2 == R1;  IF CC JUMP 5f;  BITSET (R7, 13);
5:      P1.L = BENDV;   P1.H = BENDV;   R2 = [P1];
        R1 = 0x2021;                    CC = R2 == R1;  IF CC JUMP 6f;  BITSET (R7, 14);
6:      CC = R7 == 0;
        IF !CC JUMP 7f;
        HLT;                                    /* simulator exit 0 = pass */
7:      ABORT;                                  /* simulator exit 1 = fail; R7 = failure bits */
