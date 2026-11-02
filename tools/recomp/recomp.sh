#!/usr/bin/env bash
# tools/recomp/recomp.sh OFFSET... | --file LIST: recompiles game functions (image offsets) from out/eboot.elf
# into out/recomp/librecomp.$lib (docs/RECOMPILATION.md): tools/recomp/bbrecomp.c writes the C
# (out/recomp/gen/recomp*.c), clang compiles the files in parallel. The game loads it with
# BB_RECOMP_LIB; replay.sh --recomp checks it. Needs out/recomp/functions.tsv (tools/recomp/scan.c).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source tools/recomp/host.sh
D=${BB_DEPS:-deps}
G=out/recomp/gen
mkdir -p "$G"
offsets=("$@")
if [[ ${1:-} == --file ]]; then offsets=("@$2"); fi
clang "${arch[@]}" -O1 -g -std=gnu11 -Wall -I$D/include -I$BBCPU/src tools/recomp/bbrecomp.c $BBCPU/src/cpu/cpu.c \
    $BBCPU/src/cpu/decode.c $BBCPU/src/cpu/interp.c $BBCPU/src/cpu/interp_vec.c $BBCPU/src/cpu/interp_x87.c $BBCPU/src/cpu/jit_arm64.c \
    $BBCPU/src/cpu/record.c $BBCPU/src/cpu/native.c $BBCPU/src/cpu/recomp.c $BBCPU/src/cpu/hostcall.S $D/lib/libZydis.a \
    $D/lib/libZycore.a -o out/recomp/bbrecomp
# Generated into $G/new; only files whose C changed are compiled again (OPT: clang's -O level).
rm -rf out/recomp/gen/new; mkdir -p "$G/new"
# The project's names for functions, in comments (annotations/functions.tsv).
BB_JIT=0 BB_RECOMP_ANNOTATIONS=${BB_RECOMP_ANNOTATIONS-annotations/functions.tsv} out/recomp/bbrecomp out/eboot.elf out/recomp/functions.tsv "$G/new/recomp.c" "${offsets[@]}"
for f in "$G"/recomp*.c; do [[ -e $f ]] && [[ ! -e $G/new/$(basename "$f") ]] && rm -f "$f" "$f.o"; done
changed=()
for f in "$G"/new/recomp*.c; do
    old=$G/$(basename "$f")
    if cmp -s "$f" "$old" && [[ -e $old.o && $old.o -nt $old ]]; then continue; fi
    mv "$f" "$old"; changed+=("$old")
done
echo "${#changed[@]} files to compile"
# Floating point as on x86: no fused multiply-add.
printf '%s\n' "${changed[@]}" | grep . | xargs -P "$jobs" -I{} clang "${arch[@]}" -O${OPT:-2} -g -std=gnu11 \
    -Wall -Wno-unused-label -Wno-unused-variable -ffp-contract=off -fno-strict-aliasing -fvisibility=hidden \
    -I$D/include -I$BBCPU/src -c {} -o {}.o || true
for f in "${changed[@]}"; do [[ -e $f.o && $f.o -nt $f ]] || { echo "failed: $f"; exit 1; }; done
clang "${arch[@]}" "${shared[@]}" "$G"/recomp*.o -o out/recomp/librecomp.$lib
echo "out/recomp/librecomp.$lib"
