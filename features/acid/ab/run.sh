#!/bin/sh
# A/B: ACID's ac_core against the original Open303 (MIT; cloned into build/open303, never committed).
#   sh features/acid/ab/run.sh        (from the SDK root)
set -e
SDK=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=$SDK/build/acid_ab
O303=$SDK/build/open303
[ -d "$O303" ] || git clone --depth 1 https://github.com/RobinSchmidt/Open303.git "$O303"
D=$O303/Source/DSPCode
mkdir -p "$OUT"
c++ -O2 -std=c++11 -w -I"$D" -o "$OUT/ref" "$SDK/features/acid/ab/ref.cpp" "$D"/rosic_*.cpp "$D"/GlobalFunctions.cpp
cc -std=c99 -O2 -I"$SDK/features/acid" -o "$OUT/ours" "$SDK/features/acid/ab/ours.c" "$SDK/features/acid/ac_core.c" -lm
cp "$SDK/features/acid/ab/ab.py" "$OUT/" && python3 -I "$OUT/ab.py" "$@"
