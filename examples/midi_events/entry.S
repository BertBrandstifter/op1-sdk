/* Entry hook on the TE MIDI dispatcher (0x01a85c20, VoiceCtl.cpp): every MIDI
 * message (note on/off, CC, pitch bend) arrives here as R0 = const te_bytes *.
 * build.py overwrites the dispatcher's first instruction (LINK 0x10) with
 * JUMP.L _hook_midi. We save the ABI scratch registers, call midi_event() in C,
 * restore, then re-run the overwritten LINK and jump back into the dispatcher
 * at ORIG_DISPATCH_NEXT (0x01a85c24) - the classic inline hook replay. */

        .section .text.entry
        .global _hook_midi
_hook_midi:
        [--SP] = RETS;
        [--SP] = P1;
        [--SP] = P0;
        [--SP] = R2;
        [--SP] = R1;
        [--SP] = R0;
        SP += -12;
        CALL _midi_event;
        SP += 12;
        R0 = [SP++];
        R1 = [SP++];
        R2 = [SP++];
        P0 = [SP++];
        P1 = [SP++];
        RETS = [SP++];
        JUMP.L _midi_replay;

_midi_replay:
        LINK 0x10;                  /* the overwritten dispatcher entry bytes */
        JUMP.L ORIG_DISPATCH_NEXT;  /* back into TE's dispatcher at +4 */
