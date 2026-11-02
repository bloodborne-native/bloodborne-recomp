# Sourced by the build scripts: this machine's compiler flags and library names.
#   clang "${arch[@]}"          any target clang is run on
#   clang "${arch[@]}" "${shared[@]}" ... -o "out/recomp/libX.$lib"
# BB_ARCH builds for another architecture on macOS (x86_64 on Apple Silicon runs under Rosetta).
arch=()
case $(uname -s) in
Darwin)
    arch=(-arch "${BB_ARCH:-$(uname -m)}")
    shared=(-dynamiclib)
    lib=dylib
    jobs=$(sysctl -n hw.ncpu)
    ;;
*)
    shared=(-shared -fPIC)
    lib=so
    jobs=$(nproc)
    ;;
esac

# bbcpu, the CPU layer: a sibling checkout (runtime/bbcpu next to runtime/recomp), or BBCPU. The
# recompiler is written against one version of its interface, in tools/recomp/bbcpu_api.
BBCPU=${BBCPU:-../bbcpu}
if [[ ! -f $BBCPU/src/recomp/rc.h ]]; then
    echo "No bbcpu at $BBCPU: clone https://github.com/bloodborne-native/bbcpu there, or set BBCPU" >&2
    exit 1
fi
bbcpu_api=$(sed -n 's/^#define BB_RECOMP_API_VERSION \([0-9]*\)u.*/\1/p' "$BBCPU/src/recomp/rc.h")
if [[ $bbcpu_api != "$(cat tools/recomp/bbcpu_api)" ]]; then
    echo "bbcpu at $BBCPU has interface $bbcpu_api; this recompiler needs $(cat tools/recomp/bbcpu_api)" >&2
    exit 1
fi
