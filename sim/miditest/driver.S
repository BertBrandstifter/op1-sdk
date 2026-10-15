/* MIDI hook test driver: build three te_bytes messages in free SDRAM
 * (note-on 0x90/60/100, CC 0xb0/12/45, pitch bend 0xe0/0x21/0x40) and call
 * the hook once per message. The check stub at the dispatcher address returns
 * cleanly for calls 1-2 and verifies everything on call 3.
 * Linked at 0x01f00000. Defsyms (32-bit): STACK, HOOK, V1-V3, MB1-MB3. */
        .section .text
        .global _start
_start:
        SP.L = STACK;   SP.H = STACK;
        FP = SP;

        P1.L = V1;      P1.H = V1;                  /* message 1: note on */
        P0.L = MB1;      P0.H = MB1;
        R1 = 0x90;      B[P0] = R1;
        R1 = 60;        B[P0 + 0x1] = R1;
        R1 = 100;       B[P0 + 0x2] = R1;
        R2 = P0;        R2 += 0x3;
        R1 = 0;         [P1] = R1;
        [P1 + 0x4] = P0;
        [P1 + 0x8] = R2;
        [P1 + 0xc] = R2;
        R0 = P1;
        P2.L = HOOK;    P2.H = HOOK;
        CALL (P2);
_back1:

        P1.L = V2;      P1.H = V2;                  /* message 2: CC */
        P0.L = MB2;      P0.H = MB2;
        R1 = 0xb0;      B[P0] = R1;
        R1 = 12;        B[P0 + 0x1] = R1;
        R1 = 45;        B[P0 + 0x2] = R1;
        R2 = P0;        R2 += 0x3;
        R1 = 0;         [P1] = R1;
        [P1 + 0x4] = P0;
        [P1 + 0x8] = R2;
        [P1 + 0xc] = R2;
        R0 = P1;
        P2.L = HOOK;    P2.H = HOOK;
        CALL (P2);
_back2:

        P1.L = V3;      P1.H = V3;                  /* message 3: pitch bend */
        P0.L = MB3;      P0.H = MB3;
        R1 = 0xe0;      B[P0] = R1;
        R1 = 0x21;      B[P0 + 0x1] = R1;
        R1 = 0x40;      B[P0 + 0x2] = R1;
        R2 = P0;        R2 += 0x3;
        R1 = 0;         [P1] = R1;
        [P1 + 0x4] = P0;
        [P1 + 0x8] = R2;
        [P1 + 0xc] = R2;
        R0 = P1;
        P2.L = HOOK;    P2.H = HOOK;
        CALL (P2);
_back3:
        EMUEXCPT;                                   /* never reached: the stub exits on call 3 */
        .global _back1
        .global _back2
        .global _back3
