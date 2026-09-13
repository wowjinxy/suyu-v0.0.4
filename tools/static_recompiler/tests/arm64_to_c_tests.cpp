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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "arm64_to_c tests passed\n";
    return 0;
}
