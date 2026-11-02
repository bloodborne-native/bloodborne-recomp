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
