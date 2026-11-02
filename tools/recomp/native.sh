#!/usr/bin/env bash
# tools/recomp/native.sh SOURCE_DIR: builds native replacements for chosen game functions
# (src/native/bbnative.h; SOURCE_DIR outside this repository) into out/recomp/libbbnative.$lib.
# The game loads it with BB_NATIVE_LIB; tools/recomp/replay.sh --native checks it.
# Floating point as on x86: no fused multiply-add (-ffp-contract=off).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source tools/recomp/host.sh
source_dir=${1:?usage: tools/recomp/native.sh SOURCE_DIR}
mkdir -p out/recomp
clang "${arch[@]}" -O2 -g -std=gnu11 -Wall -Wextra -ffp-contract=off -fno-strict-aliasing "${shared[@]}" \
    -Isrc "$source_dir"/*.c -o out/recomp/libbbnative.$lib
echo "out/recomp/libbbnative.$lib"
