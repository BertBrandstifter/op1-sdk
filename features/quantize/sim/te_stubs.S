/* Stand-ins at TE addresses, each forwarding to the C logger in driver.c (same ABI; P1 is scratch).
 * Assembled per address by run_quanttest.py with -defsym WHICH=n. */
        .section .text
        .global _stub
_stub:
.if WHICH == 0                  /* 0x01a85750 voice note-on(note*, extra) */
        P1.L = LOG_ON;  P1.H = LOG_ON;
        JUMP (P1);
.endif
.if WHICH == 1                  /* 0x01a85840 voice note-off(note*) */
        P1.L = LOG_OFF;  P1.H = LOG_OFF;
        JUMP (P1);
.endif
.if WHICH == 2                  /* 0x01a0d2c0 TE seq tick */
        P1.L = LOG_TICK;  P1.H = LOG_TICK;
        JUMP (P1);
.endif
.if WHICH == 4                  /* 0x019b2674 TE frame render(buf): nothing to draw */
        RTS;
.endif
.if WHICH == 5                  /* 0x019b6c0c on_knob: real entry bytes, the hook replays them */
        LINK 0x0;
        UNLINK;
        P1.L = LOG_KNOB;  P1.H = LOG_KNOB;
        JUMP (P1);
.endif
.if WHICH == 3                  /* 0x019b6c74 on_key: real entry bytes, the hook replays them */
        LINK 0x0;
        UNLINK;
        P1.L = LOG_KEY;  P1.H = LOG_KEY;
        JUMP (P1);
.endif
