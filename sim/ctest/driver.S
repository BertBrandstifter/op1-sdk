/* C-hook test driver: build te_string "pattern" (SSO form) in free SDRAM, put marker
 * values in the registers, call the hook. The check stub at the TE target address
 * verifies what arrives. Linked at 0x01f00000. Defsyms: STACK, STRING, HOOK (32-bit). */
        .section .text
        .global _start
_start:
        SP.L = STACK;    SP.H = STACK;
        FP = SP;
        P0.L = STRING;   P0.H = STRING;
        R1 = 0;          [P0] = R1;              /* reserved */
        R1 = 7;          [P0 + 0x14] = R1;       /* size */
                         [P0 + 0x18] = R1;       /* cap < 16 -> SSO */
        R1 = 0x70; B[P0 + 0x4]  = R1;            /* p */
        R1 = 0x61; B[P0 + 0x5]  = R1;            /* a */
        R1 = 0x74; B[P0 + 0x6]  = R1;            /* t */
        R1 = 0x74; B[P0 + 0x7]  = R1;            /* t */
        R1 = 0x65; B[P0 + 0x8]  = R1;            /* e */
        R1 = 0x72; B[P0 + 0x9]  = R1;            /* r */
        R1 = 0x6e; B[P0 + 0xa]  = R1;            /* n */
        R1 = 0;    B[P0 + 0xb]  = R1;            /* NUL */
        R0 = P0;                                 /* the hook's argument */
        P0.L = 0x5678;   P0.H = 0xbeef;          /* marker: hook must preserve P0 */
        P1.L = HOOK;     P1.H = HOOK;
        CALL (P1);
_back:
        EMUEXCPT;                                /* never reached: the original runs on */
        .global _back
