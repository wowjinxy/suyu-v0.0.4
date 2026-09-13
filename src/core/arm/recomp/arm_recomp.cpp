// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <openssl/evp.h>

#include "common/logging/log.h"
#include "core/arm/debug.h"
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/core.h"
#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/recompiler/nso_dynamic.h"
#include "core/recompiler/nso_relocation_transaction.h"
#include "core/recompiler/nso_relocations.h"
#include "core/recompiler/nso_symbols.h"

namespace Core {

namespace {
// Mirrors the prefix of the GuestContext the recompiler emits. Only the fields
// the emulator needs to observe or mutate are modelled; the generated struct
// carries additional members after these (heap bookkeeping, save-data handles)
// which the recompiled code manages itself and we never touch.
struct GuestContextView {
    u64 x[32];
    u64 pc;
    u8 n, z, c, v;
    u8* mem;
    u64 mem_size;
    u64 mem_base_vaddr;
    int halted;
    u64 pending_svc;
    // The SIMD/FP register file and thread pointer sit immediately after
    // pending_svc in the emitted struct. They have to be modelled here rather
    // than left off the end: the recompiler emits SIMD code, so a context
    // switch that did not carry these would silently lose every floating-point
    // and vector register the guest had live.
    u64 vreg[32][2];
    u64 tpidr_el0;
    const void* host_mem;
    // TPIDRRO_EL0 - the kernel-published thread-local region, whose first
    // 0x100 bytes are the IPC message buffer KServerSession reads a
    // SendSyncRequest out of. Kept apart from tpidr_el0 (the guest's own
    // thread pointer) because the two have different owners and different
    // lifetimes; see the note on GuestContext in core/recompiler/arm64_to_c.h.
    u64 tpidrro_el0;
};

// Matches RecompHostMem in the generated runtime. The recompiled code calls
// through this for every guest access, so that it reads and writes the
// emulator's address space rather than the flat buffer the standalone runtime
// would otherwise own - without it the recompiled code and the HLE kernel
// would be looking at two different memories.
struct RecompHostMem {
    void* user;
    u64 (*load)(void* user, u64 va, u32 size);
    void (*store)(void* user, u64 va, u32 size, u64 value);
};

// Nothing links these two builds together, so the shared layout is pinned on
// both sides: the generated runtime asserts the same four offsets against its
// own GuestContext. If a field is ever inserted rather than appended, one of
// the two fails to compile instead of the emulator silently reading the wrong
// registers.
static_assert(offsetof(GuestContextView, pc) == 256);
static_assert(offsetof(GuestContextView, pending_svc) == 304);
static_assert(offsetof(GuestContextView, vreg) == 312);
static_assert(offsetof(GuestContextView, tpidr_el0) == 824);
static_assert(offsetof(GuestContextView, host_mem) == 832);
static_assert(offsetof(GuestContextView, tpidrro_el0) == 840);

// The generated code signals an SVC by parking with this set. Kept in sync
// with the emitted recomp_svc contract in core/recompiler/arm64_to_c.h.
constexpr u64 kNoPendingSvc = ~0ULL;

// Mirrors RECOMP_HALT_UNHANDLED in the generated runtime: a block that halts
// with this parked its PC on an instruction the decoder cannot translate and
// is asking for that address to be executed by the interpreter fallback.
constexpr int kHaltUnhandled = 2;
} // namespace

namespace {
std::atomic<RecompLookupFn> g_recomp_lookup{nullptr};
std::atomic<RecompBindFn> g_recomp_binder{nullptr};
} // namespace

void SetRecompLookup(RecompLookupFn lookup) {
    g_recomp_lookup.store(lookup, std::memory_order_release);
}

void SetRecompBinder(RecompBindFn binder) {
    g_recomp_binder.store(binder, std::memory_order_release);
}

RecompLookupFn GetRecompLookup() {
    return g_recomp_lookup.load(std::memory_order_acquire);
}

class ArmRecompProcessState {
public:
    enum class Initialization {
        Uninitialized,
        Ready,
        FallbackOnly,
        Fatal,
    };

    std::once_flag initialize_once;
    std::once_flag announce_once;
    Loader::AppLoader::Modules modules;
    Initialization initialization{Initialization::Uninitialized};
    std::string error;
    bool relocations_committed{};
    u64 relocation_writes{};
    u64 finalization_writes{};
};

std::shared_ptr<ArmRecompProcessState> CreateArmRecompProcessState() {
    return std::make_shared<ArmRecompProcessState>();
}

struct ArmRecomp::Impl {
    Impl(System& system_, RecompLookupFn lookup_,
         std::shared_ptr<ArmRecompProcessState> process_state_)
        : system{system_}, lookup{lookup_}, process_state{std::move(process_state_)} {
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.pending_svc = kNoPendingSvc;
        // Point the recompiled code at the emulator's address space.
        bridge.user = this;
        bridge.load = &Impl::HostLoad;
        bridge.store = &Impl::HostStore;
        ctx.host_mem = &bridge;
    }

    static u64 HostLoad(void* user, u64 va, u32 size) {
        auto& memory = static_cast<Impl*>(user)->system.ApplicationMemory();
        switch (size) {
        case 1: return memory.Read8(va);
        case 2: return memory.Read16(va);
        case 4: return memory.Read32(va);
        default: return memory.Read64(va);
        }
    }

    static void HostStore(void* user, u64 va, u32 size, u64 value) {
        auto& memory = static_cast<Impl*>(user)->system.ApplicationMemory();
        switch (size) {
        case 1: memory.Write8(va, static_cast<u8>(value)); break;
        case 2: memory.Write16(va, static_cast<u16>(value)); break;
        case 4: memory.Write32(va, static_cast<u32>(value)); break;
        default: memory.Write64(va, value); break;
        }
    }

    using Initialization = ArmRecompProcessState::Initialization;

    struct LiveModuleMemory {
        Core::Memory::Memory* memory{};
        u64 base{};
        u64 image_size{};
    };

    struct WritableRegion {
        u64 address{};
        u64 end{};

        bool Contains(u64 candidate, u64 size) const {
            return candidate >= address && candidate <= end && size <= end - candidate;
        }
    };

    static bool RangeFits(u64 offset, u64 size, u64 limit) {
        return offset <= limit && size <= limit - offset;
    }

    static bool AddAddress(u64 base, u64 offset, u64& result) {
        if (offset > std::numeric_limits<u64>::max() - base) {
            return false;
        }
        result = base + offset;
        return true;
    }

    static bool ReadModuleMemory(const void* user, u64 module_address, std::span<u8> destination) {
        const auto& module = *static_cast<const LiveModuleMemory*>(user);
        u64 address = 0;
        if (module.memory == nullptr ||
            !RangeFits(module_address, destination.size(), module.image_size) ||
            !AddAddress(module.base, module_address, address)) {
            return false;
        }
        return module.memory->ReadBlock(Kernel::KProcessAddress{address}, destination.data(),
                                        destination.size());
    }

    static bool ReadTransactionMemory(void* user, u64 address, std::span<u8> destination) {
        auto& memory = *static_cast<Core::Memory::Memory*>(user);
        return memory.ReadBlock(Kernel::KProcessAddress{address}, destination.data(),
                                destination.size());
    }

    static bool WriteTransactionMemory(void* user, u64 address, std::span<const u8> source) {
        auto& memory = *static_cast<Core::Memory::Memory*>(user);
        return memory.WriteBlock(Kernel::KProcessAddress{address}, source.data(), source.size());
    }

    static bool HashMappedText(Core::Memory::Memory& memory, u64 base, u64 size,
                               std::array<u8, RecompSha256Size>& digest) {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(),
                                                                       EVP_MD_CTX_free};
        if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
            return false;
        }

        std::array<u8, 64 * 1024> buffer{};
        u64 offset = 0;
        while (offset < size) {
            const std::size_t chunk =
                static_cast<std::size_t>((std::min)(size - offset, u64{buffer.size()}));
            if (!memory.ReadBlock(Kernel::KProcessAddress{base + offset}, buffer.data(), chunk) ||
                EVP_DigestUpdate(context.get(), buffer.data(), chunk) != 1) {
                return false;
            }
            offset += chunk;
        }

        unsigned int digest_size = 0;
        return EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) == 1 &&
               digest_size == digest.size();
    }

    void FailInitialization(Initialization state, std::string error) {
        process_state->initialization = state;
        process_state->error = std::move(error);
        if (state == Initialization::Fatal) {
            LOG_CRITICAL(Core_ARM, "recomp: process initialization is fatal: {}",
                         process_state->error);
        } else {
            LOG_WARNING(Core_ARM, "recomp: using JIT fallback: {}", process_state->error);
        }
    }

    bool PreflightWritableAddress(Kernel::KProcess& process, u64 address,
                                  std::optional<WritableRegion>& cached_region,
                                  std::string& error) {
        constexpr u64 WriteSize = sizeof(u64);
        if ((address & (WriteSize - 1)) != 0 ||
            address > std::numeric_limits<u64>::max() - WriteSize) {
            error = fmt::format("unaligned or overflowing relocation destination {:#x}", address);
            return false;
        }
        if (cached_region && cached_region->Contains(address, WriteSize)) {
            return true;
        }

        Kernel::KMemoryInfo memory_info{};
        Kernel::Svc::PageInfo page_info{};
        const auto query =
            process.GetPageTable().QueryInfo(std::addressof(memory_info), std::addressof(page_info),
                                             Kernel::KProcessAddress{address});
        if (query.IsFailure()) {
            error = fmt::format("could not query relocation destination {:#x}", address);
            return false;
        }
        const Kernel::Svc::MemoryInfo info = memory_info.GetSvcMemoryInfo();
        if (info.size == 0 || info.size > std::numeric_limits<u64>::max() - info.base_address) {
            error =
                fmt::format("relocation destination {:#x} has an invalid memory region", address);
            return false;
        }
        const u64 end = info.base_address + info.size;
        const bool writable_code_data = (info.state == Kernel::Svc::MemoryState::CodeData ||
                                         info.state == Kernel::Svc::MemoryState::AliasCodeData) &&
                                        info.permission == Kernel::Svc::MemoryPermission::ReadWrite;
        if (!writable_code_data || address < info.base_address || address >= end ||
            WriteSize > end - address) {
            error = fmt::format(
                "relocation destination {:#x} is not wholly inside writable NSO data", address);
            return false;
        }
        cached_region = WritableRegion{info.base_address, end};
        return true;
    }

    bool PreflightRelocationWrites(Kernel::KProcess& process,
                                   const suyu::recomp::NsoRelocationPlan& plan,
                                   const suyu::recomp::NsoDynamicInfo& dynamic,
                                   std::string& error) {
        std::optional<WritableRegion> cached_region;
        for (const auto& write : plan.writes) {
            if (!PreflightWritableAddress(process, write.address, cached_region, error)) {
                return false;
            }
        }
        const auto preflight_finalizer =
            [&](const auto& table, const std::optional<u64>& value_address, const char* tag) {
                if (!table || table->byte_size == 0) {
                    return true;
                }
                u64 address = 0;
                if (!value_address || !AddAddress(plan.module_base, *value_address, address)) {
                    error = std::string{tag} + " has no valid mapped value field";
                    return false;
                }
                return PreflightWritableAddress(process, address, cached_region, error);
            };
        return preflight_finalizer(dynamic.rela, dynamic.rela_size_value_address, "DT_RELASZ") &&
               preflight_finalizer(dynamic.plt_rela, dynamic.plt_rela_size_value_address,
                                   "DT_PLTRELSZ");
    }

    void InitializeProcess(Kernel::KProcess* process) {
        if (process == nullptr) {
            FailInitialization(Initialization::FallbackOnly, "guest process is unavailable");
            return;
        }

        Loader::AppLoader::Modules modules;
        const Loader::ResultStatus module_status = system.GetAppLoader().ReadNSOModules(modules);
        if (module_status != Loader::ResultStatus::Success) {
            FailInitialization(Initialization::FallbackOnly,
                               "loader did not expose NSO modules: " +
                                   Loader::GetResultStatusString(module_status));
            return;
        }
        process_state->modules = modules;
        if (modules.size() != 1) {
            FailInitialization(
                Initialization::FallbackOnly,
                fmt::format("safe hosted relocation currently requires exactly one NSO module; "
                            "the loader reported {}",
                            modules.size()));
            return;
        }

        const auto& [module_base, module_name] = *modules.begin();
        const auto image_layout =
            GetNsoModuleImageLayout(process, Kernel::KProcessAddress{module_base});
        if (!image_layout) {
            FailInitialization(Initialization::FallbackOnly,
                               "loaded NSO memory layout failed structural validation");
            return;
        }

        LiveModuleMemory live_memory{
            .memory = &process->GetMemory(),
            .base = module_base,
            .image_size = image_layout->image_size,
        };
        const suyu::recomp::NsoModuleView module{
            .image_size = image_layout->image_size,
            .text_address = 0,
            .user = &live_memory,
            .read = ReadModuleMemory,
        };
        const auto dynamic = suyu::recomp::ParseNsoDynamic(module);
        if (!dynamic) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO dynamic metadata was rejected: " + dynamic.error);
            return;
        }
        if (!dynamic.warnings.empty()) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO uses unsupported dynamic metadata: " +
                                   dynamic.warnings.front());
            return;
        }

        const auto symbols = suyu::recomp::ParseNsoDynamicSymbols(module, *dynamic.info);
        if (!symbols) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO dynamic symbols were rejected: " + symbols.error);
            return;
        }
        const auto relocation_plan = suyu::recomp::PlanNsoRelocations(
            image_layout->image_size, *dynamic.info, *symbols.info, module_base);
        if (!relocation_plan) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO relocations were rejected: " + relocation_plan.error);
            return;
        }

        std::string preflight_error;
        if (!PreflightRelocationWrites(*process, *relocation_plan.plan, *dynamic.info,
                                       preflight_error)) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO relocation preflight failed: " + preflight_error);
            return;
        }

        const auto binder = g_recomp_binder.load(std::memory_order_acquire);
        if (binder == nullptr) {
            FailInitialization(Initialization::FallbackOnly,
                               "recompiled image has no identity-and-base binding callback");
            return;
        }
        auto& memory = process->GetMemory();
        std::array<u8, RecompSha256Size> text_sha256{};
        if (!HashMappedText(memory, module_base, image_layout->text_size, text_sha256)) {
            FailInitialization(Initialization::FallbackOnly,
                               "could not hash the live mapped NSO text image");
            return;
        }
        const auto& build_id = system.GetApplicationProcessBuildID();
        if (!binder(0, module_name.c_str(), module_base, build_id.data(), build_id.size(),
                    text_sha256.data(), text_sha256.size(), image_layout->text_size)) {
            FailInitialization(
                Initialization::FallbackOnly,
                "no unique recompiled image matched the loaded NSO identity and live text");
            return;
        }

        const suyu::recomp::NsoRelocationMemory transaction_memory{
            .user = &memory,
            .read = ReadTransactionMemory,
            .write = WriteTransactionMemory,
        };
        const auto committed = suyu::recomp::CommitNsoRelocationPlan(
            *relocation_plan.plan, *dynamic.info, transaction_memory);
        if (!committed) {
            const Initialization failure =
                committed.state == suyu::recomp::NsoRelocationCommitState::RollbackFailed
                    ? Initialization::Fatal
                    : Initialization::FallbackOnly;
            FailInitialization(failure, "NSO relocation transaction failed: " + committed.error);
            return;
        }
        process_state->relocations_committed = true;
        process_state->relocation_writes = committed.relocation_writes;
        process_state->finalization_writes = committed.finalization_writes;
        process_state->initialization = Initialization::Ready;
        LOG_INFO(Core_ARM,
                 "recomp: committed {} relocations and {} finalizers for '{}' at {:#x} "
                 "(image size {:#x})",
                 committed.relocation_writes, committed.finalization_writes, module_name,
                 module_base, image_layout->image_size);
    }

    /// Discover modules, validate and commit relocations, then publish the
    /// generated image's runtime base as one process-wide initialization step.
    /// Every CPU core calls this gate before dispatch; call_once also publishes
    /// the completed memory transaction and result to all of them.
    Initialization EnsureProcessInitialized(Kernel::KThread* thread) {
        std::call_once(process_state->initialize_once, [this, thread] {
            try {
                Kernel::KProcess* process = owner_process;
                if (process == nullptr && thread != nullptr) {
                    process = thread->GetOwnerProcess();
                }
                InitializeProcess(process);
                if (process_state->initialization == Initialization::Uninitialized) {
                    FailInitialization(Initialization::FallbackOnly,
                                       "runtime initialization produced no result");
                }
            } catch (const std::exception& exception) {
                FailInitialization(
                    process_state->relocations_committed ? Initialization::Fatal
                                                         : Initialization::FallbackOnly,
                    "runtime initialization threw: " + std::string{exception.what()});
            } catch (...) {
                FailInitialization(process_state->relocations_committed
                                       ? Initialization::Fatal
                                       : Initialization::FallbackOnly,
                                   "runtime initialization threw an unknown exception");
            }
        });
        return process_state->initialization;
    }
    System& system;
    RecompLookupFn lookup{};
    GuestContextView ctx{};
    RecompHostMem bridge{};
    u32 fpcr{};
    u32 fpsr{};
    std::atomic<bool> interrupted{false};
    std::shared_ptr<ArmRecompProcessState> process_state;
    static constexpr size_t kTrail = 32;
    u64 trail[kTrail]{};
    size_t trail_pos{0};

    // Interpreter fallback for PCs the static pass never covered. Built on the
    // first miss rather than up front: most runs never need it, and a JIT per
    // core costs a code cache each.
    Kernel::KProcess* owner_process{};
    DynarmicExclusiveMonitor* exclusive_monitor{};
    std::size_t core_index{};
    bool uses_wall_clock{};
    std::unique_ptr<ArmDynarmic64> fallback{};
    std::atomic<ArmDynarmic64*> fallback_signal_target{nullptr};
    bool in_fallback{false};
    bool fallback_unavailable{false};
};

ArmRecomp::ArmRecomp(System& system, bool uses_wall_clock, RecompLookupFn lookup,
                     std::shared_ptr<ArmRecompProcessState> process_state,
                     Kernel::KProcess* process, DynarmicExclusiveMonitor* exclusive_monitor,
                     std::size_t core_index)
    : ArmInterface{uses_wall_clock},
      impl{std::make_unique<Impl>(system, lookup, std::move(process_state))} {
    impl->owner_process = process;
    impl->exclusive_monitor = exclusive_monitor;
    impl->core_index = core_index;
    impl->uses_wall_clock = uses_wall_clock;
}

ArmRecomp::~ArmRecomp() = default;

bool ArmRecomp::EnterFallback(Kernel::KThread* thread) {
    if (impl->fallback_unavailable) {
        return false;
    }
    if (!impl->fallback) {
        if (!impl->owner_process || !impl->exclusive_monitor) {
            impl->fallback_unavailable = true;
            return false;
        }
        impl->fallback = std::make_unique<ArmDynarmic64>(
            impl->system, impl->uses_wall_clock, impl->owner_process, *impl->exclusive_monitor,
            impl->core_index);
        impl->fallback->SetWatchpointArray(m_watchpoints);
        impl->fallback_signal_target.store(impl->fallback.get(), std::memory_order_release);
        if (impl->interrupted.load(std::memory_order_acquire)) {
            impl->fallback->SignalInterrupt(thread);
        }
        LOG_WARNING(Core_ARM, "recomp: created JIT fallback for uncovered code");
    }
    impl->in_fallback = true;
    return true;
}

HaltReason ArmRecomp::RunFallback(Kernel::KThread* thread, bool single_step) {
    // The recompiled context is the single source of truth; the JIT is loaded
    // from it on the way in and drained back on the way out, so every accessor
    // on this interface (SVC arguments, thread context save/restore) keeps
    // working unchanged no matter which engine actually ran.
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.halted = 0;

    Kernel::Svc::ThreadContext tctx{};
    this->GetContext(tctx);
    impl->fallback->SetContext(tctx);
    impl->fallback->SetTpidrroEl0(impl->ctx.tpidrro_el0);

    const HaltReason hr =
        single_step ? impl->fallback->StepThread(thread) : impl->fallback->RunThread(thread);

    impl->fallback->GetContext(tctx);
    this->SetContext(tctx);
    if (True(hr & HaltReason::SupervisorCall)) {
        impl->ctx.pending_svc = impl->fallback->GetSvcNumber();
    }
    if (True(hr & HaltReason::BreakLoop)) {
        impl->interrupted.exchange(false, std::memory_order_acq_rel);
    }

    // Return to recompiled execution as soon as the PC is covered again, so a
    // single uncovered function costs only the time spent inside it. A process
    // whose image failed validation stays entirely on the JIT.
    if (impl->process_state->initialization == ArmRecompProcessState::Initialization::Ready &&
        impl->lookup && impl->lookup(impl->ctx.pc)) {
        impl->in_fallback = false;
    }
    return hr;
}

HaltReason ArmRecomp::RunThread(Kernel::KThread* thread) {
    // Logged once so it is obvious from a log whether the backend was ever
    // entered at all. A run with no errors is otherwise indistinguishable from
    // a run where the guest thread was never scheduled onto it.
    std::call_once(impl->process_state->announce_once, [this] {
        LOG_INFO(Core_ARM, "ArmRecomp::RunThread entered, pc={:#x}", impl->ctx.pc);
    });
    if (!impl->lookup) {
        LOG_ERROR(Core_ARM, "No recompiled code registered; cannot run thread");
        return HaltReason::BreakLoop;
    }

    const auto initialization = impl->EnsureProcessInitialized(thread);
    if (initialization == ArmRecompProcessState::Initialization::Fatal) {
        return HaltReason::PrefetchAbort;
    }
    if (initialization == ArmRecompProcessState::Initialization::FallbackOnly) {
        if (!EnterFallback(thread)) {
            LOG_CRITICAL(Core_ARM,
                         "recomp: runtime image initialization failed and no JIT fallback is "
                         "available");
            return HaltReason::PrefetchAbort;
        }
        return RunFallback(thread);
    }
    if (initialization != ArmRecompProcessState::Initialization::Ready) {
        LOG_CRITICAL(Core_ARM, "recomp: runtime image initialization has no terminal state");
        return HaltReason::PrefetchAbort;
    }

    // A previous miss handed this thread to the JIT; keep running there until
    // the PC lands back inside recompiled code.
    if (impl->in_fallback) {
        return RunFallback(thread);
    }

    impl->ctx.halted = 0;

    while (!impl->ctx.halted) {
        if (impl->interrupted.exchange(false, std::memory_order_acq_rel)) {
            return HaltReason::BreakLoop;
        }

        // An SVC parked us last time round; the kernel has now serviced it and
        // resumed, so clear it before continuing.
        if (impl->ctx.pending_svc != kNoPendingSvc) {
            impl->ctx.pending_svc = kNoPendingSvc;
        }

        // The host-side dispatcher picks the image that owns this absolute PC.
        // That image then subtracts the runtime base registered during process
        // initialization before looking up its module-relative block.
        // Rolling trail of the last few PCs. A wild indirect branch reports
        // only the address it landed on, which says nothing about which block
        // computed it; without the predecessors there is no way to tell a bad
        // GOT read from a bad emitted branch.
        impl->trail[impl->trail_pos++ & (Impl::kTrail - 1)] = impl->ctx.pc;

        RecompBlockFn block = impl->lookup(impl->ctx.pc);
        // Test hook: forces every lookup past the Nth to miss, so the JIT
        // fallback below can be exercised on a title that would otherwise never
        // hit a gap. Unset in normal runs.
        {
            static const char* const force_miss = std::getenv("SUYU_RECOMP_FORCE_MISS_AFTER");
            static std::atomic<int> blocks_run{0};
            if (force_miss) {
                const int n = blocks_run.fetch_add(1, std::memory_order_relaxed);
                const int limit = std::atoi(force_miss);
                // Name the blocks either side of the cutoff. Bisecting on the
                // cutoff tells you which index first breaks the run; this turns
                // that index into the actual guest address to look at.
                if (n >= limit - 4 && n <= limit + 4) {
                    LOG_ERROR(Core_ARM, "recomp: block #{} pc={:#x}", n, impl->ctx.pc);
                }
                if (n >= limit) {
                    block = nullptr;
                }
            }
        }
        // A miss is now recoverable, so it can happen many times per second;
        // the full diagnostic dump is kept for the first few only, where it is
        // still useful for finding which indirect call went uncovered.
        static std::atomic<int> miss_count{0};
        const int miss_index = block ? 0 : miss_count.fetch_add(1, std::memory_order_relaxed);
        if (!block && miss_index < 8) {
            std::string trail;
            const size_t count = std::min<size_t>(impl->trail_pos, Impl::kTrail);
            for (size_t i = 0; i < count; ++i) {
                const u64 p = impl->trail[(impl->trail_pos - count + i) & (Impl::kTrail - 1)];
                trail += fmt::format("{:#x} ", p);
            }
            LOG_ERROR(Core_ARM, "recomp PC trail (oldest first): {}", trail);
            LOG_ERROR(Core_ARM, "recomp regs x16={:#x} x17={:#x} x30={:#x} sp={:#x}",
                      impl->ctx.x[16], impl->ctx.x[17], impl->ctx.x[30], impl->ctx.x[31]);
            LOG_ERROR(Core_ARM, "recomp regs x0={:#x} x15={:#x} x18={:#x} x19={:#x}",
                      impl->ctx.x[0], impl->ctx.x[15], impl->ctx.x[18], impl->ctx.x[19]);
            {
                const auto& modules = impl->process_state->modules;
                const u64 mbase = modules.empty() ? 0 : modules.begin()->first;
                for (u64 seg : {0x0ULL, 0x2000ULL, 0x3000ULL}) {
                    std::string dump;
                    for (u64 i = 0; i < 0x40; i += 4) {
                        dump += fmt::format("{:08x} ", Impl::HostLoad(impl.get(), mbase + seg + i, 4));
                    }
                    LOG_ERROR(Core_ARM, "recomp mem mod+{:#x} (base {:#x}): {}", seg, mbase, dump);
                }
            }
        }
        if (!block) {
            // No recompiled block covers this address: an indirect branch into
            // code the static pass never reached. The guest's own instructions
            // are still mapped in guest memory, so hand the thread to a JIT and
            // keep going instead of returning PrefetchAbort - that halt reason
            // makes the kernel suspend the thread for a debugger that is not
            // attached, which is a permanent, silent black-screen hang.
            if (miss_index < 64) {
                LOG_ERROR(Core_ARM, "No recompiled block at PC {:#x}; falling back to JIT",
                          impl->ctx.pc);
            } else {
                LOG_DEBUG(Core_ARM, "No recompiled block at PC {:#x}; falling back to JIT",
                          impl->ctx.pc);
            }
            if (!EnterFallback(thread)) {
                LOG_CRITICAL(Core_ARM,
                             "recomp: no JIT fallback available at PC {:#x}; thread cannot "
                             "continue",
                             impl->ctx.pc);
                return HaltReason::PrefetchAbort;
            }
            return RunFallback(thread);
        }

        block(&impl->ctx);

        // The block stopped on an instruction the decoder has no translation
        // for, having parked the PC on that instruction. Running it on the JIT
        // instead keeps guest state exact: the alternative the generated code
        // used to take - step over it and zero x0 - silently produced a
        // plausible-looking null that only surfaced as a crash much later, in
        // whatever code eventually dereferenced it.
        if (impl->ctx.halted == kHaltUnhandled) {
            impl->ctx.halted = 0;
            static std::atomic<int> unhandled_count{0};
            if (unhandled_count.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_WARNING(Core_ARM, "recomp: unimplemented opcode at {:#x}; running on JIT",
                            impl->ctx.pc);
            }
            if (!EnterFallback(thread)) {
                LOG_CRITICAL(Core_ARM, "recomp: unimplemented opcode at {:#x} and no JIT fallback",
                             impl->ctx.pc);
                return HaltReason::PrefetchAbort;
            }
            return RunFallback(thread);
        }

        if (impl->ctx.pending_svc != kNoPendingSvc) {
            // Log every SVC call from rtld (first few hundred only to avoid spam)
            LOG_TRACE(Core_ARM, "recomp SVC {} at pc={:#x} x0={:#x} x1={:#x} x2={:#x} x3={:#x}",
                      impl->ctx.pending_svc, impl->ctx.pc, impl->ctx.x[0], impl->ctx.x[1],
                      impl->ctx.x[2], impl->ctx.x[3]);
            return HaltReason::SupervisorCall;
        }
    }

    return HaltReason::BreakLoop;
}

HaltReason ArmRecomp::StepThread(Kernel::KThread* thread) {
    // Block granularity is the finest this backend can step: recompiled blocks
    // are straight-line C with no per-instruction re-entry point.
    if (!impl->lookup) {
        return HaltReason::BreakLoop;
    }
    const auto initialization = impl->EnsureProcessInitialized(thread);
    if (initialization == ArmRecompProcessState::Initialization::Fatal) {
        return HaltReason::PrefetchAbort;
    }
    if (initialization == ArmRecompProcessState::Initialization::FallbackOnly ||
        impl->in_fallback) {
        if (!EnterFallback(thread)) {
            return HaltReason::PrefetchAbort;
        }
        return RunFallback(thread, true);
    }
    if (initialization != ArmRecompProcessState::Initialization::Ready) {
        return HaltReason::PrefetchAbort;
    }
    const RecompBlockFn block = impl->lookup(impl->ctx.pc);
    if (!block) {
        if (!EnterFallback(thread)) {
            return HaltReason::PrefetchAbort;
        }
        return RunFallback(thread, true);
    }
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.halted = 0;
    block(&impl->ctx);
    if (impl->ctx.halted == kHaltUnhandled) {
        impl->ctx.halted = 0;
        if (!EnterFallback(thread)) {
            return HaltReason::PrefetchAbort;
        }
        return RunFallback(thread, true);
    }
    if (impl->ctx.pending_svc != kNoPendingSvc) {
        return HaltReason::SupervisorCall;
    }
    return HaltReason::StepThread;
}

void ArmRecomp::ClearInstructionCache() {
    // Statically recompiled code is fixed at build time; there is no
    // translation cache to invalidate. Self-modifying guest code is
    // consequently unsupported by this backend by construction.
}

void ArmRecomp::InvalidateCacheRange(u64 addr, std::size_t size) {
    // See ClearInstructionCache.
}

void ArmRecomp::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    std::memset(&ctx, 0, sizeof(ctx));
    for (size_t i = 0; i < 29; ++i) {
        ctx.r[i] = impl->ctx.x[i];
    }
    ctx.fp = impl->ctx.x[29];
    ctx.lr = impl->ctx.x[30];
    ctx.sp = impl->ctx.x[31];
    ctx.pc = impl->ctx.pc;
    ctx.pstate = (static_cast<u32>(impl->ctx.n) << 31) |
                 (static_cast<u32>(impl->ctx.z) << 30) |
                 (static_cast<u32>(impl->ctx.c) << 29) |
                 (static_cast<u32>(impl->ctx.v) << 28);
    // u128 here is a pair of 64-bit halves, matching how the generated
    // context stores each vector register.
    for (size_t i = 0; i < 32; ++i) {
        ctx.v[i][0] = impl->ctx.vreg[i][0];
        ctx.v[i][1] = impl->ctx.vreg[i][1];
    }
    ctx.fpcr = impl->fpcr;
    ctx.fpsr = impl->fpsr;
    ctx.tpidr = impl->ctx.tpidr_el0;
}

void ArmRecomp::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    for (size_t i = 0; i < 29; ++i) {
        impl->ctx.x[i] = ctx.r[i];
    }
    impl->ctx.x[29] = ctx.fp;
    impl->ctx.x[30] = ctx.lr;
    impl->ctx.x[31] = ctx.sp;
    impl->ctx.pc = ctx.pc;
    impl->ctx.n = static_cast<u8>((ctx.pstate >> 31) & 1);
    impl->ctx.z = static_cast<u8>((ctx.pstate >> 30) & 1);
    impl->ctx.c = static_cast<u8>((ctx.pstate >> 29) & 1);
    impl->ctx.v = static_cast<u8>((ctx.pstate >> 28) & 1);
    for (size_t i = 0; i < 32; ++i) {
        impl->ctx.vreg[i][0] = ctx.v[i][0];
        impl->ctx.vreg[i][1] = ctx.v[i][1];
    }
    impl->fpcr = ctx.fpcr;
    impl->fpsr = ctx.fpsr;
    // Only the guest-owned thread pointer travels in ThreadContext. The
    // read-only one is republished separately by PhysicalCore::LoadContext
    // on every switch-in, so writing it from here would overwrite the
    // kernel's TLS pointer with the guest's.
    impl->ctx.tpidr_el0 = ctx.tpidr;
}

void ArmRecomp::SetTpidrroEl0(u64 value) {
    // The emitted MRS handler for TPIDRRO_EL0 reads this out of the guest
    // context, so it has to land there and nowhere else: writing it into
    // tpidr_el0 (as this used to) destroyed the guest's own thread pointer on
    // every context switch and made the guest emit its IPC header outside the
    // TLS region the kernel parses it from.
    impl->ctx.tpidrro_el0 = value;
}

void ArmRecomp::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; ++i) {
        args[i] = impl->ctx.x[i];
    }
}

void ArmRecomp::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; ++i) {
        impl->ctx.x[i] = args[i];
    }
}

u32 ArmRecomp::GetSvcNumber() const {
    return static_cast<u32>(impl->ctx.pending_svc);
}

void ArmRecomp::SignalInterrupt(Kernel::KThread* thread) {
    impl->interrupted.store(true, std::memory_order_release);
    // While the JIT is running this thread it is the one that has to be woken;
    // the flag above is only read by the recompiled dispatch loop.
    if (auto* fallback = impl->fallback_signal_target.load(std::memory_order_acquire)) {
        fallback->SignalInterrupt(thread);
    }
}

const Kernel::DebugWatchpoint* ArmRecomp::HaltedWatchpoint() const {
    if (const auto* fallback = impl->fallback_signal_target.load(std::memory_order_acquire)) {
        return static_cast<const ArmInterface*>(fallback)->HaltedWatchpoint();
    }
    return nullptr;
}

void ArmRecomp::RewindBreakpointInstruction() {
    if (auto* fallback = impl->fallback_signal_target.load(std::memory_order_acquire)) {
        static_cast<ArmInterface*>(fallback)->RewindBreakpointInstruction();
        Kernel::Svc::ThreadContext ctx{};
        fallback->GetContext(ctx);
        SetContext(ctx);
    }
}

} // namespace Core
