# Reverse-engineering and implementation notes

This file records the findings needed to review or extend the runtime without shipping a multi-gigabyte Ghidra database.

## Target and callback

- Target: DaVinci Resolve 21.1.1.0010, Linux x86-64, Clang build.
- The installer pins the Resolve executable SHA-256 and validates the actual mapped image before patching.
- AV1 callback: `FUN_05e3deb0` (`0x05e3deb0`); NVIDIA factory: `FUN_05f2cb30`; generic FFmpeg factory: `FUN_05eeef00`; AOM factory: `FUN_05f90830`.
- The generic FFmpeg FourCC tree is `DAT_29537cf0`, with a 0x18-byte header and 0x28-byte nodes. The stock tree has 71 nodes. The AV1 entry is `0x61763031 -> 226`.
- Resolve's red-black rebalancer is `FUN_00ecdba0` (`0x00ecdba0`), called with `rdi=root`, `rsi=new_node`; it returns void and is non-throwing in the audited build.

## Patch transaction

- Ten audited FFmpeg CALL sites are redirected through runtime wrappers.
- The callback entry uses an aligned 8-byte atomic detour to a relay page. The relay contains wrapper jump stubs and pointer cells.
- The map node is prepared with Resolve's allocator before the all-stop phase. The tree is updated only after full preflight validation.
- The callback detour is published last. Instruction streams are synchronized and the original page protections are restored.
- Rollback restores the callback bytes, ten CALL instructions, map header/nodes, relay mapping, and protections. Synthetic failure injection covers twelve rollback points plus allocation and digest failures.

## All-stop and allocator constraints

Resolve's TBB allocator can block on internal locks (`masterMutex`, `startupMallocLock`, `rmc_mutex`, `memPoolListLock`, and `initMutex`). A process-wide all-stop could suspend a thread while it owns one of these locks. Therefore no TBB/Resolve allocator call is allowed while the target is stopped. The map node is allocated before the all-stop window; any deferred free occurs after resume.

The installer validates target mapping identity using `statx(AT_EMPTY_PATH)` mount IDs and `/proc/self/mountinfo`, rather than relying on inode-only identity. It checks code mapping identity/protection and the full 71-node tree snapshot before executing the transaction.

## Private FFmpeg loading and QSV

The runtime loads private FFmpeg 6.1.6 into the base linker namespace with `RTLD_LOCAL | RTLD_DEEPBIND`. The private `libavutil.so.58` and `libavcodec.so.60` have unique `libdavinci_*` SONAMEs, and private libavcodec has an `$ORIGIN` RUNPATH to its private util dependency. This arrangement is required because the Intel oneVPL/VAAPI driver plugin loader failed under `dlmopen(LM_ID_NEWLM)`.

At provider initialization, the runtime scans DRM render nodes and accepts QSV only when exactly one accessible Intel PCI vendor `0x8086` node is found. It creates one shared QSV device context and selects FFmpeg `av1_qsv`; otherwise AV1 selects `libdav1d`. Resolve's exact `Local.IO.SoftwareAV1Decode="1"` setting forces the libdav1d decoder and bypasses QSV. QSV hardware frames are transferred to private system-memory frames before the existing Resolve-owned frame copy. Non-AV1 contexts continue to be forwarded to Resolve's FFmpeg implementation.

## Crash root cause: extradata allocator ownership

The first GUI playback session produced successful `avcodec_send_packet` and `avcodec_receive_frame` results and smooth playback, then aborted during `avcodec_free_context`. The GDB stack identified `libavcodec/options.c:177`, `av_freep(&avctx->extradata)`. Resolve's generic factory allocates that `extradata` with Resolve's `av_mallocz`; the private FFmpeg context free path tried to release it with private libavutil. This was an allocator-ownership mismatch, not a `subtitle_header` problem.

The provider now copies the Resolve-owned extradata into private libavutil memory before private `avcodec_open2`, then releases the original through Resolve's `av_freep`. Unopened contexts also release foreign extradata through the Resolve allocator. Tests cover both paths.

## Subsequent GUI SIGSEGV: Resolve-side string copy remains unresolved

A later GUI run, after the extradata ownership correction, stopped with SIGSEGV in glibc's vectorized `memcpy`. The return address was Resolve `0x3b533be`, with its caller at `0x3b50e11`. At the fault, the source register contained `0xf90f12cc795fcb8b` and the copy length register contained `0x6ca1edc0`. Disassembly shows the Resolve helper loading a source pointer from an object before calling `memcpy`; the pointer/length pair is invalid. This is a separate observed crash from the earlier `avcodec_free_context` SIGABRT. Its connection, if any, to the AV1 provider has not been established.

The first SIGSEGV log did not preserve the intended recent provider events: the diagnostic code read the tail of a file after appending large memory-map dumps, so the purported event section mostly repeated mappings. The GDB script now snapshots a bounded in-memory provider-event ring before collecting diagnostics and additionally captures the suspected string destination object, caller frame, and caller stack. This instrumentation has only passed syntax/AST checks; it has not yet been exercised on another live Resolve crash.

## Evidence and limitations

- Provider-level 8-bit and 10-bit IVF tests each decode six packets into six frames.
- A prior live transaction probe passed 74 checks for commit, map insertion, ten CALL redirects, callback detour, relay protection, rollback, and byte-exact restoration.
- A persistent Resolve session reached actual send/receive calls. A 4K playback session was reported smooth, but that session ended with the allocator bug described above.
- The extradata allocator correction passes provider tests, but has not yet been revalidated in a fresh GUI playback/close session; a subsequent run hit the separate SIGSEGV documented above.
- The exploratory Ghidra database, large analysis logs, and unrelated sample corpora are intentionally excluded. The official FFmpeg source archive is fetched by the build script and is not committed.
