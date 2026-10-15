.text
.global _s
_s: NOP; JUMP.L _t; NOP; NOP;
.rept 32
NOP;
.endr
_t: R0 = 7; HLT;
