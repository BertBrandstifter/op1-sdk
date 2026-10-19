/* Stand-ins at TE addresses for the NOTE ROLL bench (assembled per address, -defsym WHICH=n);
 * each forwards to a C function in driver.c (same ABI; P1 is scratch). */
        .section .text
        .global _stub
_stub:
.if WHICH == 0                  /* 0x01a85750 VoiceCtl note-on */
        P1.L = EMU_ON;  P1.H = EMU_ON;
        JUMP (P1);
.endif
.if WHICH == 1                  /* 0x01a85840 VoiceCtl note-off */
        P1.L = EMU_OFF;  P1.H = EMU_OFF;
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
.if WHICH == 4                  /* 0x019789ec drum sampler voice render */
        P1.L = EMU_RENDER;  P1.H = EMU_RENDER;
        JUMP (P1);
.endif
.if WHICH == 5                  /* 0x01978926 drum sampler voice note_on */
        P1.L = EMU_VOICE_ON;  P1.H = EMU_VOICE_ON;
        JUMP (P1);
.endif
.if WHICH == 6                  /* TE db / sqlite / VDK semaphore: the C stand-in TARGET (same ABI) */
        P1.L = TARGET;  P1.H = TARGET;
        JUMP (P1);
.endif
