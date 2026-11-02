#!/usr/bin/env bash
# tools/recomp/fuzz.sh [trials] [max reports]: fuzzes the recompiler's C against the interpreter, one
# instruction at a time (tests/fuzz_recomp.c), over every distinct encoding in out/eboot.elf (at most
# 30 per mnemonic, $BBCPU/tools/cpu/encodings.c).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source tools/recomp/host.sh
D=${BB_DEPS:-deps}
CPU="$BBCPU/src/cpu/cpu.c $BBCPU/src/cpu/decode.c $BBCPU/src/cpu/interp.c $BBCPU/src/cpu/interp_vec.c $BBCPU/src/cpu/interp_x87.c $BBCPU/src/cpu/jit_arm64.c
     $BBCPU/src/cpu/record.c $BBCPU/src/cpu/native.c $BBCPU/src/cpu/recomp.c $BBCPU/src/cpu/hostcall.S"
mkdir -p out/recomp/gen
clang "${arch[@]}" -O2 -std=gnu11 -I$D/include $BBCPU/tools/cpu/encodings.c $D/lib/libZydis.a $D/lib/libZycore.a -o out/recomp/encodings
out/recomp/encodings out/eboot.elf all 30 > out/recomp/encodings.txt
clang "${arch[@]}" -O1 -g -std=gnu11 -I$D/include -I$BBCPU/src tools/recomp/bbrecomp.c $CPU $D/lib/libZydis.a $D/lib/libZycore.a -o out/recomp/bbrecomp
BB_JIT=0 out/recomp/bbrecomp --snippets out/recomp/encodings.txt out/recomp/gen/snippets.c
clang "${arch[@]}" -O1 -std=gnu11 -Wno-unused-label -Wno-unused-variable -ffp-contract=off -fno-strict-aliasing \
    "${shared[@]}" -I$D/include -I$BBCPU/src out/recomp/gen/snippets.c -o out/recomp/libsnippets.$lib
clang "${arch[@]}" -O2 -g -std=gnu11 -I$D/include -I$BBCPU/src tests/fuzz_recomp.c $CPU $D/lib/libZydis.a $D/lib/libZycore.a -o out/recomp/fuzz_recomp
BB_JIT=0 out/recomp/fuzz_recomp out/recomp/encodings.txt out/recomp/libsnippets.$lib "$@"
