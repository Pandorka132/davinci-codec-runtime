# AV1 context teardown: extradata allocator ownership

## Symptom

A persistent patched Resolve session decoded AV1 frames successfully and playback was smooth, but Resolve later aborted while freeing the AV1 `AVCodecContext`.

The crash stack reached:

```text
libavcodec.so.60(avcodec_free_context+0x2a)
libdavinci_codec_runtime.so(davinci_avcodec_free_context+0x153)
```

The GDB source location resolved to FFmpeg `libavcodec/options.c:177`:

```c
av_freep(&avctx->extradata);
```

## Root cause

The Resolve generic FFmpeg factory allocates the context through the custom provider, then allocates `AVCodecContext::extradata` with Resolve's own `av_mallocz` and stores it in the context. The context is later destroyed through the private FFmpeg 6.1.6 `avcodec_free_context`, which releases `extradata` with private libavutil's `av_freep`.

The pointer therefore crossed allocator ownership domains. The allocator mismatch explains why decoding succeeded and the crash occurred only during context teardown.

The earlier `subtitle_header` suspicion was incorrect. The relevant field is `extradata` at the public FFmpeg context offset used by the factory.

## Correction

Before private `avcodec_open2`:

1. Allocate a padded copy of the payload using private libavutil's `av_mallocz`.
2. Copy the exact `extradata_size` payload bytes; the new padding remains zeroed.
3. Release the original Resolve-owned pointer with Resolve's `av_freep`.
4. Publish the private-owned copy in `AVCodecContext::extradata` and track that pointer in the context registry.

For a context that is freed before open, the provider releases its still-foreign extradata with Resolve's `av_freep` and clears the field before private `avcodec_free_context` runs.

No free operation is skipped, and no memory leak is used to hide the failure.

## Regression coverage

`ffmpeg_provider_test` allocates test extradata using Resolve's `av_mallocz`. It checks that:

- freeing an unopened context releases the foreign pointer through Resolve's allocator;
- opening a context replaces the foreign pointer with a private allocation;
- payload bytes survive the transfer and the FFmpeg padding is zero;
- context free completes through the private provider.

The provider tests pass. A fresh full Resolve playback-and-close session is still required to confirm the live teardown path.
