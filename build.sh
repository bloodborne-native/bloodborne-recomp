#!/usr/bin/env bash
# build.sh: the recompiler's tools (out/recomp/scan, out/recomp/bbrecomp) on bbcpu, the CPU layer, a
# sibling checkout (BBCPU: another path). Zydis, the x86 decoder, is built once into deps/
# (BB_DEPS: another prefix with libZydis.a and libZycore.a). Needs clang, cmake and ninja.
set -euo pipefail
cd -- "$(dirname -- "$0")"
D=${BB_DEPS:-deps}
CC=${CC:-clang}
source tools/recomp/host.sh

# bbcpu builds Zydis into the same prefix the first time.
prefix=$(mkdir -p "$D" && cd "$D" && pwd)
BB_DEPS=$prefix bash "$BBCPU/build.sh"
mkdir -p out/recomp

"$CC" "${arch[@]}" -O2 -I"$D/include" tools/recomp/scan.c "$D/lib/libZydis.a" "$D/lib/libZycore.a" -o out/recomp/scan
"$CC" "${arch[@]}" -O1 -g -std=gnu11 -Wall -I"$D/include" -I"$BBCPU/src" tools/recomp/bbrecomp.c "$BBCPU/out/libbbcpu.a" \
    "$D/lib/libZydis.a" "$D/lib/libZycore.a" -o out/recomp/bbrecomp
echo "out/recomp/scan, out/recomp/bbrecomp"
