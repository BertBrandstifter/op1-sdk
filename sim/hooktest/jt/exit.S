        .section .text
        .global _x
_x:     R7 = 42;  SP.L = 0xfffc; SP.H = 0x01f7; SP += -4; [SP] = R7; R0 = SP; P0 = 1; EXCPT 0;
