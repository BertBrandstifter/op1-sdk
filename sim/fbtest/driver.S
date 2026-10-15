/* fb_test driver: first call the seqids hook (draws the corner markers), then
 * feed three MIDI messages through the dispatcher hook (note-on 60/100 draws a
 * bar, CC 12/45 draws a block). The check stub at the dispatcher address
 * verifies the framebuffer contents on the third call.
 * Linked at 0x01f00000. Defsyms: STACK, STRING, SHOOK, HOOK, V1-V3, MB1-MB3. */
        .section .text
        .global _start
_start:
        SP.L = STACK;   SP.H = STACK;
        FP = SP;

        P0.L = STRING;  P0.H = STRING;          /* te_string "pattern" (SSO) */
        R1 = 0;         [P0] = R1;
        R1 = 7;         [P0 + 0x14] = R1;
                        [P0 + 0x18] = R1;
        R1 = 0x70; B[P0 + 0x4]  = R1;
        R1 = 0x61; B[P0 + 0x5]  = R1;
        R1 = 0x74; B[P0 + 0x6]  = R1;
        R1 = 0x74; B[P0 + 0x7]  = R1;
        R1 = 0x65; B[P0 + 0x8]  = R1;
        R1 = 0x72; B[P0 + 0x9]  = R1;
        R1 = 0x6e; B[P0 + 0xa]  = R1;
        R1 = 0;    B[P0 + 0xb]  = R1;
        R0 = P0;
        P2.L = SHOOK;   P2.H = SHOOK;           /* seqids hook -> markers */
        CALL (P2);
_back0:

        P1.L = V1;      P1.H = V1;              /* message 1: note on 60/100 */
        P0.L = MB1;     P0.H = MB1;
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

        P1.L = V2;      P1.H = V2;              /* message 2: CC 12/45 */
        P0.L = MB2;     P0.H = MB2;
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

        P1.L = V3;      P1.H = V3;              /* message 3: pitch bend */
        P0.L = MB3;     P0.H = MB3;
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
        EMUEXCPT;                               /* never reached: the stub exits on call 3 */
        .global _back0
        .global _back1
        .global _back2
        .global _back3
