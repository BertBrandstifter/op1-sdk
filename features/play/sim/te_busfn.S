/* stand-in for TE's metronome mix 0x019de548: its first 6 bytes as in stock (the entry hook
 * replays them and jumps to +6), then undo the push and return */
        .section .text
        .global _stub
_stub:
        [--SP] = (R7:4);
        P1.L = 0xdabc;
        (R7:4) = [SP++];
        RTS;
