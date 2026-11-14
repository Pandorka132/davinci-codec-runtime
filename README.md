# DaVinci Codec Runtime

Experimental in-process codec integration for **DaVinci Resolve 21.1.1.0010 on Linux x86-64**. The first milestone is AV1 decoding through a private FFmpeg 6.1.6 build, preferring Intel Quick Sync Video (QSV) when an accessible Intel render node is available and falling back to libdav1d.

> This project patches the running Resolve process. It is build-specific and still under active validation. Use the project launcher rather than starting the Resolve binary directly.

## Build

Requirements:

- DaVinci Resolve 21.1.1.0010 (the executable fingerprint is checked; other builds fail closed)
- Clang++ with libc++
- CMake 3.24 or newer, `make`, `tar`, `curl`, `sha256sum`, `pkg-config`, `patchelf`
- GDB
- libdav1d development package (`dav1d`, runtime SONAME `libdav1d.so.7`)
- libva development package (`libva`) and Intel oneVPL development package (`vpl >= 2.6`); runtime Intel media driver and render-node access are required for QSV

From this directory:

```sh
./scripts/build.sh
```

The default build uses Clang with libc++, matching Resolve's C++ exception runtime, and produces `build/clang-libcxx/libdavinci_codec_runtime.so`. Set `BUILD_DIR` to select a different build directory; `CXX` may select another Clang++ binary and `CXXFLAGS` may add compiler flags. GCC/libstdc++ is not a supported runtime build configuration. The runtime, private FFmpeg provider, callback replacement, and validated in-memory Resolve installer are implemented. Provider tests cover the FFmpeg boundary; live GUI playback/teardown remains a separate validation requirement.

The build script downloads the pinned FFmpeg 6.1.6 source archive from the official FFmpeg release server into the ignored `build/downloads/` directory, verifies its SHA-256 and version, then builds only `libavcodec` and `libavutil` with the libdav1d and QSV AV1 decoders. It assigns the private FFmpeg DSOs unique `libdavinci_*` SONAMEs so they can coexist with Resolve's FFmpeg libraries while oneVPL loads its VA driver plugin in the base linker namespace. The first build therefore needs network access; subsequent builds reuse the verified local archive. It does not need the Ghidra project, reverse-engineering reports, or any files outside this repository.

The script writes a local `build/clang-libcxx/artifact-manifest.txt`, including the libdav1d and oneVPL versions. The session controller uses the generated hashes to attest the exact binaries built on that machine; the manifest is a generated build artifact and is not committed.


## Goals
- [X] Add AV1 decoder (to replace the single threaded AOM)
- [ ] Add AV1 encoder (for exporting)
- [ ] Add h.264/h.265 encoder/decoder


## Start Resolve with the patch

```sh
./scripts/start.sh
```

The AV1 patch is automatic. There is no normal unpatched fallback path: if the build, fingerprint, mapping, ABI, or transaction checks fail, startup stops rather than launching an unmodified Resolve process. The current controller is pinned to `/opt/resolve/bin/resolve`; non-default executable paths are not supported.

The installer runs Resolve under GDB, validates the target, applies the in-memory transaction, and resumes the GUI with the provider resident. It does not modify `/opt/resolve`, the Resolve executable, or Resolve's bundled libraries. Close Resolve normally to end the session.

## Current scope and validation

- AV1 decode: NVIDIA remains first in normal mode; the private FFmpeg provider prefers Intel QSV when exactly one accessible Intel render node is found, otherwise it uses libdav1d. Resolve's AOM factory is fallback only when the custom factory returns no decoder object.
- `Local.IO.SoftwareAV1Decode="1"` skips both NVIDIA and QSV and selects private libdav1d before AOM.
- Provider-level QSV decode tests pass for 8-bit NV12 and 10-bit P010 output (6/6 frames each); software-only selection is also tested. These are not yet Resolve GUI playback tests. A 4K Resolve playback session was reported smooth, but an earlier session crashed during context teardown because Resolve-allocated extradata was freed by the private FFmpeg allocator. The allocator-ownership correction is implemented and covered by tests. A later GUI run then hit a separate SIGSEGV in Resolve-side `memcpy`/string-copy code; its relationship to the provider is not yet known, and a fresh GUI retest with improved crash tracing is required.
- SVT-AV1 encoding, HEVC, AAC, and other codec integrations are not implemented yet.

See [architecture](docs/architecture.md), [testing](docs/testing.md), [research notes](docs/research-notes.md), the [benchmark summary](docs/benchmark-summary.md), the [extradata ownership investigation](docs/av1-extradata-ownership.md), and the [validation matrix](docs/validation-matrix.md). See [CONTRIBUTING.md](CONTRIBUTING.md) for PR requirements.

## Layout

- `include/`, `src/`: runtime API, callback policy, codec definitions, provider wrappers
- `installer/`: bootstrap, session controller, GDB session script, authorization and target-identity code
- `scripts/`: reproducible build and automatic patched startup
- `tests/`: unit, transaction, provider, and small AV1 sample tests
- `third_party/`: third-party source and licensing notes; downloaded archives stay under ignored `build/`
- `docs/`: design, reverse-engineering findings, validation status, and PR context
- `reports/`: local runtime diagnostics; generated logs are not required to build

The large Ghidra database and exploratory reverse-engineering workspace are deliberately not part of this repository. The relevant findings and reproducible source inputs are summarized here. The project's own license has not yet been selected; see [contributing](CONTRIBUTING.md) before preparing a public release.
