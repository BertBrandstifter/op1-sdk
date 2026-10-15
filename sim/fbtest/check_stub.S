/* Stub at the MIDI dispatcher address (0x01a85c20) for the fb_test image:
 * original LINK 0x10 entry; calls 1-2 return cleanly; call 3 checks the
 * framebuffer + state and exits. (The seqids tail target lives in seqstub.S.)
 * Failure bits in R7:
 *   0 TL red   1 TR green   2 BR blue   3 BL white   4 bar top white
 *   5 bar end white (y=239)   6 below bar black (y=240)   7 CC block green
 *   8 midi_total==3   9 RETS==BACK3   10 SP==STACK-0x18   11 FP==STACK-8
 * Defsyms: STACK, BACK3, SCOUNT, TOTAL, MTL, MTR, MBR, MBL, BART, BARE, BARA, CCB. */
        .section .text
        .global _dispatch
_dispatch:
        LINK 0x10;                              /* original entry bytes 00 e8 04 00 */
        P1.L = SCOUNT;  P1.H = SCOUNT;
        R1 = [P1];      R1 += 1;        [P1] = R1;
        CC = R1 == 3;
        IF CC JUMP _final;
        SP += 0x10;
        UNLINK;
        RTS;
_final:
        R7 = 0;
        P1.L = MTL;     P1.H = MTL;     R2 = W[P1];
        R1.L = 0xf800;   R1.H = 0;                    CC = R2 == R1;  IF CC JUMP 1f;  BITSET (R7, 0);
1:      P1.L = MTR;     P1.H = MTR;     R2 = W[P1];
        R1 = 0x07e0;                    CC = R2 == R1;  IF CC JUMP 2f;  BITSET (R7, 1);
2:      P1.L = MBR;     P1.H = MBR;     R2 = W[P1];
        R1 = 0x001f;                    CC = R2 == R1;  IF CC JUMP 3f;  BITSET (R7, 2);
3:      P1.L = MBL;     P1.H = MBL;     R2 = W[P1];
        R1.L = 0xffff;    R1.H = 0;                    CC = R2 == R1;  IF CC JUMP 4f;  BITSET (R7, 3);
4:      P1.L = BART;    P1.H = BART;    R2 = W[P1];
        R1.L = 0xffff;    R1.H = 0;                    CC = R2 == R1;  IF CC JUMP 5f;  BITSET (R7, 4);
5:      P1.L = BARE;    P1.H = BARE;    R2 = W[P1];
        R1.L = 0xffff;    R1.H = 0;                    CC = R2 == R1;  IF CC JUMP 6f;  BITSET (R7, 5);
6:      P1.L = BARA;    P1.H = BARA;    R2 = W[P1];
        R1 = 0;                         CC = R2 == R1;  IF CC JUMP 7f;  BITSET (R7, 6);
7:      P1.L = CCB;     P1.H = CCB;     R2 = W[P1];
        R1 = 0x07e0;                    CC = R2 == R1;  IF CC JUMP 8f;  BITSET (R7, 7);
8:      P1.L = TOTAL;   P1.H = TOTAL;   R2 = [P1];
        R1 = 3;                         CC = R2 == R1;  IF CC JUMP 9f;  BITSET (R7, 8);
9:      R2 = RETS;
        R1.L = BACK3;   R1.H = BACK3;   CC = R2 == R1;  IF CC JUMP 1f;  BITSET (R7, 9);
1:      R1.L = STACK;   R1.H = STACK;   R1 += -0x18;
        R2 = SP;                        CC = R2 == R1;  IF CC JUMP 2f;  BITSET (R7, 10);
2:      R1.L = STACK;   R1.H = STACK;   R1 += -0x8;
        R2 = FP;                        CC = R2 == R1;  IF CC JUMP 3f;  BITSET (R7, 11);
3:      CC = R7 == 0;
        IF !CC JUMP 4f;
        HLT;                                    /* simulator exit 0 = pass */
4:      ABORT;                                  /* simulator exit 1 = fail; R7 = failure bits */
