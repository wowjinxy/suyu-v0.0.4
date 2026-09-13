// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/arm64_to_c.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void Check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void CheckNoShiftBy64(std::uint32_t instruction, std::string_view description) {
    std::string output;
    Check(suyu::recomp::Translate(instruction, 0x1000, output), description);
    Check(output.find("1ULL << 64") == std::string::npos,
          "bitfield translation never emits a 64-bit C shift count");
}

void CheckPrefetchNoop(std::uint32_t instruction, std::string_view description) {
    std::string output;
    Check(suyu::recomp::Translate(instruction, 0x1000, output), description);
    Check(output == "    /* prfum */\n",
          "PRFUM emits a no-op without unused address temporaries");
}

void CheckDiscardedLoad(std::uint32_t instruction, std::string_view load,
                        std::string_view description) {
    std::string output;
    Check(suyu::recomp::Translate(instruction, 0x1000, output), description);
    Check(output.find(load) != std::string::npos,
          "loads into the zero register still perform the memory access");
}

void CheckUnhandled(std::uint32_t instruction, std::string_view description) {
    std::string output;
    Check(suyu::recomp::Translate(instruction, 0x1000, output), description);
    Check(output.find("recomp_unhandled") != std::string::npos,
          "unallocated load encoding is handed to the fallback engine");
}

} // namespace

int main() {
    // SBFIZ X8, X1, #33, #31: its sign bit lands at bit 63, so there are no upper bits to
    // extend. Emitting a nominal extension mask used to generate the undefined `1ULL << 64`.
    CheckNoShiftBy64(0x935F7828, "64-bit SBFIZ translates");

    // SBFM X0, X1, #0, #63 is a full-width move and likewise needs no extension mask.
    CheckNoShiftBy64(0x9340FC20, "full-width SBFM translates");

    // Real SDK instances with positive and negative unscaled offsets. PRFUM is a cache hint,
    // so declaring address temporaries for it only creates compiler warnings in generated C.
    CheckPrefetchNoop(0xF8801120, "positive-offset PRFUM translates");
    CheckPrefetchNoop(0xF89C0100, "negative-offset PRFUM translates");

    // Odyssey uses these LDUR WZR forms while probing memory. The destination is discarded,
    // but the load itself must still happen (and the generated address temporaries must be used).
    CheckDiscardedLoad(0xB85D019F, "(void)recomp_load32", "negative-offset LDUR WZR translates");
    CheckDiscardedLoad(0xB85E819F, "(void)recomp_load32", "second LDUR WZR translates");

    // size=10/opc=11 is unallocated, rather than a 32-bit sign-extending load.
    CheckUnhandled(0xB9C0019F, "unsigned-offset unallocated load remains a fallback");
    CheckUnhandled(0xB8C0019F, "unscaled unallocated load remains a fallback");
    CheckUnhandled(0xB8ED699F, "register-offset unallocated load remains a fallback");
    CheckUnhandled(0xF8C01120, "unallocated PRFUM opc remains a fallback");
    CheckUnhandled(0xF8801520, "unallocated PRFUM writeback remains a fallback");
    CheckUnhandled(0x88DFFD9F, "LDAR WZR retains acquire semantics through fallback");

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "arm64_to_c tests passed\n";
    return 0;
}
