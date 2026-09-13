// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/arm64_to_c.h"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

using suyu::recomp::u8;
using suyu::recomp::u64;

struct Options {
    std::filesystem::path text_path;
    std::filesystem::path output_path;
    std::string module = "main";
    std::string title;
    u64 base{};
    std::optional<u64> entry;
    std::vector<u64> extra_roots;
    bool force = false;
};

void PrintUsage(std::ostream& out, std::string_view executable) {
    out << "suyu-recomp - experimental AArch64 static recompiler\n\n"
        << "Usage:\n"
        << "  " << executable
        << " emit-raw --input <text.bin> --base <address> --output <directory> [options]\n"
        << "  " << executable
        << " <text.bin> <address> <directory> [--source-only]  (legacy form)\n\n"
        << "Required:\n"
        << "  --input <path>       Raw, little-endian AArch64 .text bytes\n"
        << "  --base <address>     Guest virtual address, decimal or 0x-prefixed hex\n"
        << "  --output <path>      Directory for the generated CMake project\n\n"
        << "Options:\n"
        << "  --entry <address>    Initial guest PC (defaults to --base)\n"
        << "  --root <address>     Additional block-discovery root; may be repeated\n"
        << "  --module <name>      C identifier fragment (defaults to main)\n"
        << "  --title <text>       Display title embedded in the standalone runner\n"
        << "  --force              Allow writing into a non-empty output directory\n"
        << "  -h, --help           Show this help\n\n"
        << "This low-level tool accepts a decoded AArch64 text segment. NSP/XCI/NCA/NSO\n"
        << "loading remains in suyu's game exporter. Generated standalone programs use\n"
        << "stub services; real games require the hosted suyu ArmRecomp runtime.\n";
}

bool IsOption(std::string_view value) {
    return value.size() > 1 && value.front() == '-';
}

std::optional<u64> ParseAddress(std::string_view text) {
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty() || text.front() == '-') {
        return std::nullopt;
    }

    u64 result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return result;
}

bool IsModuleName(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    for (const unsigned char ch : name) {
        if ((ch < 'a' || ch > 'z') && (ch < 'A' || ch > 'Z') &&
            (ch < '0' || ch > '9') && ch != '_') {
            return false;
        }
    }
    return true;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

#ifdef _WIN32
std::optional<std::vector<std::string>> GetUtf8Arguments() {
    int wide_count = 0;
    wchar_t** wide_arguments = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (!wide_arguments) {
        return std::nullopt;
    }

    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(wide_count));
    bool conversion_failed = false;
    for (int i = 0; i < wide_count; ++i) {
        const std::wstring_view wide = wide_arguments[i];
        if (wide.empty()) {
            arguments.emplace_back();
            continue;
        }
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                             static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                             nullptr);
        if (size <= 0) {
            conversion_failed = true;
            break;
        }
        std::string utf8(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), utf8.data(), size, nullptr,
                                nullptr) != size) {
            conversion_failed = true;
            break;
        }
        arguments.push_back(std::move(utf8));
    }
    LocalFree(wide_arguments);
    if (conversion_failed) {
        return std::nullopt;
    }
    return arguments;
}
#endif

std::optional<std::vector<u8>> ReadFile(const std::filesystem::path& path,
                                        std::string_view description) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        std::cerr << "error: cannot open " << description << ": " << PathToUtf8(path) << '\n';
        return std::nullopt;
    }

    const std::streampos end = stream.tellg();
    if (end < 0) {
        std::cerr << "error: cannot determine the size of " << description << ": "
                  << PathToUtf8(path) << '\n';
        return std::nullopt;
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        std::cerr << "error: " << description << " is too large for this host\n";
        return std::nullopt;
    }

    std::vector<u8> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!bytes.empty() &&
        !stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()))) {
        std::cerr << "error: could not read all of " << description << ": " << PathToUtf8(path)
                  << '\n';
        return std::nullopt;
    }
    return bytes;
}

std::optional<Options> ParseOptions(int argc, char** argv) {
    Options options;
    bool have_input = false;
    bool have_base = false;
    bool have_output = false;

    const bool has_command = argc > 1 && std::string_view{argv[1]} == "emit-raw";
    if (!has_command && argc >= 4 && !IsOption(argv[1]) && !IsOption(argv[2]) &&
        !IsOption(argv[3])) {
        options.text_path = suyu::recomp::Utf8Path(argv[1]);
        const auto base = ParseAddress(argv[2]);
        if (!base) {
            std::cerr << "error: invalid base address: " << argv[2] << '\n';
            return std::nullopt;
        }
        options.base = *base;
        options.output_path = suyu::recomp::Utf8Path(argv[3]);
        have_input = have_base = have_output = true;
    }

    const int start = have_input ? 4 : (has_command ? 2 : 1);
    for (int i = start; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto value_after = [&](std::string_view option) -> std::optional<std::string_view> {
            if (argument != option) {
                return std::nullopt;
            }
            if (++i >= argc) {
                std::cerr << "error: " << option << " requires a value\n";
                return std::string_view{};
            }
            return argv[i];
        };

        if (argument == "-h" || argument == "--help") {
            PrintUsage(std::cout, argv[0]);
            std::exit(0);
        }
        if (argument == "--source-only") {
            // Kept for compatibility with the original positional CLI. EmitProject only emits
            // source and has never invoked a compiler itself.
            continue;
        }
        if (argument == "--force") {
            options.force = true;
            continue;
        }
        if (const auto value = value_after("--input")) {
            if (value->empty()) {
                return std::nullopt;
            }
            options.text_path = suyu::recomp::Utf8Path(std::string{*value});
            have_input = true;
            continue;
        }
        if (const auto value = value_after("--base")) {
            if (value->empty()) {
                return std::nullopt;
            }
            const auto address = ParseAddress(*value);
            if (!address) {
                std::cerr << "error: invalid base address: " << *value << '\n';
                return std::nullopt;
            }
            options.base = *address;
            have_base = true;
            continue;
        }
        if (const auto value = value_after("--output")) {
            if (value->empty()) {
                return std::nullopt;
            }
            options.output_path = suyu::recomp::Utf8Path(std::string{*value});
            have_output = true;
            continue;
        }
        if (const auto value = value_after("--entry")) {
            if (value->empty()) {
                return std::nullopt;
            }
            options.entry = ParseAddress(*value);
            if (!options.entry) {
                std::cerr << "error: invalid entry address: " << *value << '\n';
                return std::nullopt;
            }
            continue;
        }
        if (const auto value = value_after("--root")) {
            if (value->empty()) {
                return std::nullopt;
            }
            const auto address = ParseAddress(*value);
            if (!address) {
                std::cerr << "error: invalid discovery root address: " << *value << '\n';
                return std::nullopt;
            }
            options.extra_roots.push_back(*address);
            continue;
        }
        if (const auto value = value_after("--module")) {
            if (value->empty()) {
                return std::nullopt;
            }
            options.module = *value;
            continue;
        }
        if (const auto value = value_after("--title")) {
            if (value->empty()) {
                return std::nullopt;
            }
            options.title = *value;
            continue;
        }
        std::cerr << "error: unknown argument: " << argument << '\n';
        return std::nullopt;
    }

    if (!have_input || !have_base || !have_output) {
        std::cerr << "error: --input, --base, and --output are required\n";
        return std::nullopt;
    }
    if (!IsModuleName(options.module)) {
        std::cerr << "error: --module must contain 1-32 ASCII letters, digits, or underscores\n";
        return std::nullopt;
    }
    if ((options.base & 3) != 0) {
        std::cerr << "error: --base must be 4-byte aligned\n";
        return std::nullopt;
    }

    return options;
}

int Run(int argc, char** argv) {
    if (argc == 1) {
        PrintUsage(std::cerr, argv[0]);
        return 2;
    }

    const auto parsed = ParseOptions(argc, argv);
    if (!parsed) {
        std::cerr << "Run with --help for usage.\n";
        return 2;
    }
    const Options& options = *parsed;

    const auto text = ReadFile(options.text_path, "text segment");
    if (!text) {
        return 1;
    }
    if (text->empty() || (text->size() & 3) != 0) {
        std::cerr << "error: the text segment must be non-empty and a multiple of four bytes\n";
        return 1;
    }
    if (text->size() > std::numeric_limits<u64>::max() - options.base) {
        std::cerr << "error: text segment address range overflows 64 bits\n";
        return 1;
    }

    const u64 entry = options.entry.value_or(options.base);
    if ((entry & 3) != 0 || entry < options.base || entry >= options.base + text->size()) {
        std::cerr << "error: --entry must be aligned and inside the text segment\n";
        return 1;
    }

    for (const u64 root : options.extra_roots) {
        if ((root & 3) != 0 || root < options.base || root >= options.base + text->size()) {
            std::cerr << "error: every --root must be aligned and inside the text segment\n";
            return 1;
        }
    }

    std::error_code output_error;
    const bool output_exists = std::filesystem::exists(options.output_path, output_error);
    if (output_error) {
        std::cerr << "error: cannot inspect output path: " << output_error.message() << '\n';
        return 1;
    }
    bool output_has_entries = false;
    if (output_exists) {
        const bool output_is_directory =
            std::filesystem::is_directory(options.output_path, output_error);
        if (output_error) {
            std::cerr << "error: cannot inspect output path: " << output_error.message() << '\n';
            return 1;
        }
        if (!output_is_directory) {
            std::cerr << "error: output path exists and is not a directory: "
                      << PathToUtf8(options.output_path) << '\n';
            return 1;
        }
        const std::filesystem::directory_iterator first(options.output_path, output_error);
        if (output_error) {
            std::cerr << "error: cannot inspect output directory: " << output_error.message()
                      << '\n';
            return 1;
        }
        output_has_entries = first != std::filesystem::directory_iterator{};
    }
    if (output_has_entries && !options.force) {
        std::cerr << "error: output directory is not empty; pass --force to overwrite the "
                     "generated files\n";
        return 1;
    }

    try {
        const std::string output = PathToUtf8(options.output_path);
        const auto stats = suyu::recomp::EmitProject(
            options.module, text->data(), text->size(), options.base, output, true, nullptr, 0,
            nullptr, 0, entry, options.title,
            options.extra_roots.empty() ? nullptr : &options.extra_roots);

        const std::vector<std::filesystem::path> required_files{
            "CMakeLists.txt",          "main.c",
            "recomp_export.c",        "recomp_runtime.c",
            "recomp_runtime.h",       "data/text.bin",
            std::filesystem::path{"src"} / ("recompiled_" + options.module + ".c"),
            std::filesystem::path{"src"} / ("recompiled_" + options.module + "_0.c"),
        };
        for (const auto& relative_path : required_files) {
            const std::filesystem::path generated_path = options.output_path / relative_path;
            if (!std::filesystem::is_regular_file(generated_path)) {
                std::cerr << "error: generation did not produce " << PathToUtf8(generated_path)
                          << '\n';
                return 1;
            }
        }

        std::cout << "Generated " << stats.blocks << " blocks from " << stats.instructions
                  << " AArch64 instructions (" << stats.translated_terminators
                  << " translated terminators).\nOutput: " << PathToUtf8(options.output_path) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "error: generation failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    auto utf8_arguments = GetUtf8Arguments();
    if (!utf8_arguments) {
        std::cerr << "error: could not convert the Windows command line to UTF-8\n";
        return 1;
    }
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(utf8_arguments->size());
    for (std::string& argument : *utf8_arguments) {
        argument_pointers.push_back(argument.data());
    }
    return Run(static_cast<int>(argument_pointers.size()), argument_pointers.data());
#else
    return Run(argc, argv);
#endif
}
