# Project goal and non-negotiable priorities

## Primary goal

Accelerate DaVinci Resolve's AV1 decoding by routing AV1 decode requests through our own faster decoder before Resolve's internal AOM decoder gets a chance to claim the request.

This is a decoder-performance project. It is not a project to add another last-resort software decoder, and it is not primarily about broad codec support for its own sake.

## Required AV1 backend order

Normal mode:

1. Resolve's existing NVIDIA hardware decoder.
2. Our custom accelerated decoder through Resolve's generic FFmpeg factory: Intel QSV when available, otherwise private FFmpeg/libdav1d.
3. Resolve's internal AOM decoder as fallback.

When `Local.IO.SoftwareAV1Decode` is exactly `"1"`:

1. Our custom software decoder through private FFmpeg/libdav1d (QSV is bypassed).
2. Resolve's internal AOM decoder as fallback.

**The custom decoder must always be attempted before AOM. Never reverse these two.** If AOM's factory successfully creates a decoder object, Resolve considers the request handled and does not proceed to our decoder.

Fallback is defined at factory creation: continue only when the current factory returns no decoder object. A later stream-open or decode failure is not automatically a factory fallback.

## Scope and sequencing

- AV1 decode acceleration is first priority.
- Preserve NVIDIA as the normal-mode hardware-first route.
- Use SVT-AV1 for the later AV1 encoding work.
- Expand to other codecs only after the AV1 path is stable; H.265 is a later expansion, not a change to the current goal.
- Keep codec definitions and backend policy declarative.
- The custom FFmpeg route prefers QSV hardware decode when available and retains libdav1d as its software fallback. The explicit software-only Resolve setting bypasses both NVIDIA and QSV.

## Engineering constraints

- Prefer the least invasive, best-supported integration point and preserve unrelated Resolve codec paths.
- Do not globally redirect unrelated FFmpeg calls.
- Do not modify Resolve's on-disk executable or bundled libraries.
- Validate the exact Resolve build and every patch site before touching process memory.
- Do not read any `.env` file.
- Keep important project files out of `/tmp`.

## Performance evidence

The local benchmark measured libdav1d at approximately 3.1x the throughput of libaom-av1 across the tested 1080p 8-bit, 1080p 10-bit, and 4K 10-bit streams on the development host. The measurements used 14 threads, one warm-up, and three measured runs. Hardware decode results and limitations are summarized in [benchmark-summary.md](benchmark-summary.md).

## Current integration status

The AV1 callback/map/CALL transaction and private provider are implemented. Provider tests pass for QSV 8-bit and 10-bit AV1; software-only selection is covered too. A patched Resolve session reached successful packet/frame calls and a 4K playback was reported smooth. That session exposed a teardown crash caused by cross-allocator `extradata` ownership. The ownership fix is implemented and covered by provider tests. A later GUI run produced a separate SIGSEGV in Resolve-side string-copy code, with its relationship to the provider still unknown; a fresh GUI playback-and-normal-close session with improved crash tracing is still required.

## Change control

The goal and backend priorities above are fixed project requirements. Do not move AOM ahead of the custom decoder or redefine the task as merely adding a software fallback without asking first.
