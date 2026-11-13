# Codec definitions

## Purpose

Codec-specific facts belong in declarative definitions. Registration, lookup, and backend dispatch consume those definitions through shared code. Adding a codec must not require another codec-specific branch in the runtime.

## Definition model

The initial model has five parts:

| Field | Meaning |
|---|---|
| `id` | Stable internal identifier, independent of FourCC spelling and backend name |
| `fourcc_aliases` | Every Resolve/container FourCC accepted for this codec |
| `ffmpeg_codec_id` | FFmpeg codec ID used by the generic FFmpeg backend; absent when that backend is unsupported |
| `decoder` | Optional ordered decoder backend plan |
| `encoder` | Optional ordered encoder backend plan |

Backend implementations are referenced by a provider ID. They are not embedded in the codec definition. The definition must not contain Resolve addresses, vtable offsets, FFmpeg function pointers, or object layouts.

## FourCC representation

FourCC values are stored as a 32-bit integer in character order, for example `av01` = `0x61763031`. Conversion from four characters must be centralized and constexpr-capable. Definitions and their backing alias/candidate arrays must outlive every registry lookup; the initial runtime declarations will have static storage duration. Do not rely on host endianness or reinterpret a four-byte string as an integer.

Aliases are a list, not a single field. The registry rejects duplicate aliases across definitions and within a definition unless an explicit future alias-resolution rule is introduced. FourCC lookup is only valid after the complete definition set has passed validation.

## Backend policy

The policy is per operation, not one global backend list. Provider and condition identifiers are resolved against separate runtime registries. Each provider declares its operation (`decode` or `encode`) and ownership (`resolve_owned` or `runtime_owned`). A candidate contains:

- backend provider ID;
- activation condition (unconditional or a named runtime condition);
- failure behavior (continue to next candidate or stop);

This keeps backend ordering data-driven while allowing existing Resolve behavior to be represented. A codec definition has an optional decoder plan and an optional encoder plan; at least one must be present. A provider returning no codec object is a candidate failure; it must not be confused with a successful decoder that later fails to open a stream.

### AV1 initial declaration status

Known AV1 metadata:

- internal ID: `av1`;
- FourCC: `av01` (`0x61763031`);
- FFmpeg codec ID: `AV_CODEC_ID_AV1` (`226`);
- initial operation: decode only.

Known existing Resolve routes and the selected extension policy:

The callback plan uses these candidates in one shared order:

1. `resolve.nvidia.decoder` — attempted only when `Local.IO.SoftwareAV1Decode` is not `"1"`.
2. `resolve.generic_ffmpeg.decoder` — the project's custom accelerated AV1 software path. It is attempted immediately after NVIDIA, or first when the software setting skips NVIDIA. It uses Resolve's generic FFmpeg decoder factory, with `av01 -> AV_CODEC_ID_AV1 (226)` added to its lookup map; the FFmpeg calls are served by the runtime's isolated FFmpeg 6.1.6 provider.
3. `resolve.aom.av1.decoder` — fallback only when the custom decoder factory returns no decoder object.

The goal is to accelerate AV1 decoding by giving the custom decoder the first software opportunity; AOM is strictly the fallback, not a preferred software route. With `Local.IO.SoftwareAV1Decode="1"`, NVIDIA is skipped and the order is custom FFmpeg, then AOM. With any other value, the order is NVIDIA, custom FFmpeg, then AOM. This ordering is essential: if AOM is tried first and successfully creates a decoder, Resolve will never ask the custom decoder to load. The single named condition is the inverse of the exact-`"1"` setting check used by Resolve.

Fallback is limited to factory creation: a provider returning no codec object is unavailable and the next candidate may be tried. Once a provider returns a decoder object, later stream initialization or decode errors are not automatically retried through a different backend. That distinction matches the factory boundary visible in Resolve's callback API. The runtime must not snapshot either condition when its library is loaded. It must query the existing Resolve setting helper when dispatching. That helper itself lazily reads `Local.IO.SoftwareAV1Decode` once and caches the exact-`"1"` result for the process lifetime, matching the stock callback behavior; changing the setting after its first read is not expected to change the selected path.

## Registration consumers

The same validated definition set is the source for:

1. FourCC-to-definition lookup;
2. Resolve decoder/encoder callback registration;
3. Resolve generic FFmpeg FourCC-to-`AVCodecID` map additions;
4. operation/backend dispatch tables.

These consumers may have different lifecycle and synchronization requirements. They must not be updated as one partially committed operation: validate the full definition set and all target registries first, then apply registration at a proven safe initialization point.

Descriptor/Deliver UI registration is intentionally outside this first model. Resolve's AV1 profile-family classifier has separate behavior and is not solved by registering a decoder backend.

## Resolve integration seam (static findings)

- `FUN_05e3deb0` is the AV1 decoder callback wrapper. Resolve registers it in the callback map, but `IOTemporalCodecContainer::InitCodecInternal` also calls it directly from the `av01` branch in `FUN_05de4000`. Replacing only the callback-map entry would miss the direct call. The integration hook must cover the wrapper entry.
- The candidate factories are `FUN_05f2cb30(1, properties)` for NVIDIA, `FUN_05eeef00(1, properties)` for the generic FFmpeg decoder, and `FUN_05f90830(1, properties)` for internal AOM.
- `FUN_05eeef00` and its constructor both look up the FourCC in Resolve's private generic-decoder map. The current 71-entry map has no `av01` entry; the required mapping is `0x61763031 -> 226`.
- `FUN_05c87340` is a bulk map initializer, not a callable one-entry insertion API. Its machine code does expose the insertion sequence: search, allocate a 0x28-byte node with Resolve's `operator new`, initialize its links/key/value, link it, update the begin pointer when required, call `FUN_00ecdba0(root, new_node)` for red-black rebalancing, then increment size. A structural probe validated a single `av01 -> 226` insertion into the existing 71-entry tree. The candidate runtime implementation would reproduce that one-node insertion and reuse Resolve's rebalancer; it would not rebuild/swap the live tree. The probe does not call Resolve's allocator or rebalancer, and the live mutation is not implemented.

These are single-build Resolve 21.1.1 findings. They are not portable addresses or proof of safe live patching. The original wrapper, map, Resolve process, and executable remain untouched.

## Initialization and map update constraints

The generic FFmpeg FourCC tree is built by Resolve's `_INIT_3830` ELF constructor before `main()`. The initializer registers `0x05c874a0` as the process-exit destructor for the global map. The static call database shows the map readers behind normal decoder creation: the generic factory `FUN_05eeef00` is called by the temporal-container codec path and two existing decoder wrappers; its constructor `FUN_05ef0890` is called by the factory. No direct reader call from `_INIT_3830` was found.

The inspected binary's main entry is `0x00ec0c70`. This is evidence for one build only, not a constant suitable for the runtime. Its `.init_array` has 4380 entries; `_INIT_3830` builds the FFmpeg map. `_INIT_1865` calls a property-descriptor helper 27 times, but a dedicated GDB trace observed no entry into its guarded converter branch, no `DbObj::DbSaveHandler` singleton construction, and no worker-creation helper call. The earlier static callgraph had over-approximated a conditional error path. At `main`, the live map contained 71 valid nodes, had a black root and black-height 6, correct parent/begin links, no `av01` key, and the callback prologue matched the expected bytes. Static direct-call analysis found no initializer-to-FFmpeg-map-reader path. A synthetic GDB test confirms that `scheduler-locking on` prevents a pre-main worker from progressing during an inferior `dlopen`; no DSO was loaded into the actual Resolve process.

The custom FFmpeg 6.1.6 pair has a strict load order: load its `libavutil.so.58` first into `LM_ID_NEWLM`, query the resulting namespace ID with `dlinfo`, then load `libavcodec.so.60` into that same namespace. `libavcodec` has no `$ORIGIN` RUNPATH; loading it by itself cannot resolve the project-local `libavutil.so.58`. The isolated dispatcher harness uses this exact order. Its dependency set is 12 ELF objects on the inspected host: standalone `ldd` resolves 11, and the custom `libavutil.so.58` is loaded explicitly first into that namespace. Static loader-constructor inspection found only CRT registration/CPU-dispatch setup at dependency initialization; codec libraries still contain thread-capable code that is used later during decoder or encoder setup.

The preferred map-update candidate is a single-node insertion using Resolve's allocator and red-black rebalancer. It must validate the executable fingerprint, map/sentinel/node layout, initial 71-entry size, absence of the AV1 key, and exact helper targets before touching memory. Allocation and all node fields must be completed before linking; allocation failure must leave the map unchanged. All readers must be stopped during the insertion because no public map lock has been found. If any validation fails, initialization must abort without changing Resolve memory. The earlier full-tree rebuild/swap remains documented as a less attractive alternative, not the current preferred design.

The installed Resolve C++ runtimes resolve to this machine's system `libc++.so.1` and `libc++abi.so.1`. The default build therefore uses Clang/libc++; a separate-library exception probe confirmed that a libc++abi exception can be caught across a DSO boundary using this runtime. This does not yet test Resolve's own `operator new` exception path.

## Validation rules

- Internal IDs are non-empty and unique.
- Every definition has at least one FourCC alias.
- FourCC aliases are unique across the complete registry.
- An FFmpeg backend requires an FFmpeg codec ID.
- A definition cannot advertise decode or encode without at least one candidate for that operation.
- Backend providers and named activation conditions must be registered and unique; unknown values fail closed. A provider must support the operation of the plan that references it.
- Every codec definition must expose at least one operation, and an explicit FFmpeg codec ID must be positive.
- Existing Resolve-owned routes must be represented explicitly in the codec policy; changing that set is an intentional policy change.
- Factory dispatch skips candidates whose named condition evaluates false, and only falls back on an explicit unavailable result.
- No live registry mutation occurs if validation fails.

## First implementation boundary

The initial executable implementation covers the definition model, structural/provider/condition validation, deterministic lookup, and the generic provider-plan dispatcher. The AV1 policy is unit-tested with simulated factory results; no Resolve provider is called yet. It should be testable without loading Resolve or FFmpeg. Resolve callback hooks, map mutation, provider loading, and live dispatch remain separate integration stages.
