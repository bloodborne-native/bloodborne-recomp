#!/usr/bin/env bash
# tools/recomp/replay.sh RECORDS [options]: builds tools/recomp/replay.c with the CPU core and replays
# recorded calls (docs/RECOMPILATION.md).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source tools/recomp/host.sh
D=${BB_DEPS:-deps}
mkdir -p out/recomp
clang "${arch[@]}" -O1 -g -std=gnu11 -Wall -I$D/include tools/recomp/replay.c src/cpu/cpu.c \
    src/cpu/decode.c src/cpu/interp.c src/cpu/interp_vec.c src/cpu/interp_x87.c src/cpu/jit_arm64.c \
    src/cpu/record.c src/cpu/native.c src/cpu/recomp.c src/cpu/hostcall.S $D/lib/libZydis.a $D/lib/libZycore.a -o out/recomp/replay
BB_JIT=0 exec out/recomp/replay "$@"
