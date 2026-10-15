/* Stand-ins at TE addresses for the engine-layer bench (assembled per address, -defsym WHICH=n).
 * Each forwards to a C function in driver.c (same ABI; P1 is scratch). */
        .section .text
        .global _stub
_stub:
.if WHICH == 4                  /* 0x019a23d4 fm voice render: records the shared table it sees */
        P1.L = FM_RENDER;  P1.H = FM_RENDER;
        JUMP (P1);
.endif
.if WHICH == 0                  /* 0x01a4f098 string voice render(self, out, knobs) */
        P1.L = FAKE_RENDER;  P1.H = FAKE_RENDER;
        JUMP (P1);
.endif
.if WHICH == 1                  /* 0x01a00d14 patch set knobs(patch, knobs) */
        P1.L = LOG_SAVE;  P1.H = LOG_SAVE;
        JUMP (P1);
.endif
.if WHICH == 2                  /* 0x019b6c74 on_key: real entry bytes, the hook replays them */
        LINK 0x0;
        UNLINK;
        P1.L = LOG_KEY;  P1.H = LOG_KEY;
        JUMP (P1);
.endif
.if WHICH == 3                  /* 0x019b6c0c on_knob */
        LINK 0x0;
        UNLINK;
        P1.L = LOG_KNOB;  P1.H = LOG_KNOB;
        JUMP (P1);
.endif
