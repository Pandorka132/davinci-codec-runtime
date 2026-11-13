# Resolve AV1 callback integration seam

## Scope

These findings apply only to the inspected DaVinci Resolve 21.1.1 executable with SHA-256 `d083cf9f1eb875bdb928db54b20425b4111b53a16cae72a1aef1c3eeb603a121`. They describe a candidate in-memory hook. No Resolve process was patched or launched for this investigation.

## Callback contract

`FUN_05e3deb0` is the AV1 decoder factory wrapper at executable address `0x05e3deb0`.

- ABI: x86-64 System V, `long* callback(int operation, long* properties)`.
- `operation == 1` is the decoder path. Other operations return null.
- `properties` is forwarded unchanged to each backend factory.
- A successful result is a Resolve codec object pointer; the caller performs the expected `__dynamic_cast` to the common codec interface.
- The wrapper is registered in the decoder callback map for FourCC `av01` (`0x61763031`). The AV1 encoder callback is a separate registration and is not part of this hook.

The original entry bytes are:

```text
53 83 ff 01 75 28
```

They decode to `push rbx; cmp edi, 1; jne +0x28`. The wrapper body ends at `0x05e3deef`.

## Reachability

There are two routes into this same function entry:

1. The callback registry stores the address `0x05e3deb0`; the generic callback dispatcher invokes the stored function pointer.
2. `FUN_05de4000` contains a direct call at `0x05de409d` to `0x05e3deb0` in its explicit `av01` branch.

The static reference database contains exactly these two references to the callback: the registration argument at `0x05e33d90` and the direct call at `0x05de409d`. Hooking the function entry therefore covers both observed decode routes. Replacing only the callback-map entry would miss the direct call.

The stock encoder callback is registered separately for `av01` with the encoder flag. The proposed decoder hook does not modify that entry.

## Existing factory and setting behavior

All three decoder factories use the same `(1, properties)` shape:

| Factory | Address | Role |
|---|---:|---|
| NVIDIA | `0x05f2cb30` | Existing Resolve hardware path |
| Generic FFmpeg | `0x05eeef00` | Existing Resolve generic FFmpeg decoder object |
| AOM | `0x05f90830` | Existing Resolve internal AOM decoder |

The stock wrapper first calls `FUN_00ef4cf0()`, then calls `FUN_05e444f0` to read `Local.IO.SoftwareAV1Decode`. The first call must be preserved as part of the observed callback prelude. The callsite places its return value in `RDI` before calling the setting helper, but the helper's disassembly does not consume that argument. The helper accepts only the exact string `"1"` as true. It uses a C++ local-static guard and stores the parsed result in process-global state, so the setting is lazily read once and cached. The runtime must preserve the singleton prelude and call this existing helper rather than inventing a second config reader; it must not imply that edits to the setting after its first read will take effect.

The selected backend policy is:

- Setting is not `"1"`: NVIDIA, then the custom generic-FFmpeg-backed decoder, then AOM.
- Setting is `"1"`: the custom generic-FFmpeg-backed decoder, then AOM.

Fallback applies only when a factory returns null. Once a factory returns an object, later stream-open or decode failures are outside this factory-selection policy, matching the stock callback boundary.

## Candidate entry-hook shape

The least invasive complete interception point is the callback entry itself. The function is short, its entry contract is known, and no original instructions need to be executed by a trampoline if the replacement fully implements the factory policy.

Candidate patch layout:

1. Validate the exact executable fingerprint and the six original bytes above.
2. Reserve a small executable relay page within the signed 32-bit relative-branch range of `0x05e3deb0`.
3. Replace the first six bytes with a five-byte `E9 rel32` jump to that relay followed by one `NOP`.
4. The relay performs an absolute indirect jump to the runtime's callback replacement.
5. The replacement preserves the observed ABI, returns null for non-decode operations, reads the stock setting helper, then iterates the already-validated declarative backend plan and calls the existing factory addresses.

This layout avoids copying or relocating the original short conditional branch and its relative call. It also avoids changing the callback registry or the direct callsite: both continue to target the original entry, which now forwards to the replacement. If a near relay cannot be reserved, installation must fail closed; do not silently substitute a larger, less-audited prologue patch.

The hook must not call `FUN_05e3deb0` as a fallback, because that would recurse after the entry is patched. In normal mode, call NVIDIA first, then the custom generic FFmpeg factory, then AOM. In software mode, call the custom generic FFmpeg factory first, then AOM. AOM must never precede the custom decoder.

## Installation conditions still to prove

- The runtime is loaded after Resolve's ELF constructors and before any AV1 factory call.
- Use GDB all-stop mode and verify every inferior thread is stopped at the main breakpoint. Enable `scheduler-locking on` before the inferior `dlopen` call and keep it enabled while the runtime constructor performs the map insertion and callback patch. The breakpoint alone is insufficient: a synthetic test showed that with scheduler-locking off, a pre-main worker continued running during a slow DSO constructor.
- The patch page's original permissions are recorded and restored exactly.
- The relay allocation and target are executable, the relative displacement is in range, and the final bytes are read back and verified.
- A failed write or verification restores the original six bytes before execution resumes.
- The exact binary fingerprint, function bytes, factory signatures, and relevant map layout all match; otherwise initialization aborts without patching.

A synthetic GDB probe starts a pthread worker from an ELF constructor and stops at main. Both threads report stopped at the breakpoint. During a deliberately slow DSO constructor, the worker counter remains unchanged with `scheduler-locking on`; with it off, the counter advances from 1 to 192. Nested `dlmopen` from the loaded DSO also succeeds. The actual Resolve executable was also launched as a GDB child and stopped at `0x00ec0c70`: one thread was present, all threads were stopped, the live decoder map contained 71 nodes with a black root, black-height 6, valid parent/begin links and no `av01` entry, and the AV1 callback bytes matched the inspected prologue. The `_INIT_1865` property helper was hit 27 times, but its guarded converter, the DbSaveHandler singleton, constructor, and worker-creation helper were not entered. The earlier static callgraph had over-approximated a conditional error path. No DSO was loaded into Resolve, so the live installation sequence remains untested.

## Remaining dependency

The generic FFmpeg factory currently rejects `av01` because Resolve's private FourCC-to-`AVCodecID` tree lacks the `av01 -> 226` entry. A structural probe now supports a smaller candidate than replacing the entire 71-node tree: insert one 0x28-byte node into the existing tree and call Resolve's `FUN_00ecdba0` red-black rebalancer. This preserves the existing root and avoids old-tree reclamation, but still requires a proven all-reader quiescence window and ABI validation for Resolve's allocator/rebalancer calls. The probe models the structures; it does not call Resolve's functions or touch live memory.

No code in this project currently installs this hook. No Resolve binary, library, process memory, or original Ghidra project has been modified.
