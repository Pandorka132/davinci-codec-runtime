# Codec definitions and AV1 policy

Codec metadata is kept declarative so later codecs can reuse the same validation, FourCC lookup, and backend-policy machinery.

## AV1 definition

| Field | Value |
|---|---|
| Internal ID | `av1` |
| FourCC | `av01` (`0x61763031`) |
| FFmpeg codec ID | `AV_CODEC_ID_AV1` (`226`) |
| Initial operation | Decode |
| Hardware implementation | Private FFmpeg 6.1.6 + Intel QSV (`av1_qsv`) |
| Software fallback | Private FFmpeg 6.1.6 + libdav1d |

The Resolve generic FFmpeg map is extended by one node, `av01 -> 226`. Existing map entries are preserved.

## Backend policy

Normal mode:

1. `resolve.nvidia.decoder`
2. `resolve.generic_ffmpeg.decoder` (QSV when an accessible Intel render node is available; otherwise libdav1d)
3. `resolve.aom.av1.decoder`

When `Local.IO.SoftwareAV1Decode` is exactly `"1"`:

1. `resolve.generic_ffmpeg.decoder` (libdav1d; QSV is explicitly bypassed)
2. `resolve.aom.av1.decoder`

AOM is a fallback, not the preferred software route. The custom factory is considered unavailable only when it returns no decoder object. Open/decode failures after successful factory creation are not converted into backend fallback.

## Definition invariants

- Internal IDs are non-empty and unique.
- FourCC aliases are unique across the registry.
- An FFmpeg-backed codec has a positive FFmpeg codec ID.
- Decoder and encoder policies are independent.
- Each candidate names a registered provider, an optional activation condition, and a failure policy.
- The complete definition set is validated before registry or Resolve map mutation.

The current implementation integrates AV1 decode only. Encoder definitions and SVT-AV1 registration are future work.
