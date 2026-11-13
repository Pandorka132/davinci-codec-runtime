#!/usr/bin/env bash

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/clang-libcxx}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
CXX_COMPILER="${CXX:-clang++}"
CXX_FLAGS="${CXXFLAGS:-}"

command -v cmake >/dev/null 2>&1 || {
    printf '%s\n' 'Error: cmake is required.' >&2
    exit 1
}

command -v "$CXX_COMPILER" >/dev/null 2>&1 || {
    printf 'Error: C++ compiler not found: %s\n' "$CXX_COMPILER" >&2
    exit 1
}

case "$CXX_COMPILER" in
    *clang*)
        CXX_FLAGS="${CXX_FLAGS:+$CXX_FLAGS }-stdlib=libc++"
        ;;
    *)
        printf '%s\n' 'Error: Clang++ with libc++ is required for Resolve exception-ABI compatibility.' >&2
        exit 1
        ;;
esac

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
    -DCMAKE_CXX_FLAGS="$CXX_FLAGS" || exit $?

cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-$(nproc)}" || exit $?
