#!/usr/bin/env bash
# build.sh: bbcpu (out/libbbcpu.a, the CPU layer programs link: src/cpu) and the recompiler's tools
# (out/recomp/scan, out/recomp/bbrecomp). Zydis, the x86 decoder, is built once into deps/
# (BB_DEPS: another prefix with libZydis.a and libZycore.a). Needs clang, cmake and ninja.
set -euo pipefail
cd -- "$(dirname -- "$0")"
D=${BB_DEPS:-deps}
ZYDIS_VERSION=v4.1.1
CC=${CC:-clang}
source tools/recomp/host.sh

# cmake and ninja are only needed for Zydis; check before cloning it, not halfway through.
missing=
for tool in "$CC" $([[ -f $D/lib/libZydis.a ]] || echo cmake ninja); do
    command -v "$tool" >/dev/null || missing+=" $tool"
done
if [[ -n $missing ]]; then
    echo "build.sh: missing$missing (macOS: brew install cmake ninja; clang: xcode-select --install)" >&2
    exit 1
fi

if [[ ! -f $D/lib/libZydis.a ]]; then
    echo "== zydis $ZYDIS_VERSION into $D"
    prefix=$(mkdir -p "$D" && cd "$D" && pwd)
    work=$prefix/build
    mkdir -p "$work"
    [[ -d $work/zydis ]] || git -c advice.detachedHead=false clone -q --recursive --depth 1 -b "$ZYDIS_VERSION" https://github.com/zyantific/zydis.git "$work/zydis"
    common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF)
    [[ ${#arch[@]} == 0 ]] || common+=(-DCMAKE_OSX_ARCHITECTURES="${arch[1]}")
    # Zydis and Zycore ask for CMake < 3.10, which CMake 4 deprecates; 3.10 policies keep them building.
    common+=(-DCMAKE_POLICY_VERSION_MINIMUM=3.10)
    # Zydis builds its bundled Zycore without install rules: Zycore first, then Zydis on it.
    cmake -S "$work/zydis/dependencies/zycore" -B "$work/zycore-build" "${common[@]}" >/dev/null
    cmake --build "$work/zycore-build" >/dev/null && cmake --install "$work/zycore-build" >/dev/null
    cmake -S "$work/zydis" -B "$work/zydis-build" "${common[@]}" -DZYAN_SYSTEM_ZYCORE=ON \
        -DCMAKE_C_FLAGS="-I$prefix/include" -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF \
        -DZYDIS_BUILD_DOXYGEN=OFF >/dev/null
    cmake --build "$work/zydis-build" >/dev/null && cmake --install "$work/zydis-build" >/dev/null
fi

mkdir -p out/cpu out/recomp
for source in src/cpu/*.c src/cpu/hostcall.S; do
    "$CC" "${arch[@]}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -I"$D/include" \
        -c "$source" -o "out/cpu/$(basename "$source").o"
done
rm -f out/libbbcpu.a && ar rcs out/libbbcpu.a out/cpu/*.o
echo "out/libbbcpu.a"

"$CC" "${arch[@]}" -O2 -I"$D/include" tools/recomp/scan.c "$D/lib/libZydis.a" "$D/lib/libZycore.a" -o out/recomp/scan
"$CC" "${arch[@]}" -O1 -g -std=gnu11 -Wall -I"$D/include" tools/recomp/bbrecomp.c out/libbbcpu.a \
    "$D/lib/libZydis.a" "$D/lib/libZycore.a" -o out/recomp/bbrecomp
echo "out/recomp/scan, out/recomp/bbrecomp"
