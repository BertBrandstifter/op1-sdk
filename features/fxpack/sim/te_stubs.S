/* Stand-ins at TE addresses for the FXPACK bench (assembled per address, -defsym WHICH=n).
 * Each forwards to a C function in driver.c (same ABI; P1 is scratch). */
        .section .text
        .global _stub
_stub:
.if WHICH == 0                  /* 0x01a1bb74 phone mono process(self, buf, params) */
        P1.L = PHONE_MONO;  P1.H = PHONE_MONO;
        JUMP (P1);
.endif
.if WHICH == 1                  /* 0x01a1c25c phone stereo process(self, l, r, params) */
        P1.L = PHONE_STEREO;  P1.H = PHONE_STEREO;
        JUMP (P1);
.endif
.if WHICH == 2                  /* 0x019ff664 patch set fx params(patch, p8) */
        P1.L = LOG_SAVE;  P1.H = LOG_SAVE;
        JUMP (P1);
.endif
.if WHICH == 3                  /* 0x019b6c74 on_key: real entry bytes, the hook replays them */
        LINK 0x0;
        UNLINK;
        P1.L = LOG_KEY;  P1.H = LOG_KEY;
        JUMP (P1);
.endif
.if WHICH == 5                  /* 0x0193ef3c master changed (state) */
        P1.L = LOG_MASTER;  P1.H = LOG_MASTER;
        JUMP (P1);
.endif
.if WHICH == 4                  /* 0x019b6c0c on_knob */
        LINK 0x0;
        UNLINK;
        P1.L = LOG_KNOB;  P1.H = LOG_KNOB;
        JUMP (P1);
.endif
