/* Overlay test driver (features/key_probe): key event 38/down through the generated entry hook,
 * then one frame through the generated "after" call-site hook. HLT = pass, ABORT = fail, bits in R7:
 *   0 SP/FP after key hook   1 on_key stub ran once with (38, 1)   2 probe saw key 38
 *   3 SP/FP after frame      4 frame hook returned render's 0x5a5a  5 render ran once with BUF
 *   6 red swatch = 0xf800 (overlay drawn after render)   7 'K' top-left pixel white
 *   8 black box pixel = 0   9 R4-R6 preserved
 * Defsyms: STACK, KHOOK, FHOOK, BUF, KCOUNT, KARGS, LASTKEY, RCOUNT, RBUF, SWATCH, KPIX, BOXPIX */
        .section .text
        .global _start

        .macro spcheck bit
        R1.L = STACK;   R1.H = STACK;
        R2 = SP;        CC = R2 == R1;  IF !CC JUMP 9f;
        R2 = FP;        CC = R2 == R1;  IF CC JUMP 8f;
9:      BITSET (R7, \bit);
8:
        .endm

        .macro expect addr, val, bit
        P1.L = \addr;   P1.H = \addr;   R2 = [P1];
        R1.L = \val;    R1.H = \val;      /* \val must be a symbol: .H of a symbol = its high half */
        CC = R2 == R1;  IF CC JUMP 7f;
        BITSET (R7, \bit);
7:
        .endm

        .macro expectw addr, val, bit
        P1.L = \addr;   P1.H = \addr;   R2 = W[P1] (Z);
        R1.L = \val;    R1.H = 0;
        CC = R2 == R1;  IF CC JUMP 6f;
        BITSET (R7, \bit);
6:
        .endm

        .set V_ONE, 1
        .set V_38, 38

_start:
        SP.L = STACK;   SP.H = STACK;
        FP = SP;
        R7 = 0;
        R4 = 0x44;      R5 = 0x55;      R6 = 0x66;

        R0 = 38;        R1 = 1;
        P2.L = KHOOK;   P2.H = KHOOK;
        CALL (P2);
        spcheck 0
        expect KCOUNT, V_ONE, 1
        P1.L = KARGS;   P1.H = KARGS;
        R2 = [P1];      R1 = 38;        CC = R2 == R1;  IF !CC JUMP 1f;
        R2 = [P1 + 4];  R1 = 1;         CC = R2 == R1;  IF CC JUMP 2f;
1:      BITSET (R7, 1);
2:      expect LASTKEY, V_38, 2

        R0.L = BUF;     R0.H = BUF;
        P2.L = FHOOK;   P2.H = FHOOK;
        CALL (P2);
        R3 = R0;
        spcheck 3
        R1 = 0x5a5a (Z);
        CC = R3 == R1;  IF CC JUMP 3f;
        BITSET (R7, 4);
3:      expect RCOUNT, V_ONE, 5
        expect RBUF, BUF, 5
        expectw SWATCH, 0xf800, 6
        expectw KPIX, 0xffff, 7
        expectw BOXPIX, 0, 8
        R1 = 0x44;      CC = R4 == R1;  IF !CC JUMP 4f;
        R1 = 0x55;      CC = R5 == R1;  IF !CC JUMP 4f;
        R1 = 0x66;      CC = R6 == R1;  IF CC JUMP 5f;
4:      BITSET (R7, 9);
5:      CC = R7 == 0;
        IF !CC JUMP 6f;
        HLT;
6:      ABORT;
