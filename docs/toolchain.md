# Building the bfin-elf toolchain

The OP-1's CPU is an Analog Devices Blackfin (ADSP-BF524C2), so the SDK uses a `bfin-elf` GNU
toolchain: binutils 2.43.1, GCC 14.2.0 and GDB 16.3 (whose Blackfin simulator runs the SDK's
test benches). It is not in git — build it once into `toolchain/`. Download the three official
source tarballs into `toolchain/src/` and unpack them there first. The commands below run from
`toolchain/build*` directories; macOS needs system zlib and no makeinfo.

## binutils (assembler, linker, objdump)

```sh
../src/binutils-2.43.1/configure --target=bfin-elf --prefix=$PWD/../bfin --disable-nls --disable-werror \
  --disable-gdb --disable-gdbserver --disable-gprofng --disable-sim --with-system-zlib --without-zstd
make -j8 MAKEINFO=true all-binutils all-gas all-ld && make MAKEINFO=true install-binutils install-gas install-ld
```

## GCC (C compiler)

Freestanding; no libc — libgcc is enough for the SDK so far:

```sh
../src/gcc-14.2.0/configure --target=bfin-elf --prefix=$PWD/../bfin --enable-languages=c \
  --disable-nls --disable-werror --disable-multilib --disable-shared --disable-threads --disable-lto \
  --disable-plugin --disable-libssp --disable-libquadmath --disable-libgomp --disable-libatomic \
  --without-headers --with-newlib --with-gmp=/opt/homebrew/opt/gmp --with-mpfr=/opt/homebrew/opt/mpfr \
  --with-mpc=/opt/homebrew/opt/libmpc --with-system-zlib --without-zstd
make -j8 MAKEINFO=true all-gcc && make MAKEINFO=true install-gcc
make -j8 MAKEINFO=true all-target-libgcc && make MAKEINFO=true install-target-libgcc
```

GCC's bfin ABI was checked against TE's VisualDSP++ code and matches: args in R0–R2 then the stack
(4th arg at `[SP+12]`), 12-byte outgoing arg area reserved by the caller, result in R0, R4–R7/P3–P5
callee-saved. Assembly that calls C must reserve the 12 bytes itself (`SP += -12` before `CALL`).

## GDB + simulator

**The simulator must be built with -O0**: at -O2 clang miscompiles it and every `JUMP.L`/`CALL`
with a 24-bit offset loops forever.

```sh
../src/gdb-16.3/configure --target=bfin-elf --prefix=$PWD/../bfin --disable-nls --disable-werror \
  --with-system-zlib --without-zstd --with-gmp=/opt/homebrew/opt/gmp --with-mpfr=/opt/homebrew/opt/mpfr \
  --disable-binutils --disable-ld --disable-gas --disable-gprof --disable-gprofng --without-python \
  --without-guile --disable-tui --with-expat=no
make -j8 MAKEINFO=true CFLAGS="-g -O0" all-sim && make MAKEINFO=true install-sim
make -j8 MAKEINFO=true all-gdb && make MAKEINFO=true install-gdb
```
