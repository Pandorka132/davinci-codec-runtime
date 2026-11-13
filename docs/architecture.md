# Runtime architecture

## First milestone: AV1 decode

Preserve Resolve's NVIDIA hardware route and internal AOM fallback, while inserting our custom accelerated AV1 decoder before AOM. The fixed project goal and backend priority are recorded in [project-goal.md](project-goal.md).

The runtime must keep codec metadata separate from backend implementation. See [codec definitions](codec-definitions.md) for the declarative schema, validation rules, and AV1 backend policy. Callback and FFmpeg-map registration will consume that definition rather than contain codec-specific branches. The AV1 goal is decode acceleration: normal mode tries NVIDIA, then the custom generic-FFmpeg-backed decoder, then AOM as fallback; when `SoftwareAV1Decode="1"`, NVIDIA is skipped and the order is custom decoder then AOM. Never put AOM ahead of the custom decoder.

The observed Resolve 21.1.1 callback contract, direct-call/reference evidence, and candidate entry-hook shape are documented in [Resolve AV1 callback integration](resolve-av1-callback.md). This is a static integration design, not an implemented or tested live hook.

## Registration model

- One decoder dispatcher and one encoder dispatcher.
- Per-codec definitions supply FourCC aliases and backend policy.
- Existing Resolve callbacks are preserved when extending an already-supported codec.
- FourCC aliases and FFmpeg codec IDs come from the same validated codec definition set.
- Generic FFmpeg calls are routed by context ownership, never globally.
- Descriptor and Deliver UI registration are separate from decoder/encoder backend registration.

## Initialization and ABI constraints

- Resolve initializes the generic decoder FourCC tree in ELF constructor `_INIT_3830`, before `main()`. The initial tree contains 71 entries and has a registered process-exit destructor.
- The inspected binary has 4380 `.init_array` entries. `_INIT_3830` builds the 71-entry FFmpeg map at slot 3830. `_INIT_1865` at slot 1865 calls a property-descriptor helper 27 times. A GDB trace observed all 27 calls but none of the guarded converter branch, the `DbObj::DbSaveHandler` singleton, or its worker-creation helper; the earlier static callgraph had over-approximated a conditional error path. At `main`, only the main thread existed at `0x00ec0c70`. The main address is build-specific. A synthetic GDB test proves all-stop mode stops a pre-main worker; `scheduler-locking on` is required during inferior `dlopen` calls to keep it stopped while DSO constructors execute.
- The generic decoder map is read by `FUN_05eeef00` and again by `FUN_05ef0890`. The static call database places those readers behind normal codec-creation paths, not in `_INIT_3830`.
- Any map mutation must occur after stock initialization and while all potential readers are quiesced. In the actual GDB observation, `main` was reached with one stopped thread; the live map was validated as 71 nodes with a black root, black-height 6, correct parent/begin links, and no `av01`, while the callback prologue matched the expected bytes. The candidate installation sequence remains a fingerprint-checked GDB child launch, main breakpoint, thread enumeration, `scheduler-locking on`, then inferior `dlopen`. DSO loading and runtime mutation have not been performed in Resolve. The preferred map mutation is a single-node insertion using Resolve's allocator and red-black rebalancer, but it is only structurally simulated.
- Resolve's executable loads `libc++.so.1` and `libc++abi.so.1` from `/opt/resolve/libs`; both symlink to the system libc++ runtime on this machine. The default runtime build therefore uses Clang with `-stdlib=libc++`, so C++ allocation exceptions can be caught using the same ABI. A cross-DSO libc++ exception probe passed; Resolve's own allocator has not yet been invoked from the runtime.
- The custom FFmpeg 6.1.6 `libavcodec.so.60` has no `$ORIGIN` RUNPATH and needs `libavutil.so.58`. Load the custom `libavutil` first with `dlmopen(LM_ID_NEWLM, ...)`, obtain that namespace with `dlinfo(RTLD_DI_LMID)`, then load the matching custom `libavcodec` into the same namespace. A standalone `ldd` of `libavcodec` reports `libavutil.so.58` missing, which is expected until the first explicit load. The isolated harness already uses this order.
- The current custom `libavcodec` dependency set contains 12 ELF objects on this host (11 returned by standalone `ldd`, plus the explicitly preloaded custom `libavutil.so.58`), including dav1d, AOM, rav1e, SVT-AV1 and their runtime dependencies. Static inspection of their loader constructors found no thread creation in the constructor entrypoints; liblzma additionally performs CPU-feature dispatch setup. The libraries still contain thread-capable code that can run later when codecs are opened. This is a static audit of the current host library versions, not a claim about other versions.

## Safety requirements

- Resolve build identity and every patch site must be validated before use.
- Ambiguous or changed signatures abort initialization.
- Registry changes happen before concurrent codec use, or under a proven synchronization strategy.
- Every backend owns and releases its own FFmpeg contexts and frames according to the tested ABI boundary.
- No on-disk Resolve binary or library modification.

## Implementation status

The Clang/libc++ buildable shared-library skeleton, generic codec-definition validation/lookup, generic backend-plan dispatcher, and first AV1 declaration exist. The dispatcher is tested with simulated providers only. Resolve callback routing, registry registration, the runtime condition reader, FFmpeg namespace loading, provider calls, and Resolve injection are not implemented.
