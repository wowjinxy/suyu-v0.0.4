# Static recompiler roadmap

## Product direction

Build a title-specialized, static-first Switch runtime:

```text
lawfully supplied title image
  -> format/architecture probe
  -> NSO module analysis and build-ID verification
  -> ahead-of-time native blocks
  -> suyu kernel/HLE/GPU/audio/input/filesystem runtime
  -> Dynarmic fallback for uncovered or dynamic code
```

Pure whole-program static recompilation is a per-title optimization goal, not an MVP requirement.
Indirect calls, dynamically loaded NROs, runtime-generated code, atomics, and missing instruction
semantics make a correctness-preserving fallback necessary.

The GPU command stream is not CPU code and should continue through suyu's existing GPU engines,
rasterizer, and runtime shader translation.

## Current foundation

- `core/recompiler/arm64_to_c.h` discovers AArch64 blocks and emits portable C.
- `core/arm/recomp/arm_recomp.cpp` implements the normal CPU interface and bridges generated code
  to suyu memory, SVC dispatch, and a lazy Dynarmic fallback.
- `core/recompiler/nso_image.{h,cpp}` now provides checked, non-Qt NSO0 inspection and segment
  decoding to both the command-line tool and in-app exporter.
- `core/recompiler/npdm_info.{h,cpp}` provides a dependency-light, structurally checked NPDM
  architecture probe; AArch32 is rejected before bytes reach the AArch64 decoder.
- `suyu/game_export.cpp` retains container/VFS work but now consumes the shared NSO decoder and
  canonical block discovery instead of maintaining a second parser and branch sweep.
- `tools/static_recompiler` provides deterministic `emit-raw`, `inspect-nso`, and `emit-nso`
  commands plus synthetic parser, JSON, LZ4, generation, native-build, and execution tests.
- Generated standalone projects require a 64-bit host, preserve page-aligned NSO segment virtual
  addresses, validate bundled file sizes, and explicitly map zeroed BSS instead of overlaying every
  segment at the text base.

## Milestones

### M0: canonical emitter CLI

- One frontend for the canonical emitter; no duplicated decoder/runtime.
- Strict input/address/module validation and checked output writes.
- Cross-platform generation/build/run smoke test with no proprietary inputs.

### M1: shared executable analysis

- Completed slice: checked NSO0 layout/build-ID/segment inspection, bounded decoding, optional LZ4,
  explicit ZBIC detection, unverified-hash signaling, structurally validated NPDM architecture
  probing, and architecture-honest human/JSON CLI output.
- Completed slice: `emit-nso` decodes the three segments, validates a conventional MOD0/AArch64
  entry, rejects AArch32, and emits an address-correct C project behind an NPDM or explicit
  assumption gate.
- Move exported-symbol roots, relocation roots, and code-pointer scanning out of the Qt exporter.
- Key output by build ID plus hashes of the post-update, post-mod decompressed segments.
- Keep decryption/key management outside the recompiler core.

### M2: correctness oracle and telemetry

- Differentially execute synthetic AArch64 blocks under AOT and vendored Dynarmic.
- Compare GPRs, vector registers, NZCV, FPCR/FPSR, TLS registers, memory writes, SVC exits, and
  exceptions.
- Record compile-time unsupported-opcode histograms and runtime fallback PCs/transitions.
- Report static execution ratio per module; prioritize semantics by measured frequency.

### M3: hosted hybrid proof

- Start with a small open-source AArch64 libnx program.
- Let guest `rtld` run under Dynarmic first; transition into compiled `main`/SDK blocks after the
  real loader has applied relocations.
- Preserve complete CPU state across every AOT/JIT transition.
- Add code-page invalidation and safe handling for dynamically loaded modules.
- Define success as deterministic boot, first frame, input, audio, save I/O, and useful coverage
  metrics—not merely successful C compilation.

### M4: scalable backend

- Use the vendored Dynarmic AArch64 frontend and optimized IR as the instruction-semantics source
  of truth.
- First implement an IR-to-C backend behind the existing `ArmRecomp` ABI.
- Then evaluate relocatable native object or LLVM emission to avoid multi-gigabyte C output and
  long compiler passes.
- Keep Dynarmic as the reference engine and fallback even as static coverage grows.

### M5: specialized runtime packaging

- Link verified per-module AOT objects into a per-title `suyu-cmd` runtime.
- Refuse module/build-ID mismatches instead of attempting to run stale output.
- Package runtime code and user configuration only; generated title code remains local.

## Immediate correctness work

1. Preserve FPCR/FPSR in `ArmRecomp::GetContext` and `SetContext`.
2. Add hosted AOT-vs-Dynarmic differential tests before expanding instruction coverage.
3. Replace heuristic runtime module discovery with loader-provided module descriptors.
4. Add coverage/fallback counters and first-failure diagnostics.
5. Fix or deliberately delegate exclusives/LSE atomics, indirect targets, TLS/relocations, and
   instruction-cache invalidation.

## Development and distribution boundary

Develop with synthetic fixtures and open homebrew. Accept only locally supplied inputs that the
user is entitled to analyze. Never commit or distribute console keys, firmware, game images,
extracted assets, instruction-containing traces, or generated code/binaries derived from
proprietary titles. Preserve suyu's GPL notices and audit the license/provenance of external work
before incorporating it.

Useful references include [N64Recomp](https://github.com/N64Recomp/N64Recomp) for the generated-C
and per-title runtime model, and the rapidly evolving
[mk8-recomp experiment](https://github.com/dougchansan/mk8-recomp) for recent work on this exact
suyu AOT/hybrid path. Audit and selectively integrate compatible changes instead of copying an
unreviewed fork wholesale.
