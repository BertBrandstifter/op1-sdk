#!/usr/bin/env bash
# One-time setup of the bfin-elf toolchain into toolchain/ (see docs/toolchain.md).
# Builds on GNU binutils, GCC and GDB (incl. its Blackfin simulator) — thanks to their maintainers.
# Idempotent: finished steps are skipped, so it can be re-run after a failure.
#   ./setup_toolchain.sh            build everything
#   ./setup_toolchain.sh --clean    wipe toolchain/ build dirs first (keeps downloads)
set -euo pipefail

SDK="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TC="$SDK/toolchain"
SRC="$TC/src"
PREFIX="$TC/bfin"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)"

BINUTILS=binutils-2.43.1
GCC=gcc-14.2.0
GDB=gdb-16.3
url_of() {
  case $1 in
    binutils-*) echo "https://ftp.gnu.org/gnu/binutils/$1.tar.xz" ;;
    gcc-*)      echo "https://ftp.gnu.org/gnu/gcc/$1/$1.tar.xz" ;;
    gdb-*)      echo "https://ftp.gnu.org/gnu/gdb/$1.tar.xz" ;;
  esac
}
sha_of() {
  case $1 in
    $BINUTILS) echo 13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd ;;
    $GCC)      echo a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9 ;;
    $GDB)      echo bcfcd095528a987917acf9fff3f1672181694926cc18d609c99d0042c00224c5 ;;
  esac
}

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }

[[ $EUID -ne 0 ]] || die "do not run as root/sudo (root-owned files break later builds)"

if [[ "${1:-}" == "--clean" ]]; then
  say "removing old build dirs and install"
  rm -rf "$TC/build" "$TC/build-gcc" "$TC/build-gdb" "$PREFIX"
  rm -rf "${SRC:?}/$BINUTILS" "${SRC:?}/$GCC" "${SRC:?}/$GDB"
fi

# --- prerequisites -----------------------------------------------------------
say "checking prerequisites"
[[ "$(uname)" == Darwin ]] || echo "warning: only tested on macOS; brew paths below assume Homebrew"
xcode-select -p >/dev/null 2>&1 || die "Xcode command line tools missing: run 'xcode-select --install'"
command -v brew >/dev/null || die "Homebrew missing: see https://brew.sh"
command -v python3 >/dev/null || die "python3 missing"
for pkg in gmp mpfr libmpc xz; do
  brew list --versions "$pkg" >/dev/null || { say "brew install $pkg"; brew install "$pkg"; }
done
GMP="$(brew --prefix gmp)"; MPFR="$(brew --prefix mpfr)"; MPC="$(brew --prefix libmpc)"

# --- sources -----------------------------------------------------------------
mkdir -p "$SRC"
for pkg in $BINUTILS $GCC $GDB; do
  tarball="$SRC/$pkg.tar.xz"
  if [[ ! -f "$tarball" ]]; then
    say "downloading $pkg"
    curl -fL --retry 3 -o "$tarball.part" "$(url_of "$pkg")"
    mv "$tarball.part" "$tarball"
  fi
  got="$(shasum -a 256 "$tarball" | cut -d' ' -f1)"
  [[ "$got" == "$(sha_of "$pkg")" ]] || die "$tarball: sha256 mismatch ($got); delete it and re-run"
  if [[ ! -d "$SRC/$pkg" ]]; then
    say "unpacking $pkg"
    tar -xJf "$tarball" -C "$SRC"
  fi
done

MK=(make MAKEINFO=true)

# --- binutils ----------------------------------------------------------------
if [[ ! -x "$PREFIX/bin/bfin-elf-ld" ]]; then
  say "building binutils"
  mkdir -p "$TC/build" && cd "$TC/build"
  "../src/$BINUTILS/configure" --target=bfin-elf --prefix="$PREFIX" --disable-nls --disable-werror \
    --disable-gdb --disable-gdbserver --disable-gprofng --disable-sim --with-system-zlib --without-zstd
  "${MK[@]}" -j"$JOBS" all-binutils all-gas all-ld
  "${MK[@]}" install-binutils install-gas install-ld
else
  say "binutils already installed, skipping"
fi
export PATH="$PREFIX/bin:$PATH"

# --- GCC ---------------------------------------------------------------------
if [[ ! -x "$PREFIX/bin/bfin-elf-gcc" ]] || [[ -z "$(find "$PREFIX/lib/gcc/bfin-elf" -name libgcc.a 2>/dev/null)" ]]; then
  say "building gcc (this takes a while)"
  mkdir -p "$TC/build-gcc" && cd "$TC/build-gcc"
  [[ -f Makefile ]] || "../src/$GCC/configure" --target=bfin-elf --prefix="$PREFIX" --enable-languages=c \
    --disable-nls --disable-werror --disable-multilib --disable-shared --disable-threads --disable-lto \
    --disable-plugin --disable-libssp --disable-libquadmath --disable-libgomp --disable-libatomic \
    --without-headers --with-newlib --with-gmp="$GMP" --with-mpfr="$MPFR" \
    --with-mpc="$MPC" --with-system-zlib --without-zstd
  "${MK[@]}" -j"$JOBS" all-gcc && "${MK[@]}" install-gcc
  "${MK[@]}" -j"$JOBS" all-target-libgcc && "${MK[@]}" install-target-libgcc
else
  say "gcc already installed, skipping"
fi

# --- GDB + simulator (simulator MUST be -O0, clang miscompiles it at -O2) -----
if [[ ! -x "$PREFIX/bin/bfin-elf-run" || ! -x "$PREFIX/bin/bfin-elf-gdb" ]]; then
  say "building gdb + simulator"
  mkdir -p "$TC/build-gdb" && cd "$TC/build-gdb"
  [[ -f Makefile ]] || "../src/$GDB/configure" --target=bfin-elf --prefix="$PREFIX" --disable-nls --disable-werror \
    --with-system-zlib --without-zstd --with-gmp="$GMP" --with-mpfr="$MPFR" \
    --disable-binutils --disable-ld --disable-gas --disable-gprof --disable-gprofng --without-python \
    --without-guile --disable-tui --with-expat=no
  "${MK[@]}" -j"$JOBS" CFLAGS="-g -O0" all-sim && "${MK[@]}" install-sim
  "${MK[@]}" -j"$JOBS" all-gdb && "${MK[@]}" install-gdb
else
  say "gdb + simulator already installed, skipping"
fi

# --- smoke test --------------------------------------------------------------
say "verifying"
for t in as ld gcc objcopy objdump nm run gdb; do
  [[ -x "$PREFIX/bin/bfin-elf-$t" ]] || die "missing $PREFIX/bin/bfin-elf-$t"
done
"$PREFIX/bin/bfin-elf-gcc" --version | head -1
cd "$SDK"
if python3 tools/test_all.py; then
  say "toolchain ready. next: python3 tools/compose.py recipes/op1plus.json /path/to/op1_246.op1"
else
  die "toolchain built, but tools/test_all.py failed (see above)"
fi
