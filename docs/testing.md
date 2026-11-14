# Testing and evidence

## Build and unit tests

```sh
./scripts/build.sh
ctest --test-dir build/clang-libcxx --output-on-failure
```

The current local run passes 10/10 tests:

- codec-definition validation and lookup
- AV1 backend policy
- callback policy
- session-controller API
- install authorization
- 8-bit and 10-bit private provider decode (QSV NV12/P010)
- bootstrap transaction API
- target-identity range validation
- transaction executor and rollback simulation

The provider tests decode six IVF packets into six frames for both 8-bit and 10-bit AV1 through QSV (NV12/P010) on the development workstation, and verify that the software-only switch selects libdav1d. A forced VA driver initialization failure also exercises the automatic libdav1d fallback. They also exercise Resolve-owned VP9 forwarding, AV1 context lifecycle, frame transfer, multithreaded context lifecycle (software path), and provider shutdown coordination. The allocator-ownership regression test uses Resolve's `av_mallocz` to create foreign extradata and checks both unopened-context cleanup and the pre-open ownership transfer. The current provider tests cannot run under ASan because the sanitizer runtime explicitly rejects `RTLD_DEEPBIND`; this is a loader/tooling incompatibility, not a test assertion. The other eight ASan/UBSan tests pass. QSV hardware execution has been validated in the regular provider tests, not under sanitizers. The pinned FFmpeg archive was also extracted into a clean build directory, configured, compiled, installed, and both provider tests passed against those freshly built libraries. The build script now downloads that pinned archive into `build/downloads/` and verifies its SHA-256 before extraction; the first build requires network access.

## Live transaction validation

A disposable Resolve child previously passed 74 live checks covering the 72-node map after insertion, all ten CALL redirects, callback detour, relay page protection, wrapper target validation, rollback, and byte-exact restoration. The child was not resumed with the patch installed.

## GUI validation

A persistent patched Resolve session reached the custom AV1 provider. The trace contained successful `avcodec_send_packet` and `avcodec_receive_frame` returns; `-11` (`EAGAIN`) and `AVERROR_EOF` appeared in normal drain conditions. A 4K playback was reported smooth. The same session crashed on context cleanup because Resolve-owned extradata was freed by the private FFmpeg allocator.

The provider now rehomes extradata into the private allocator and passes regression tests. A later GUI run produced a separate SIGSEGV in Resolve-side string-copy code (`memcpy`), with an invalid source pointer and implausible copy length. The previous crash log's recent-event section was contaminated by mapping output; the GDB script now snapshots provider events before diagnostics and records the caller stack/object. This updated instrumentation has not yet been exercised in a new GUI run. **The next required integration test is a fresh GUI session using the QSV-enabled provider: play 8-bit, 10-bit, and 4K AV1, stop playback, close the project, and close Resolve normally; if it crashes, inspect the corrected provider-event and caller-stack capture.** Neither GUI teardown nor the newer SIGSEGV is considered resolved.

## What a test does not prove

Provider tests do not install the Resolve callback or map patch. Transaction tests do not prove UI playback. A smooth playback session does not prove safe teardown. Keep these validation layers separate in PRs and issue reports.
