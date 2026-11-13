# Project goal and non-negotiable priorities

## Primary goal

Accelerate DaVinci Resolve's AV1 decoding by routing AV1 decode requests through our own faster decoder before Resolve's internal AOM decoder gets a chance to claim the request.

This is a decoder-performance project. It is not a project to add another last-resort software decoder, and it is not primarily about broad codec support for its own sake.

## Required AV1 backend order

Normal mode:

1. Resolve's existing NVIDIA hardware decoder.
2. Our custom accelerated decoder, currently implemented through Resolve's generic FFmpeg decoder object and a private FFmpeg 6.1.6 namespace.
3. Resolve's internal AOM decoder as fallback.

When `Local.IO.SoftwareAV1Decode` is exactly `"1"`:

1. Our custom accelerated decoder.
2. Resolve's internal AOM decoder as fallback.

**The custom decoder must always be attempted before AOM. Never reverse these two.** If AOM's factory successfully creates a decoder object, Resolve considers the request handled and does not proceed to our decoder. Therefore putting AOM first defeats the entire acceleration goal; AOM will not fail merely to let our backend load.

Fallback is defined at factory creation: continue to the next candidate only when the current factory returns no decoder object. A later stream-open or decode failure is not automatically a factory fallback.

## Scope and sequencing

- AV1 decode acceleration is first priority.
- The custom AV1 decoder is the preferred software route; AOM is the safety fallback.
- Preserve NVIDIA as the normal-mode hardware-first route.
- Use SVT-AV1 for the later AV1 encoding work; do not silently change the first milestone into an encoder project.
- Expand to other codecs only after the AV1 path is working; H.265 is a later expansion/test, not a change to the current goal.
- Keep the architecture declarative and extensible so additional codec definitions do not require codec-specific runtime branches.
- Keep the existing Resolve NVIDIA route independent from any future custom hardware path. The current generic FFmpeg decoder route is software-only.
- Do not claim integration works in Resolve until it has been tested in Resolve.

## Engineering constraints

- Prefer the least invasive, best-supported integration point and preserve unrelated Resolve codec paths.
- Do not replace Resolve's whole FFmpeg stack or globally redirect unrelated FFmpeg calls.
- Do not modify Resolve's on-disk executable or bundled libraries.
- Runtime hooks, process-memory modifications, DSO injection, and live Resolve patching require explicit user authorization before implementation or testing.
- Do not read any `.env` file.
- Keep important project files out of `/tmp`.

## Current measured evidence (2026-11-13)\n\n- In the project's isolated FFmpeg 6.1.6 build, libdav1d decoded the 1080p8, 1080p10 and 4K10 test streams at approximately 3.1x the throughput of libaom-av1 (14 threads, one warmup plus three measured runs).\n- AMD Radeon RX 6700 XT AV1 Profile 0 VA-API decoding was verified for 8-bit and 10-bit Main-profile samples. In the host FFmpeg 9 same-version comparison, its GPU-surface throughput was below dav1d on the Core Ultra 5 245K; downloading frames to system memory reduced throughput further.\n- Intel Quick Sync remains unmeasured because the current Linux boot exposes no Intel display-class PCI device or Intel DRM render node, despite the CPU's integrated GPU.\n- Full methodology and measurements: `reverse_engineering/reports/av1-decoder-benchmark-2026-11-13.md`.\n\n## Change control

The goal and backend priorities above are fixed project requirements. Do not reinterpret, optimize away, or change them without asking the user first. In particular, do not move AOM ahead of the custom decoder, and do not redefine the task as merely adding a software fallback.
