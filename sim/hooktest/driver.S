/* Hook test driver: put marker values in registers, call the code at the patched call site's target
 * (our hook), and stop. The test checks the hook increments its counter, preserves R0/P0 and reaches
 * the original TE function with RETS pointing back here. */
        .section .text
        .global _start
_start:
        SP.L = 0xfffc;   SP.H = 0x01f7;           /* stack in free SDRAM */
        FP = SP;
        R0.L = 0x1234;   R0.H = 0xcafe;
        P0.L = 0x5678;   P0.H = 0xbeef;
        P1.L = 0x0000;   P1.H = 0x01e0;           /* _hook_seqids (hello block at 0x01e00000) */
        CALL (P1);
_back:
        EMUEXCPT;                                  /* never reached in this test: the original runs on */
        .global _back
