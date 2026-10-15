/* Placed at the TE MIDI dispatcher (0x01a85c20) in the test image. Starts with the
 * original entry bytes (LINK 0x10) so the generated replay (_te_orig_01a85c20:
 * LINK 0x10; JUMP.L 0x01a85c24) lands on the second instruction like on the device.
 * Counts calls in SCOUNT, remembers the message pointer in SLAST, returns cleanly. */
        .section .text
        .global _dispatch
_dispatch:
        LINK 0x10;
        P1.L = SCOUNT;  P1.H = SCOUNT;
        R1 = [P1];      R1 += 1;        [P1] = R1;
        P1.L = SLAST;   P1.H = SLAST;
        [P1] = R0;
        UNLINK;
        RTS;
