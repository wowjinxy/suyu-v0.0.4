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
- `suyu/game_export.cpp` already extracts application modules and generates AOT projects, but its
  loader/analyzer is tied to Qt and should become a shared non-UI library.
- `tools/static_recompiler` provides a deterministic low-level `emit-raw` CLI and an end-to-end
  native smoke test. It deliberately does not claim container or NSO support yet.

## Milestones

### M0: canonical emitter CLI

- One frontend for the canonical emitter; no duplicated decoder/runtime.
- Strict input/address/module validation and checked output writes.
- Cross-platform generation/build/run smoke test with no proprietary inputs.

### M1: shared executable analysis

- Move NPDM/NSO parsing, decompression, module layout, build-ID extraction, exported-symbol roots,
  relocation roots, and code-pointer scanning out of the Qt exporter.
- Add `inspect` and `emit-nso` commands using that shared library.
- Reject AArch32 modules explicitly until a separate frontend exists.
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
