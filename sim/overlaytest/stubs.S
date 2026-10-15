/* Stand-ins for TE functions, each starting with the real entry bytes.
 * _render (0x019b2674, LINK 0xc): counts calls, records its R0 (buf), scribbles 0x1111 over
 *   the pixel where the overlay's red swatch goes (so an overlay drawn BEFORE render would be
 *   overwritten), returns R0 = 0x5a5a.
 * _onkey (0x019b6c74, LINK 0): counts calls, records R0/R1. Defsyms: RCOUNT, RBUF, SWATCH,
 *   KCOUNT, KARGS. Linked as two separate images (see run_overlaytest.py). */
        .section .text
        .global _render
_render:
        LINK 0xc;
        P1.L = RCOUNT;  P1.H = RCOUNT;
        R1 = [P1];      R1 += 1;        [P1] = R1;
        P1.L = RBUF;    P1.H = RBUF;    [P1] = R0;
        P1.L = SWATCH;  P1.H = SWATCH;
        R1 = 0x1111 (Z);
        W[P1] = R1;
        R0 = 0x5a5a (Z);
        UNLINK;
        RTS;
