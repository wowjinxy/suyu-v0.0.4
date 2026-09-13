# suyu static recompiler CLI

This directory contains the low-level command-line frontend for suyu's experimental
AArch64-to-C recompiler. It emits a portable CMake project from a raw, little-endian AArch64
text segment. It uses the same implementation as the in-app exporter:
`src/core/recompiler/arm64_to_c.h`.

This is an engineering tool, not a finished one-click game converter. The generated standalone
runtime has stub services. Switch software that uses Horizon services, the GPU, audio, input, or
dynamic modules must run through suyu's hosted `ArmRecomp` backend, which can fall back to
Dynarmic for uncovered code.

See [the static recompiler roadmap](../../docs/StaticRecompiler.md) for the hosted-runtime
architecture and milestones.

## Build

Use a 64-bit C/C++ toolchain:

```sh
cmake -S tools/static_recompiler -B build/static_recompiler -G Ninja
cmake --build build/static_recompiler
```

On Windows, select an x64 MSVC environment or pass x64 Clang explicitly. The checked-in `.exe`
and `.obj` files in this legacy directory are not used by the CMake build; rebuild from source.

## Emit a project

```sh
build/static_recompiler/suyu_recomp emit-raw \
  --input path/to/text.bin \
  --base 0x1000 \
  --entry 0x1000 \
  --module main \
  --output build/generated-main
```

Run `suyu_recomp --help` for all options. The older positional form remains accepted for
compatibility.

The tool is also available from the full suyu build with `-DSUYU_RECOMPILER_CLI=ON`.

## Test

```sh
ctest --test-dir build/static_recompiler --output-on-failure
```

The smoke test recompiles the repository's four-instruction AArch64 fixture, builds the emitted
C project, and verifies that it computes `5 + 7 = 12`. It passes `--no-save` to the generated
runner so a test does not create a 256 MiB standalone autosave.

Only use executable content that you are legally allowed to analyze. Do not commit console keys,
game dumps, extracted assets, or generated code derived from proprietary titles.
