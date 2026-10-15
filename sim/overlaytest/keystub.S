        .section .text
        .global _onkey
_onkey:
        LINK 0x0;
        P1.L = KCOUNT;  P1.H = KCOUNT;
        R2 = [P1];      R2 += 1;        [P1] = R2;
        P1.L = KARGS;   P1.H = KARGS;
        [P1] = R0;
        [P1 + 4] = R1;
        UNLINK;
        RTS;
