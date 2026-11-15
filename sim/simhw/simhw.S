/* simhw bench program: 16-bit immediates + a device read in the operating environment.
 * Writes 5, 6, 7, (0x1f00000 + 8) and 0xd into the L1 scratchpad, then stops at _end. */
        .section .text
        .global _start
_start:
        P5.H = 0xffb0
        P5.L = 0x0000
        R0 = 0x5
        [P5] = R0
        R1 = 0x6
        [P5 + 4] = R1
        R2.L = 0x7
        [P5 + 8] = R2
        P0.H = 0x01f0
        P0.L = 0x0000
        P0 += 8
        [P5 + 12] = P0
        P1 = 0xd
        [P5 + 16] = P1
_end:
        NOP
