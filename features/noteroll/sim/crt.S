/* bench startup: stack, call main, HLT (exit 0) if it returned 0, else ABORT with R7 = result */
        .section .text.start
        .global _start
_start:
        SP.L = STACK;   SP.H = STACK;
        FP = SP;
        SP += -12;
        CALL _main;
        R7 = R0;
        CC = R0 == 0;
        IF !CC JUMP 1f;
        HLT;
1:      ABORT;
