// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>

#include "core/arm/arm_interface.h"

namespace Kernel {
class KProcess;
}

namespace Core {

class System;
class DynarmicExclusiveMonitor;
class ArmRecompProcessState;

/**
 * Signature of a recompiled block produced by suyu::recomp::EmitProject.
 *
 * The generated code is plain C operating on its own GuestContext, but the
 * only state it actually needs from us is the register file, PC and NZCV, so
 * the bridge below re-declares a layout-compatible view of that prefix rather
 * than pulling the generated header into the emulator build.
 */
struct RecompGuestRegs {
    u64 x[32];
    u64 pc;
    u8 n, z, c, v;
};

using RecompBlockFn = void (*)(void*);
using RecompLookupFn = RecompBlockFn (*)(u64 pc);
constexpr std::size_t RecompBuildIdSize = 0x20;
constexpr std::size_t RecompSha256Size = 0x20;
using RecompBuildIdFn = const u8* (*)();
using RecompTextHashFn = const u8* (*)();
using RecompTextSizeFn = u64 (*)();

/**
 * Registers the lookup function of a loaded recompiled image.
 *
 * Set before starting a process to have it run on ArmRecomp instead of the
 * JIT; pass nullptr to go back to the normal backend. Kept as a global rather
 * than threaded through the loader because the choice is per-image and has to
 * be visible at KProcess::InitializeInterfaces time, which the loader doesn't
 * own.
 */
void SetRecompLookup(RecompLookupFn lookup);

/// Identifies the loaded NSO by its immutable 32-byte build ID, then tells the
/// matching recompiled image where the module landed. The callback must return
/// true only after finding exactly one byte-for-byte build-ID, mapped-text-size,
/// and live-text SHA-256 match and setting that image's base. Initialization
/// fails closed before guest memory is modified when no matching image exists.
///
/// `index` and `module` are diagnostic hints only. They are not identities:
/// filenames and embedded module names routinely differ, while load order is
/// not sufficient to distinguish two unrelated single-module titles.
using RecompBindFn = bool (*)(std::size_t index, const char* module, u64 base,
                              const u8* build_id, std::size_t build_id_size,
                              const u8* text_sha256, std::size_t text_sha256_size,
                              u64 text_size);
void SetRecompBinder(RecompBindFn binder);

/// Returns the registered lookup, or nullptr when no recompiled image is
/// loaded and the JIT should be used.
RecompLookupFn GetRecompLookup();

/**
 * Creates the initialization state shared by every ArmRecomp core belonging
 * to one guest process.
 *
 * The definition stays private to the backend; KProcess owns one handle and
 * passes it to each core so module registration and relocation happen once
 * for that process rather than once per core (or once globally).
 */
std::shared_ptr<ArmRecompProcessState> CreateArmRecompProcessState();

/**
 * CPU backend that executes statically recompiled AArch64 rather than JITing
 * it.
 *
 * The point of routing this through ArmInterface is that everything above the
 * CPU in suyu - the HLE kernel, every service, the GPU - reaches the guest
 * only through this interface. Satisfying it means a recompiled game gets the
 * real HLE and GPU stack instead of the generated runtime's stub SVC handler,
 * which is what stops a recompiled commercial title from getting past its
 * first service call.
 *
 * Execution model: RunThread runs recompiled blocks back to back until the
 * generated code parks on an SVC (it writes the instruction's imm and the
 * resume PC into the context, then returns), at which point we hand control
 * back to the kernel with HaltReason::SupervisorCall exactly as the dynarmic
 * backend does. The kernel then reads arguments through GetSvcArguments,
 * dispatches the real service call, and writes results back through
 * SetSvcArguments before resuming us.
 */
class ArmRecomp final : public ArmInterface {
public:
    /// `process`, `exclusive_monitor` and `core_index` are only used to build a
    /// dynarmic JIT lazily, the first time a PC is reached that the static pass
    /// never covered (an indirect call into code no heuristic found). Without
    /// that fallback such a gap is terminal: the thread is suspended for a
    /// debugger that is not attached and the game hangs on a black screen with
    /// no forward progress.
    explicit ArmRecomp(System& system, bool uses_wall_clock, RecompLookupFn lookup,
                       std::shared_ptr<ArmRecompProcessState> process_state,
                       Kernel::KProcess* process, DynarmicExclusiveMonitor* exclusive_monitor,
                       std::size_t core_index);
    ~ArmRecomp() override;

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

    Architecture GetArchitecture() const override {
        return Architecture::AArch64;
    }

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;

    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override;
    void RewindBreakpointInstruction() override;

private:
    /// Builds the JIT fallback if needed and marks this thread as running on
    /// it. Returns false when no JIT can be built (no process/monitor).
    bool EnterFallback(Kernel::KThread* thread);
    /// Runs the JIT fallback, syncing guest state in and back out. A full
    /// scheduling slice returns to recompiled execution once the PC is covered
    /// again; single-step mode executes exactly one JIT instruction.
    HaltReason RunFallback(Kernel::KThread* thread, bool single_step = false);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core
