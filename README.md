# DaVinci Codec Runtime

In-memory codec backend integration for DaVinci Resolve Free. 

## Build

```sh
./scripts/build.sh
```

The default build uses Clang with libc++, matching Resolve's C++ exception runtime, and produces `build/clang-libcxx/libdavinci_codec_runtime.so`. Set `BUILD_DIR` to select a different build directory; `CXX` may select another Clang++ binary and `CXXFLAGS` may add compiler flags. GCC/libstdc++ is not a supported runtime build configuration. The current library exposes a version/name ABI marker. A Resolve-independent codec-definition model, validation, FourCC lookup, and generic backend-plan dispatcher are implemented. The AV1 declaration is tested with simulated backend providers; Resolve integration is not implemented yet.

## Goals
-[ ] Add AV1 decoder (to replace the single threaded AOM) WIP
-[ ] Add AV1 encoder (for exporting)
-[ ] Add h.264/h.265 encoder/decoder

## Test

```sh
ctest --test-dir build/clang-libcxx --output-on-failure
```

## Start Resolve

```sh
./scripts/start.sh
```

This currently starts Resolve normally. It does not inject or load the runtime library.

## Layout

- `include/`: public runtime interfaces
- `src/`: runtime implementation and codec declarations (`src/codecs/`)
- `scripts/`: build and launch entry points
- `tests/`: isolated tests
- `docs/`: design and integration notes

Reverse-engineering databases, FFmpeg source/build trees, generated binaries, and research logs stay outside this Git project.
