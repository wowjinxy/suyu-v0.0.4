// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/npdm_info.h"
#include "core/recompiler/nso_image.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Byte = std::uint8_t;

void PutU32(std::vector<Byte>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<Byte>(value);
    bytes[offset + 1] = static_cast<Byte>(value >> 8);
    bytes[offset + 2] = static_cast<Byte>(value >> 16);
    bytes[offset + 3] = static_cast<Byte>(value >> 24);
}

std::vector<Byte> MakeNso() {
    std::vector<Byte> bytes(0x120);
    std::copy_n(reinterpret_cast<const Byte*>("NSO0"), 4, bytes.begin());

    // Three adjacent, uncompressed segments. Deliberately put nonsense in the
    // compressed-size fields: the format says those fields are only authoritative
    // when the matching flag is set.
    PutU32(bytes, 0x10, 0x100);
    PutU32(bytes, 0x14, 0x1000);
    PutU32(bytes, 0x18, 0x10);
    PutU32(bytes, 0x20, 0x110);
    PutU32(bytes, 0x24, 0x2000);
    PutU32(bytes, 0x28, 0x08);
    PutU32(bytes, 0x30, 0x118);
    PutU32(bytes, 0x34, 0x3000);
    PutU32(bytes, 0x38, 0x08);
    PutU32(bytes, 0x3C, 0x20);
    PutU32(bytes, 0x60, 1);
    PutU32(bytes, 0x64, 2);
    PutU32(bytes, 0x68, 3);

    for (std::size_t i = 0; i < 0x20; ++i) {
        bytes[0x40 + i] = static_cast<Byte>(i);
    }
    const Byte text[] = {0xA0, 0x00, 0x80, 0xD2, 0xE1, 0x00, 0x80, 0xD2,
                         0x02, 0x00, 0x01, 0x8B, 0x01, 0x00, 0x00, 0xD4};
    std::copy(std::begin(text), std::end(text), bytes.begin() + 0x100);
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[0x110 + i] = static_cast<Byte>(0x40 + i);
        bytes[0x118 + i] = static_cast<Byte>(0x80 + i);
    }
    return bytes;
}

std::vector<Byte> MakeLz4Nso() {
    const std::vector<Byte> uncompressed = MakeNso();
    std::vector<Byte> bytes(0x122);
    std::copy_n(uncompressed.begin(), 0x100, bytes.begin());
    PutU32(bytes, 0x0C, 1); // LZ4-compressed text only
    PutU32(bytes, 0x60, 18);
    PutU32(bytes, 0x20, 0x112);
    PutU32(bytes, 0x30, 0x11A);

    // A valid raw LZ4 block containing one 16-byte literal run and no match.
    bytes[0x100] = 0xF0;
    bytes[0x101] = 1;
    std::copy_n(uncompressed.begin() + 0x100, 16, bytes.begin() + 0x102);
    std::copy_n(uncompressed.begin() + 0x110, 8, bytes.begin() + 0x112);
    std::copy_n(uncompressed.begin() + 0x118, 8, bytes.begin() + 0x11A);
    return bytes;
}

std::vector<Byte> MakeEmittableNso() {
    constexpr std::size_t TextSize = 0x68;
    std::vector<Byte> bytes(0x178);
    std::copy_n(reinterpret_cast<const Byte*>("NSO0"), 4, bytes.begin());
    PutU32(bytes, 0x10, 0x100);
    PutU32(bytes, 0x14, 0x1000);
    PutU32(bytes, 0x18, TextSize);
    PutU32(bytes, 0x20, 0x168);
    PutU32(bytes, 0x24, 0x3000);
    PutU32(bytes, 0x28, 8);
    PutU32(bytes, 0x30, 0x170);
    PutU32(bytes, 0x34, 0x5000);
    PutU32(bytes, 0x38, 8);
    PutU32(bytes, 0x3C, 0x10);
    for (std::size_t i = 0; i < 0x20; ++i) {
        bytes[0x40 + i] = static_cast<Byte>(0x80 + i);
    }

    // A conventional NSO entry stub and a minimum-sized MOD0 header.
    PutU32(bytes, 0x100, 0x14000010); // b text+0x40
    PutU32(bytes, 0x104, 8);
    std::copy_n(reinterpret_cast<const Byte*>("MOD0"), 4, bytes.begin() + 0x108);

    // Load 5 from rodata and 7 from data, add them, then prove that the zeroed
    // BSS tail is mapped by storing and reloading 9 at data+8. The standalone SVC
    // diagnostic prints x0-x3.
    constexpr std::array<std::uint32_t, 10> EntryInstructions{
        0xD0000000, // adrp x0, rodata@0x3000
        0xF9400000, // ldr  x0, [x0]
        0x90000021, // adrp x1, data@0x5000
        0xF9400021, // ldr  x1, [x1]
        0x8B010002, // add  x2, x0, x1
        0x90000023, // adrp x3, data@0x5000
        0xD2800124, // mov  x4, #9
        0xF9000464, // str  x4, [x3, #8] (BSS)
        0xF9400463, // ldr  x3, [x3, #8]
        0xD4000001, // svc  #0
    };
    for (std::size_t i = 0; i < EntryInstructions.size(); ++i) {
        PutU32(bytes, 0x140 + i * 4, EntryInstructions[i]);
    }
    bytes[0x168] = 5;
    bytes[0x170] = 7;
    return bytes;
}

std::vector<Byte> MakeNpdm(bool aarch64, std::uint8_t address_space = 3) {
    std::vector<Byte> bytes(0x300);
    std::copy_n(reinterpret_cast<const Byte*>("META"), 4, bytes.begin());
    bytes[0x0C] = static_cast<Byte>((aarch64 ? 1 : 0) | (address_space << 1));
    bytes[0x0E] = 0x2C;
    PutU32(bytes, 0x1C, 0x100000);
    PutU32(bytes, 0x70, 0x2C0);
    PutU32(bytes, 0x74, 0x40);
    PutU32(bytes, 0x78, 0x80);
    PutU32(bytes, 0x7C, 0x240);

    std::copy_n(reinterpret_cast<const Byte*>("ACID"), 4, bytes.begin() + 0x280);
    for (const std::size_t offset : {0x2A0, 0x2A8, 0x2B0}) {
        PutU32(bytes, offset, 0x240);
    }
    std::copy_n(reinterpret_cast<const Byte*>("ACI0"), 4, bytes.begin() + 0x2C0);
    for (const std::size_t offset : {0x2E0, 0x2E8, 0x2F0}) {
        PutU32(bytes, offset, 0x40);
    }
    return bytes;
}

bool IdentityDecompress(std::span<const Byte> source, std::span<Byte> destination) {
    if (source.size() != destination.size()) {
        return false;
    }
    std::copy(source.begin(), source.end(), destination.begin());
    return true;
}

int failures = 0;

void Check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void RunTests() {
    const std::vector<Byte> valid = MakeNso();
    const auto inspection = suyu::recomp::InspectNso(valid);
    Check(static_cast<bool>(inspection), "valid NSO inspects successfully");
    if (inspection) {
        const auto& info = *inspection.info;
        Check(suyu::recomp::ValidateNsoExecutableLayout(info).empty(),
              "valid NSO has an executable load layout");
        Check(info.segments[0].stored_size == 0x10,
              "uncompressed text uses decoded size rather than compressed-size "
              "field");
        Check(info.segments[1].stored_size == 0x08, "rodata stored size is decoded size");
        Check(info.bss_size == 0x20, "data extra word is exposed as BSS size");
        Check(suyu::recomp::NsoBuildIdToHex(info.build_id) ==
                  "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
              "build ID has stable lowercase hexadecimal formatting");
    }

    const auto decoded = suyu::recomp::DecodeNso(valid);
    Check(static_cast<bool>(decoded), "uncompressed NSO decodes without a callback");
    if (decoded) {
        Check(decoded.image->segments[0].size() == 0x10, "decoded text has expected size");
        Check(decoded.image->segments[0][0] == 0xA0, "decoded text has expected contents");
    }

    auto bad_magic = valid;
    bad_magic[0] = 'X';
    Check(!suyu::recomp::InspectNso(bad_magic), "bad magic is rejected");
    Check(!suyu::recomp::InspectNso(std::span<const Byte>{valid}.first(0xFF)),
          "truncated header is rejected");

    auto outside_file = valid;
    PutU32(outside_file, 0x30, 0x1000);
    Check(!suyu::recomp::InspectNso(outside_file), "segment outside file is rejected");

    auto file_overlap = valid;
    PutU32(file_overlap, 0x20, 0x108);
    Check(!suyu::recomp::InspectNso(file_overlap), "overlapping file segments are rejected");

    auto memory_overlap = valid;
    PutU32(memory_overlap, 0x24, 0x1008);
    Check(!suyu::recomp::InspectNso(memory_overlap), "overlapping memory segments are rejected");

    auto misaligned_layout = valid;
    PutU32(misaligned_layout, 0x14, 0x1004);
    const auto misaligned_inspection = suyu::recomp::InspectNso(misaligned_layout);
    Check(misaligned_inspection &&
              !suyu::recomp::ValidateNsoExecutableLayout(*misaligned_inspection.info).empty(),
          "executable layout rejects a non-page-aligned destination");

    auto reversed_layout = valid;
    PutU32(reversed_layout, 0x24, 0x4000);
    PutU32(reversed_layout, 0x34, 0x3000);
    const auto reversed_inspection = suyu::recomp::InspectNso(reversed_layout);
    Check(reversed_inspection &&
              !suyu::recomp::ValidateNsoExecutableLayout(*reversed_inspection.info).empty(),
          "executable layout rejects reversed rodata and data destinations");

    auto bad_extent = valid;
    PutU32(bad_extent, 0x88, 8);
    PutU32(bad_extent, 0x8C, 1);
    Check(!suyu::recomp::InspectNso(bad_extent), "out-of-range rodata extent is rejected");

    auto empty_extent = valid;
    PutU32(empty_extent, 0x88, 0xFFFFFFFF);
    Check(static_cast<bool>(suyu::recomp::InspectNso(empty_extent)),
          "zero-sized extent ignores its sentinel offset");

    auto empty_module_name = valid;
    PutU32(empty_module_name, 0x1C, 0);
    PutU32(empty_module_name, 0x2C, 1);
    Check(static_cast<bool>(suyu::recomp::InspectNso(empty_module_name)),
          "one-byte module-name sentinel is accepted");

    auto overlapping_module_name = valid;
    PutU32(overlapping_module_name, 0x1C, 0x100);
    PutU32(overlapping_module_name, 0x2C, 4);
    Check(!suyu::recomp::InspectNso(overlapping_module_name),
          "module-name range may not overlap a segment");

    auto overlapping_bss = valid;
    PutU32(overlapping_bss, 0x34, 0x1FF0);
    Check(!suyu::recomp::InspectNso(overlapping_bss), "data BSS tail may not overlap rodata");

    auto zero_length_bad_offset = valid;
    PutU32(zero_length_bad_offset, 0x28, 0);
    PutU32(zero_length_bad_offset, 0x20, 0xFFFFFFFF);
    Check(!suyu::recomp::InspectNso(zero_length_bad_offset),
          "zero-length segment offset beyond the file is rejected");

    auto compressed = valid;
    PutU32(compressed, 0x0C, 1);
    PutU32(compressed, 0x60, 0x10);
    const auto missing_decompressor = suyu::recomp::DecodeNso(compressed);
    Check(!missing_decompressor &&
              missing_decompressor.error.find("requires an LZ4") != std::string::npos,
          "compressed segment clearly requests an LZ4 backend");
    Check(static_cast<bool>(suyu::recomp::DecodeNso(compressed, IdentityDecompress)),
          "injected decompressor decodes compressed segment");

    auto compressed_without_bytes = compressed;
    PutU32(compressed_without_bytes, 0x60, 0);
    Check(!suyu::recomp::InspectNso(compressed_without_bytes),
          "nonempty compressed segment with no stored bytes is rejected");

    auto zbic = compressed;
    PutU32(zbic, 0x0C, 1U | (1U << 7));
    const auto zbic_result = suyu::recomp::DecodeNso(zbic, IdentityDecompress);
    Check(!zbic_result && zbic_result.error.find("ZBIC") != std::string::npos,
          "ZBIC is recognized and rejected explicitly");

    auto size_bomb = valid;
    PutU32(size_bomb, 0x0C, 1);
    PutU32(size_bomb, 0x14, 0);
    PutU32(size_bomb, 0x18, 0x40000001);
    PutU32(size_bomb, 0x60, 1);
    PutU32(size_bomb, 0x20, 0);
    PutU32(size_bomb, 0x24, 0);
    PutU32(size_bomb, 0x28, 0);
    PutU32(size_bomb, 0x30, 0);
    PutU32(size_bomb, 0x34, 0);
    PutU32(size_bomb, 0x38, 0);
    PutU32(size_bomb, 0x3C, 0);
    const auto bomb_result = suyu::recomp::DecodeNso(size_bomb, IdentityDecompress);
    Check(!bomb_result && bomb_result.error.find("configured limit") != std::string::npos,
          "decoded-size bomb is rejected before allocation");

    std::vector<Byte> fake_mod0(0x40);
    PutU32(fake_mod0, 4, 8);
    PutU32(fake_mod0, 0, 0x14000009); // b +0x24
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0,
          "entry heuristic rejects a missing MOD0 magic");
    fake_mod0[8] = 'M';
    fake_mod0[9] = 'O';
    fake_mod0[10] = 'D';
    fake_mod0[11] = '0';
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0x24,
          "entry probe accepts a bounded MOD0 header and AArch64 branch stub");
    PutU32(fake_mod0, 0, 0x14000002); // b text+8, into MOD0 metadata
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0,
          "entry probe rejects a branch into the MOD0 header");

    suyu::recomp::DecodedNso split_mod0{};
    split_mod0.info.segments[0].memory_offset = 0x1000;
    split_mod0.info.segments[1].memory_offset = 0x3000;
    split_mod0.info.segments[2].memory_offset = 0x4000;
    split_mod0.segments[0].resize(0x10);
    split_mod0.segments[1].resize(0x1C);
    PutU32(split_mod0.segments[0], 0, 0x14000002); // b text+8
    PutU32(split_mod0.segments[0], 4, 0x2000);
    std::copy_n(reinterpret_cast<const Byte*>("MOD0"), 4, split_mod0.segments[1].begin());
    Check(suyu::recomp::FindNsoAarch64EntryOffset(split_mod0) == 8,
          "entry probe resolves a module-relative MOD0 pointer into rodata");

    // HOS 19+ extends MOD0 from 0x1c to 0x34 bytes. Nonzero extension fields must
    // not be mistaken for code; the entry-stub branch is the authoritative
    // target.
    PutU32(fake_mod0, 0, 0x1400000F); // b +0x3c
    for (std::size_t offset = 0x24; offset < 0x3C; offset += 4) {
        PutU32(fake_mod0, offset, static_cast<std::uint32_t>(offset));
    }
    PutU32(fake_mod0, 0x3C, 0xD4000001);
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0x3C,
          "entry probe follows the branch over an extended MOD0 header");

    std::vector<Byte> truncated_mod0(0x0C);
    PutU32(truncated_mod0, 0, 0x14000002);
    PutU32(truncated_mod0, 4, 8);
    truncated_mod0[8] = 'M';
    truncated_mod0[9] = 'O';
    truncated_mod0[10] = 'D';
    truncated_mod0[11] = '0';
    Check(suyu::recomp::FindNsoAarch64EntryOffset(truncated_mod0) == 0,
          "entry probe rejects a truncated MOD0 header");

    fake_mod0.resize(0x3E);
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0,
          "entry probe requires a complete instruction at the branch target");

    auto hash_required = valid;
    PutU32(hash_required, 0x0C, 1U << 3);
    const auto unverified = suyu::recomp::DecodeNso(hash_required);
    Check(unverified && !unverified.image->required_hashes_verified,
          "hash-required image is marked unverified");
    Check(unverified && std::any_of(unverified.warnings.begin(), unverified.warnings.end(),
                                    [](const std::string& warning) {
                                        return warning.find("not verified") != std::string::npos;
                                    }),
          "hash-required image emits an integrity warning");

    const std::vector<Byte> npdm64 = MakeNpdm(true);
    const auto npdm64_result = suyu::recomp::InspectNpdm(npdm64);
    Check(static_cast<bool>(npdm64_result), "valid AArch64 NPDM inspects successfully");
    if (npdm64_result) {
        Check(npdm64_result.info->architecture == suyu::recomp::NpdmArchitecture::Aarch64,
              "NPDM bit zero selects AArch64");
        Check(npdm64_result.info->address_space ==
                  suyu::recomp::NpdmAddressSpace::AddressSpace64Bit,
              "NPDM address-space bits are decoded independently");
    }
    const auto npdm32_result = suyu::recomp::InspectNpdm(MakeNpdm(false, 2));
    Check(npdm32_result &&
              npdm32_result.info->architecture == suyu::recomp::NpdmArchitecture::Aarch32,
          "NPDM bit zero selects AArch32");

    auto modern_flags = npdm64;
    modern_flags[0x0C] |= 0xF0;
    Check(static_cast<bool>(suyu::recomp::InspectNpdm(modern_flags)),
          "modern NPDM feature flags are accepted");
    const auto unknown_address_space = suyu::recomp::InspectNpdm(MakeNpdm(true, 4));
    Check(unknown_address_space && !unknown_address_space.info->address_space,
          "unknown NPDM address spaces remain explicit");

    auto bad_npdm = npdm64;
    bad_npdm[0] = 'X';
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "bad META magic is rejected");
    Check(!suyu::recomp::InspectNpdm(std::span<const Byte>{npdm64}.first(0x80)),
          "META-only file cannot supply a trusted architecture");
    bad_npdm = npdm64;
    bad_npdm[0x2C0] = 'X';
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "bad ACI0 magic is rejected");
    bad_npdm = npdm64;
    bad_npdm[0x280] = 'X';
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "bad ACID magic is rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x70, 0x40);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "ACI0 may not overlap META");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x74, 0x20);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "undersized ACI0 is rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x70, 0x100);
    std::copy_n(reinterpret_cast<const Byte*>("ACI0"), 4, bad_npdm.begin() + 0x100);
    for (const std::size_t offset : {0x120, 0x128, 0x130}) {
        PutU32(bad_npdm, offset, 0x40);
    }
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "overlapping ACI0 and ACID are rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x78, 0xFFFFFFFF);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "overflowing ACID range is rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x2A0, 0x23F);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "ACID child before its header is rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x2E0, 0x40);
    PutU32(bad_npdm, 0x2E4, 1);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "ACI0 child beyond its region is rejected");
    bad_npdm = npdm64;
    bad_npdm.resize(suyu::recomp::MaximumNpdmSize + 1);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "oversized NPDM input is rejected");
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<Byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    return output &&
           output.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())) &&
           output.flush().good();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view{argv[1]}.starts_with("--write-")) {
        const std::string_view path_bytes = argv[2];
        const auto path = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(path_bytes.data()), path_bytes.size()));
        const std::string_view operation = argv[1];
        std::optional<std::vector<Byte>> fixture;
        if (operation == "--write-fixture") {
            fixture = MakeNso();
        } else if (operation == "--write-lz4-fixture") {
            fixture = MakeLz4Nso();
        } else if (operation == "--write-emittable-fixture") {
            fixture = MakeEmittableNso();
        } else if (operation == "--write-misaligned-emittable-fixture") {
            fixture = MakeEmittableNso();
            PutU32(*fixture, 0x14, 0x1004);
        } else if (operation == "--write-reversed-emittable-fixture") {
            fixture = MakeEmittableNso();
            PutU32(*fixture, 0x24, 0x6000);
        } else if (operation == "--write-npdm") {
            fixture = MakeNpdm(true);
        } else if (operation == "--write-oversized-npdm") {
            fixture = MakeNpdm(true);
            fixture->resize(suyu::recomp::MaximumNpdmSize + 1);
        } else if (operation == "--write-aarch32-npdm") {
            fixture = MakeNpdm(false, 2);
        } else if (operation == "--write-unknown-address-npdm") {
            fixture = MakeNpdm(true, 4);
        }
        if (!fixture) {
            std::cerr << "unknown fixture type\n";
            return 2;
        }
        if (!WriteBytes(path, *fixture)) {
            std::cerr << "could not write synthetic fixture\n";
            return 1;
        }
        return 0;
    }
    if (argc != 1) {
        std::cerr << "unexpected test-helper arguments\n";
        return 2;
    }

    RunTests();
    if (failures != 0) {
        std::cerr << failures << " NSO image test(s) failed\n";
        return 1;
    }
    std::cout << "NSO image tests passed\n";
    return 0;
}
