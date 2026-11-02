#!/usr/bin/env bash
# tools/recomp/fuzz.sh [trials] [max reports]: fuzzes the recompiler's C against the interpreter, one
# instruction at a time (tests/fuzz_recomp.c), over every distinct encoding in out/eboot.elf (at most
# 30 per mnemonic, tools/cpu/encodings.c).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source tools/recomp/host.sh
D=${BB_DEPS:-deps}
CPU="src/cpu/cpu.c src/cpu/decode.c src/cpu/interp.c src/cpu/interp_vec.c src/cpu/interp_x87.c src/cpu/jit_arm64.c
     src/cpu/record.c src/cpu/native.c src/cpu/recomp.c src/cpu/hostcall.S"
mkdir -p out/recomp/gen
clang "${arch[@]}" -O2 -std=gnu11 -I$D/include tools/cpu/encodings.c $D/lib/libZydis.a $D/lib/libZycore.a -o out/recomp/encodings
out/recomp/encodings out/eboot.elf all 30 > out/recomp/encodings.txt
clang "${arch[@]}" -O1 -g -std=gnu11 -I$D/include tools/recomp/bbrecomp.c $CPU $D/lib/libZydis.a $D/lib/libZycore.a -o out/recomp/bbrecomp
BB_JIT=0 out/recomp/bbrecomp --snippets out/recomp/encodings.txt out/recomp/gen/snippets.c
clang "${arch[@]}" -O1 -std=gnu11 -Wno-unused-label -Wno-unused-variable -ffp-contract=off -fno-strict-aliasing \
    "${shared[@]}" -I$D/include -Isrc out/recomp/gen/snippets.c -o out/recomp/libsnippets.$lib
clang "${arch[@]}" -O2 -g -std=gnu11 -I$D/include tests/fuzz_recomp.c $CPU $D/lib/libZydis.a $D/lib/libZycore.a -o out/recomp/fuzz_recomp
BB_JIT=0 out/recomp/fuzz_recomp out/recomp/encodings.txt out/recomp/libsnippets.$lib "$@"
