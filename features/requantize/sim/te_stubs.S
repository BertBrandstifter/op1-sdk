/* Stand-ins at TE addresses for the requantize bench (assembled per address, -defsym WHICH=n);
 * each forwards to a C function in driver.c (same ABI; P1 is scratch). */
        .section .text
        .global _stub
_stub:
.if WHICH == 0                  /* 0x01a85750 voice note-on */
        P1.L = LOG_ON;  P1.H = LOG_ON;
        JUMP (P1);
.endif
.if WHICH == 1                  /* 0x01a85840 voice note-off */
        P1.L = LOG_OFF;  P1.H = LOG_OFF;
        JUMP (P1);
.endif
.if WHICH == 2                  /* 0x01a0d2c0 TE seq tick, 0x019b2674 frame render */
        RTS;
.endif
.if WHICH == 3                  /* 0x019b6c74 on_key / 0x019b6c0c on_knob: entry bytes, then log */
        LINK 0x0;
        UNLINK;
        P1.L = LOG_INPUT;  P1.H = LOG_INPUT;
        JUMP (P1);
.endif
.if WHICH == 4                  /* 0x019c2f24 key queue push */
        P1.L = EMU_PUSH;  P1.H = EMU_PUSH;
        JUMP (P1);
.endif
.if WHICH == 5                  /* 0x01a55024 tape seek */
        P1.L = EMU_SEEK;  P1.H = EMU_SEEK;
        JUMP (P1);
.endif
.if WHICH == 7                  /* 0x019c2f04 wake TE's key thread */
        P1.L = EMU_WAKE;  P1.H = EMU_WAKE;
        JUMP (P1);
.endif
.if WHICH == 6                  /* 0x01967ed0 track gains update */
        P1.L = EMU_GAINS;  P1.H = EMU_GAINS;
        JUMP (P1);
.endif
