/* fb_test: two hooks, one image.
 *  - _hook_seqids: call-site hook (SeqManager -> SeqIds, fires on sequencer
 *    switches): draws the corner markers once, then tail-jumps to the original.
 *  - _hook_midi: entry hook on the MIDI dispatcher: draws note/CC bars,
 *    then replays the overwritten LINK 0x10 and re-enters at +4. */

        .section .text.entry
        .global _hook_seqids
_hook_seqids:
        [--SP] = RETS;
        [--SP] = P1;
        [--SP] = P0;
        [--SP] = R2;
        [--SP] = R1;
        [--SP] = R0;
        SP += -12;
        CALL _seqids_touch;
        SP += 12;
        R0 = [SP++];
        R1 = [SP++];
        R2 = [SP++];
        P0 = [SP++];
        P1 = [SP++];
        RETS = [SP++];
        JUMP.L ORIG_SEQIDS;

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
