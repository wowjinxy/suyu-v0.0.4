# suyu static recompiler CLI

This directory contains the low-level command-line frontend for suyu's experimental
AArch64-to-C recompiler. It can inspect a plaintext NSO0 module and emit a portable CMake project
from a raw, little-endian AArch64 text segment. It shares the NSO parser and block/emission code
with the in-app exporter under `src/core/recompiler`.

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

LZ4 is optional. If CMake can find an `lz4::lz4` package target, `inspect-nso` also decodes
compressed segments. Without it, header inspection and uncompressed NSO decoding still work and
compressed input is reported as unavailable rather than misread. For example, a vcpkg install can
be exposed with `-DCMAKE_PREFIX_PATH=<vcpkg-installed-triplet>`.

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

## Inspect a plaintext NSO

```sh
build/static_recompiler/suyu_recomp inspect-nso \
  --input path/to/main \
  --json
```

Inspection reports checked file and memory ranges, build ID, segment compression and expected
hashes. NSO0 does not encode whether its instructions are AArch32 or AArch64, so architecture is
reported as unknown. `--assume-aarch64` explicitly opts into entry-point and block discovery for
experimentation; the future `emit-nso` command will instead use `main.npdm` by default.

Required-hash flags are surfaced, but SHA-256 verification is not wired into this first slice;
JSON therefore reports `required_hashes_verified: false` when verification was requested. Do not
treat successful decompression alone as an integrity check.

`inspect-nso` consumes already-decrypted module bytes. It neither accepts nor manages console
keys, NSP/XCI/NCA containers, updates, or mods.

## Test

```sh
ctest --test-dir build/static_recompiler --output-on-failure
```

The tests validate malformed NSO ranges, allocation limits, MOD0 probing, optional LZ4 behavior,
JSON inspection, and raw emission. The raw smoke test recompiles the repository's four-instruction
AArch64 fixture, builds the emitted C project, and verifies that it computes `5 + 7 = 12`. It
passes `--no-save` to the generated runner so a test does not create a 256 MiB standalone autosave.

Only use executable content that you are legally allowed to analyze. Do not commit console keys,
game dumps, extracted assets, or generated code derived from proprietary titles.
