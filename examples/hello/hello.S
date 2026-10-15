/* op1-sdk example: count how often the sequencer-id lookup (SeqIds.cpp, 0x01a42344) is called
 * from SeqManager, then continue into the original function unchanged.
 * Linked at 0x01e00000 (free SDRAM after the stock image). ORIG_SEQIDS is given by the linker. */

        .section .text
        .global _hook_seqids
_hook_seqids:
        [--SP] = P0;
        [--SP] = R0;
        P0.L = _seqids_calls;
        P0.H = _seqids_calls;
        R0 = [P0];
        R0 += 1;
        [P0] = R0;
        R0 = [SP++];
        P0 = [SP++];
        JUMP.L ORIG_SEQIDS              /* tail call: RETS still points back into SeqManager */

        .section .data
        .align 4
        .global _seqids_calls
_seqids_calls:
        .long 0
