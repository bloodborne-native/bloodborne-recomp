#!/usr/bin/env bash
# tools/recomp/replay.sh RECORDS [options]: builds tools/recomp/replay.c with the CPU core and replays
# recorded calls (docs/RECOMPILATION.md).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source tools/recomp/host.sh
D=${BB_DEPS:-deps}
mkdir -p out/recomp
clang "${arch[@]}" -O1 -g -std=gnu11 -Wall -I$D/include -I$BBCPU/src tools/recomp/replay.c $BBCPU/src/cpu/cpu.c \
    $BBCPU/src/cpu/decode.c $BBCPU/src/cpu/interp.c $BBCPU/src/cpu/interp_vec.c $BBCPU/src/cpu/interp_x87.c $BBCPU/src/cpu/jit_arm64.c \
    $BBCPU/src/cpu/record.c $BBCPU/src/cpu/native.c $BBCPU/src/cpu/recomp.c $BBCPU/src/cpu/hostcall.S $D/lib/libZydis.a $D/lib/libZycore.a -o out/recomp/replay
BB_JIT=0 exec out/recomp/replay "$@"
