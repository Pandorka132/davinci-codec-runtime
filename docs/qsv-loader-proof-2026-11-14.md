# QSV loading with private FFmpeg SONAMEs

Date: 2026-11-14

## Result

The private-FFmpeg/main-linker-namespace approach works on the target workstation. A test process first loads the Resolve-bundled `libavutil.so.58` and `libavcodec.so.60`, then loads the private FFmpeg build using `RTLD_LOCAL | RTLD_DEEPBIND`. The private FFmpeg libraries use distinct SONAMEs, so they coexist with Resolve's libraries without being mistaken for the same ELF objects.

The test successfully initializes Intel QSV through oneVPL/VAAPI and decodes both test streams:

- AV1 8-bit: 6/6 frames, downloaded as NV12.
- AV1 10-bit: 6/6 frames, downloaded as P010.
- Explicit Intel render node: `/dev/dri/renderD128`.
- QSV auto-selection also decoded the 8-bit sample, but production selection should remain explicit.

The render node is confirmed as Intel Arrow Lake-S at PCI `0000:00:02.0`, driver `i915`. The AMD RX 6700 XT is `0000:04:00.0` and maps to `/dev/dri/renderD129`.

## Experimental ELF layout

The first loader experiment used a copy under `build/qsv-soname-test/lib`. The current build script now applies the same SONAME transformation reproducibly to the selected private FFmpeg prefix; it never patches Resolve libraries.

- `libavutil.so.58` SONAME changed to `libdavinci_avutil.so.58`.
- `libavcodec.so.60` SONAME changed to `libdavinci_avcodec.so.60`.
- Private libavcodec's `DT_NEEDED` reference changed from `libavutil.so.58` to `libdavinci_avutil.so.58`.
- Private libavcodec RUNPATH set to `$ORIGIN`.
- The probe opens Resolve's FFmpeg libraries first, then opens private util/codec with `RTLD_DEEPBIND`.

This avoids the previous `dlmopen(LM_ID_NEWLM)` failure: oneVPL's VA driver plugin loader runs in the normal process linker namespace, while the private FFmpeg ABI pair remains distinguishable by SONAME and is selected through its own handles.

## Reproduction

From the repository root:

```sh
cc -std=gnu11 -Wall -Wextra -Werror \
  -Ibuild/private-ffmpeg-qsv-vaapi/include \
  tests/qsv_namespace_probe.c -ldl -o build/qsv_namespace_probe

build/qsv_namespace_probe \
  "$PWD/build/qsv-soname-test" \
  /dev/dri/renderD128 tests/data/av1-8bit.ivf

build/qsv_namespace_probe \
  "$PWD/build/qsv-soname-test" \
  /dev/dri/renderD128 tests/data/av1-10bit.ivf
```

Both invocations returned success and reported six decoded frames.

## Build configuration

The private FFmpeg 6.1.6 build has these enabled:

- `CONFIG_LIBVPL=yes`
- `CONFIG_VAAPI=yes`
- `CONFIG_QSV=yes`
- `CONFIG_QSVDEC=yes`
- `CONFIG_AV1_QSV_DECODER=yes`

The build script now requires oneVPL development files (pkg-config module `vpl >= 2.6`) and libva development files, and enables `libvpl`, `av1_qsv`, and VAAPI.

## Provider integration result

The SONAME transformation is now part of `scripts/build.sh`. The production provider uses `dlopen(..., RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND)` and resolves all private FFmpeg APIs through the returned handles. The old `dlmopen` namespace path has been removed.

The provider detects a single accessible Intel render node by reading the DRM device PCI vendor ID (`0x8086`), creates a shared QSV device context, and selects `av1_qsv`. If no unique accessible Intel node or QSV device is available, it selects private `libdav1d`. The Resolve software-only setting bypasses both NVIDIA and QSV and selects libdav1d. QSV output is transferred to private system-memory frames before the existing Resolve-compatible copy path.

Provider tests now exercise QSV decode through the real wrapper path:

- 8-bit: 6 packets → 6 NV12 frames, 128x72.
- 10-bit: 6 packets → 6 P010 frames, 128x72.
- The software-only selector returns `libdav1d` even when QSV is available.
- With `LIBVA_DRIVER_NAME` deliberately set to a nonexistent driver, QSV initialization fails and the provider successfully falls back to libdav1d (6/6 frames).
- Resolve-owned VP9 calls remain forwarded to Resolve FFmpeg60.
- The full CTest suite passes 10/10.

## Remaining validation

This is still a provider-level integration, not proof of Resolve GUI playback. Odd-dimension QSV frames, long-form 4K/4K180 playback, device-loss/reinitialization, and a fresh Resolve GUI teardown have not been validated. The QSV device is retained for provider lifetime and released after the context registry is empty; hot device-loss recovery is not implemented. A QSV open/decode failure after successful factory selection is not silently switched to libdav1d or AOM, preserving the project's factory-only fallback rule.

No Resolve binary or bundled library under `/opt/resolve` was modified. No live Resolve process was patched or restarted for this integration.
