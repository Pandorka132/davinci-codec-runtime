# Resolve AV1 callback integration

## Observed callback contract

For Resolve 21.1.1.0010, the AV1 callback is `FUN_05e3deb0` at image offset `0x05e3deb0`. Its supported operation is `1`; the callback accepts a properties pointer and returns a codec object pointer.

The stock callback order is:

1. Read Resolve's cached `Local.IO.SoftwareAV1Decode` condition.
2. Try the NVIDIA factory unless the value is exactly `"1"`.
3. Try the internal AOM factory if the preceding route returned no object.

The custom integration preserves the NVIDIA-first behavior and inserts the generic FFmpeg factory before AOM. In software-only mode it skips NVIDIA and tells the provider to bypass QSV, then tries private libdav1d, then AOM. In normal mode, the provider prefers QSV when one accessible Intel render node is found and otherwise uses libdav1d.

## Relevant build-specific functions

| Function | Image offset | Role |
|---|---:|---|
| AV1 callback | `0x05e3deb0` | Callback entry and dispatch |
| NVIDIA factory | `0x05f2cb30` | Existing hardware route |
| Generic FFmpeg factory | `0x05eeef00` | Custom FFmpeg route (QSV/libdav1d) |
| AOM factory | `0x05f90830` | Existing software fallback |
| Map rebalancer | `0x00ecdba0` | Resolve red-black tree rebalancing |

These offsets are only meaningful after the executable fingerprint has been verified. The installer does not treat them as portable addresses.

## Generic FFmpeg map

Resolve's generic FFmpeg factory looks up the stream FourCC in a private red-black tree. The stock tree has 71 nodes and no `av01` entry. The transaction adds `0x61763031 -> 226` using Resolve's node layout and rebalancer, preserving the existing tree.

The callback and generic FFmpeg factory are both relevant: changing only a callback registry entry would not cover all direct factory calls. The live transaction therefore patches the callback and the ten audited generic FFmpeg CALL sites as one validated operation.

## Verification boundary

The transaction has passed live commit/rollback verification in a disposable Resolve child. A persistent patched GUI session also reached actual AV1 send/receive calls and produced smooth playback, but the earlier teardown crash exposed a cross-allocator `extradata` ownership bug. That bug is corrected in the provider and covered by provider tests. A later run produced a separate SIGSEGV in Resolve-side string-copy code; its relationship to the provider remains unknown. A fresh persistent GUI session with the corrected crash-event capture must verify playback and normal close.
