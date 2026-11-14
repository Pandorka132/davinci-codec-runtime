# Contributing

Thanks for helping improve the Resolve codec runtime. The project is build-specific and touches a live process, so changes need evidence and regression coverage.

## Before opening a PR

1. Read [the architecture](docs/architecture.md), [the project goal](docs/project-goal.md), and [the testing notes](docs/testing.md).
2. Build with `./scripts/build.sh` and run `ctest --test-dir build/clang-libcxx --output-on-failure`.
3. Keep changes scoped to the relevant codec/provider/installer boundary. Explain why the selected integration point is safer than a deeper render-pipeline hook.
4. Include the exact Resolve build fingerprint and any new function/structure assumptions. Resolve offsets are not portable across releases.
5. Add a regression test for allocation, open, decode, flush, close, free, failure, and concurrency paths affected by the change.

## Non-negotiable invariants

- Do not move AOM ahead of the custom AV1 route. In normal mode NVIDIA remains first; in software-only mode NVIDIA is skipped; AOM is fallback only when the preceding factory returns no object.
- Do not turn an open/decode error into an automatic factory fallback.
- Do not modify the installed Resolve executable or bundled libraries. The supported integration is an in-memory transaction.
- Fail closed if the target fingerprint, mapping identity, ABI, instruction bytes, allocator target, or transaction preconditions differ.
- Do not allocate or free through Resolve/TBB while the target is in the all-stop window. Preallocate before the stop and defer any release until after resume.
- Every allocation must be released by the allocator that owns it. Treat Resolve and private-FFmpeg namespaces as separate ownership domains even when the symbol names and major ABI versions match.
- Do not add silent leaks, free-skips, or unrelated workarounds to mask teardown failures.

## Tests and reports

- Keep fixtures small and purpose-built. The included AV1 IVF samples are intentionally tiny.
- Keep benchmark summaries reproducible: record decoder/library versions, sample dimensions, bit depth, chroma format, thread count, warm-up count, and measured runs.
- Distinguish provider tests, live transaction tests, and GUI playback tests. Passing one layer does not imply the others pass.
- Do not commit Ghidra databases, process dumps, multi-gigabyte datasets, generated build trees, or machine-specific runtime logs. Summarize the useful findings in `docs/` instead.
- Do not add machine-specific absolute paths to scripts. Use paths relative to this repository or explicit configuration.
- Never commit `.env` files, credentials, or local authorization material.

## PR description checklist

- [ ] What Resolve behavior changes, and why?
- [ ] Which exact Resolve build was tested?
- [ ] Which allocator owns every new/modified allocation?
- [ ] What happens on factory-null, open failure, decode failure, close, and free?
- [ ] Are non-AV1 codec calls still forwarded to Resolve's original FFmpeg?
- [ ] Which unit/provider/transaction/GUI tests were run?
- [ ] Are any limitations or unverified paths stated explicitly?

## Licensing

The configured FFmpeg shared-library build disables GPL/nonfree components and reports LGPL-2.1-or-later. The source archive contains upstream license notices for both LGPL and GPL components; retain them when redistributing it. The project's own license has not yet been selected; choose and add one before publishing a public release so contributors know the terms under which their contributions are accepted.
