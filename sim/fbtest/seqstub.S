/* At ORIG_SEQIDS (0x01a42344) in the fb_test image: the seqids hook's tail
 * target. The call-site mechanism is proven by sim/ctest; here it only has to
 * hand control back to the driver. */
        .section .text
        .global _seqstub
_seqstub:
        RTS;
