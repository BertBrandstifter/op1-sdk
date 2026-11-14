/* Stand-ins at TE addresses for the slotlink bench (assembled per address, -defsym WHICH=n).
 * Each forwards to a C function in driver.c (same ABI; P1 is scratch). */
        .section .text
        .global _stub
_stub:
.if WHICH == 0                  /* 0x01a997f4 open(path, oflag, mode) */
        P1.L = FS_OPEN;  P1.H = FS_OPEN;
.endif
.if WHICH == 1                  /* 0x01a99c90 read(fd, buf, n) */
        P1.L = FS_READ;  P1.H = FS_READ;
.endif
.if WHICH == 2                  /* 0x01a99908 close(fd) */
        P1.L = FS_CLOSE;  P1.H = FS_CLOSE;
.endif
.if WHICH == 3                  /* 0x01a99f84 write(fd, buf, n) */
        P1.L = FS_WRITE;  P1.H = FS_WRITE;
.endif
.if WHICH == 4                  /* 0x0196eb7e TE's directory copy content -> user (dir, root) */
        P1.L = DIR_COPY;  P1.H = DIR_COPY;
.endif
        JUMP (P1);
