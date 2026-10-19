/* Stand-ins at TE addresses for the M1 loader bench (assembled per address, -defsym WHICH=n).
 * Each forwards to a C function in load_driver.c (same ABI; P1 is scratch). */
        .section .text
        .global _stub
_stub:
.if WHICH == 0                  /* 0x01a997f4 yaffs open(path, oflag, mode) */
        P1.L = FILE_OPEN;  P1.H = FILE_OPEN;
        JUMP (P1);
.endif
.if WHICH == 1                  /* 0x01a99c90 yaffs read(fd, buf, n) */
        P1.L = FILE_READ;  P1.H = FILE_READ;
        JUMP (P1);
.endif
.if WHICH == 2                  /* 0x01a99908 yaffs close(fd) */
        P1.L = FILE_CLOSE;  P1.H = FILE_CLOSE;
        JUMP (P1);
.endif
.if WHICH == 3                  /* 0x01aa0318 heap_malloc(heap, size) */
        P1.L = HEAP_MALLOC;  P1.H = HEAP_MALLOC;
        JUMP (P1);
.endif
.if WHICH == 4                  /* 0x01aa185c data cache flush(start, end, invalidate) */
        RTS;
.endif
