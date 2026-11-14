# Validation matrix

| Layer | What it proves | What it does not prove | Current status |
|---|---|---|---|
| Codec-definition/policy tests | FourCC, codec ID, and backend ordering | Resolve integration | Passing |
| Provider tests | Private FFmpeg coexistence with Resolve FFmpeg, QSV and libdav1d 8/10-bit decode, NV12/P010 transfer, software-only selection, QSV-init-failure fallback, lifecycle, allocator ownership, concurrency | Callback/map patch or GUI playback | Passing 8-bit and 10-bit; QSV and software paths both exercised |
| Transaction unit tests | Snapshot validation, patch order, rollback, injected failures | Correct behavior in a live Resolve process | Passing |
| Live transaction probe | Real Resolve map/CALL/callback commit and byte-exact rollback | Long-running GUI playback | 74 checks passed in a disposable child |
| Persistent GUI playback | Actual Resolve callback and provider path, visible playback | Safe teardown unless Resolve closes normally | Earlier software-provider 4K playback observed; QSV-enabled GUI playback not yet tested; separate Resolve-side memcpy/string-copy SIGSEGV remains unlocalized |
| GUI teardown | Normal stop, close, and process exit after patched playback | Other Resolve builds | Pending: verify QSV path, extradata fix, and investigate separate Resolve-side SIGSEGV |

When reporting results, name the layer. Avoid saying simply "AV1 works" when only a provider or transaction test passed.
