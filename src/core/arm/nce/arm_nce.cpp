// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <cinttypes>
#include <cstdio>
#include <memory>

#include "common/signal_chain.h"
#include "core/arm/nce/arm_nce.h"
#include "core/arm/nce/interpreter_visitor.h"
#include "core/arm/nce/patcher.h"
#include "core/core.h"
#include "core/memory.h"

#include "core/hle/kernel/k_process.h"

#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Core {

namespace {

struct sigaction g_orig_bus_action;
struct sigaction g_orig_segv_action;
struct sigaction g_orig_ill_action;

// Diagnostics: guest faults that NCE cannot handle are logged (the first few), with the host pc,
// the faulting address and the instruction, so a native-execution failure shows its cause.
std::atomic<u32> g_failed_guest_faults{0};
constexpr u32 MaxLoggedGuestFaults = 16;

// A guest instruction the host refuses (SIGILL) kills the process; say where it was first.
void GuestIllegalInstructionHandler(int sig, siginfo_t* info, void* raw_context) {
    u64 tpidr = 0;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    if (tpidr != 0 && *reinterpret_cast<const u32*>(tpidr + TpidrEl0TlsMagic) == TlsMagic) {
        char buffer[256];
        const u32 insn = *reinterpret_cast<const u32*>(host_ctx.pc);
        const int length = std::snprintf(
            buffer, sizeof(buffer),
            "NCE: guest illegal instruction at pc=%#llx insn=%#010x lr=%#llx sp=%#llx\n",
            static_cast<unsigned long long>(host_ctx.pc), insn,
            static_cast<unsigned long long>(host_ctx.regs[30]),
            static_cast<unsigned long long>(host_ctx.sp));
        if (length > 0) {
            [[maybe_unused]] const auto written = write(STDERR_FILENO, buffer, static_cast<size_t>(length));
        }
    }
    // Hand it to the previous handler (or the default action) by re-raising with it restored.
    sigaction(SIGILL, &g_orig_ill_action, nullptr);
}

// Verify assembly offsets.
using NativeExecutionParameters = Kernel::KThread::NativeExecutionParameters;
static_assert(offsetof(NativeExecutionParameters, native_context) == TpidrEl0NativeContext);
static_assert(offsetof(NativeExecutionParameters, lock) == TpidrEl0Lock);
static_assert(offsetof(NativeExecutionParameters, magic) == TpidrEl0TlsMagic);

fpsimd_context* GetFloatingPointState(mcontext_t& host_ctx) {
    _aarch64_ctx* header = reinterpret_cast<_aarch64_ctx*>(&host_ctx.__reserved);
    while (header->magic != FPSIMD_MAGIC) {
        header = reinterpret_cast<_aarch64_ctx*>(reinterpret_cast<char*>(header) + header->size);
    }
    return reinterpret_cast<fpsimd_context*>(header);
}

using namespace Common::Literals;
constexpr u32 StackSize = 128_KiB;
constexpr size_t SignalStackSize = 1_MiB;

} // namespace

void* ArmNce::RestoreGuestContext(void* raw_context) {
    // Retrieve the host context.
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;

    // Thread-local parameters will be located in x9.
    auto* tpidr = reinterpret_cast<NativeExecutionParameters*>(host_ctx.regs[9]);
    auto* guest_ctx = static_cast<GuestContext*>(tpidr->native_context);

    // Retrieve the host floating point state.
    auto* fpctx = GetFloatingPointState(host_ctx);

    // Save host callee-saved registers.
    std::memcpy(guest_ctx->host_ctx.host_saved_vregs.data(), &fpctx->vregs[8],
                sizeof(guest_ctx->host_ctx.host_saved_vregs));
    std::memcpy(guest_ctx->host_ctx.host_saved_regs.data(), &host_ctx.regs[19],
                sizeof(guest_ctx->host_ctx.host_saved_regs));

    // Save stack pointer.
    guest_ctx->host_ctx.host_sp = host_ctx.sp;

    // Restore all guest state except tpidr_el0.
    host_ctx.sp = guest_ctx->sp;
    host_ctx.pc = guest_ctx->pc;
    host_ctx.pstate = guest_ctx->pstate;
    fpctx->fpcr = guest_ctx->fpcr;
    fpctx->fpsr = guest_ctx->fpsr;
    std::memcpy(host_ctx.regs, guest_ctx->cpu_registers.data(), sizeof(host_ctx.regs));
    std::memcpy(fpctx->vregs, guest_ctx->vector_registers.data(), sizeof(fpctx->vregs));

    // Return the new thread-local storage pointer.
    return tpidr;
}

void ArmNce::SaveGuestContext(GuestContext* guest_ctx, void* raw_context) {
    // Retrieve the host context.
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;

    // Retrieve the host floating point state.
    auto* fpctx = GetFloatingPointState(host_ctx);

    // Save all guest registers except tpidr_el0.
    std::memcpy(guest_ctx->cpu_registers.data(), host_ctx.regs, sizeof(host_ctx.regs));
    std::memcpy(guest_ctx->vector_registers.data(), fpctx->vregs, sizeof(fpctx->vregs));
    guest_ctx->fpsr = fpctx->fpsr;
    guest_ctx->fpcr = fpctx->fpcr;
    guest_ctx->pstate = static_cast<u32>(host_ctx.pstate);
    guest_ctx->pc = host_ctx.pc;
    guest_ctx->sp = host_ctx.sp;

    // Restore stack pointer.
    host_ctx.sp = guest_ctx->host_ctx.host_sp;

    // Restore host callee-saved registers.
    std::memcpy(&host_ctx.regs[19], guest_ctx->host_ctx.host_saved_regs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_regs));
    std::memcpy(&fpctx->vregs[8], guest_ctx->host_ctx.host_saved_vregs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_vregs));

    // Return from the call on exit by setting pc to x30.
    host_ctx.pc = guest_ctx->host_ctx.host_saved_regs[11];

    // Clear esr_el1 and return it.
    host_ctx.regs[0] = guest_ctx->esr_el1.exchange(0);
}

bool ArmNce::HandleFailedGuestFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    auto* info = static_cast<siginfo_t*>(raw_info);

    // We can't handle the access, so determine why we crashed.
    const bool is_prefetch_abort = host_ctx.pc == reinterpret_cast<u64>(info->si_addr);

    if (const u32 n = g_failed_guest_faults.fetch_add(1, std::memory_order_relaxed);
        n < MaxLoggedGuestFaults) {
        const u32 insn = is_prefetch_abort ? 0 : *reinterpret_cast<const u32*>(host_ctx.pc);
        LOG_ERROR(Core_ARM,
                  "NCE: unhandled guest {} #{} at pc={:#x} addr={:#x} insn={:#010x} lr={:#x} "
                  "sp={:#x} x0={:#x} x1={:#x} x8={:#x} x19={:#x}",
                  is_prefetch_abort ? "prefetch abort" : "data abort", n + 1, host_ctx.pc,
                  reinterpret_cast<u64>(info->si_addr), insn, host_ctx.regs[30], host_ctx.sp,
                  host_ctx.regs[0], host_ctx.regs[1], host_ctx.regs[8], host_ctx.regs[19]);
    }

    // For data aborts, skip the instruction and return to guest code.
    // This will allow games to continue in many scenarios where they would otherwise crash.
    if (!is_prefetch_abort) {
        host_ctx.pc += 4;
        return true;
    }

    // This is a prefetch abort.
    guest_ctx->esr_el1.fetch_or(static_cast<u64>(HaltReason::PrefetchAbort));

    // Forcibly mark the context as locked. We are still running.
    // We may race with SignalInterrupt here:
    // - If we lose the race, then SignalInterrupt will send us a signal we are masking,
    //   and it will do nothing when it is unmasked, as we have already left guest code.
    // - If we win the race, then SignalInterrupt will wait for us to unlock first.
    auto& thread_params = guest_ctx->parent->m_running_thread->GetNativeExecutionParameters();
    thread_params.lock.store(SpinLockLocked);

    // Return to host.
    SaveGuestContext(guest_ctx, raw_context);
    return false;
}

bool ArmNce::HandleGuestAlignmentFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    auto* fpctx = GetFloatingPointState(host_ctx);
    auto& memory = guest_ctx->parent->m_running_thread->GetOwnerProcess()->GetMemory();

    // Match and execute an instruction.
    auto next_pc = MatchAndExecuteOneInstruction(memory, &host_ctx, fpctx);
    if (next_pc) {
        host_ctx.pc = *next_pc;
        return true;
    }

    // We couldn't handle the access.
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

namespace {
// ESR of the fault, from the kernel's esr_context record (absent for some faults).
std::optional<u64> GetFaultEsr(mcontext_t& host_ctx) {
    constexpr u32 EsrMagic = 0x45535201;
    auto* const base = reinterpret_cast<char*>(&host_ctx.__reserved);
    size_t offset = 0;
    while (offset + sizeof(_aarch64_ctx) <= sizeof(host_ctx.__reserved)) {
        auto* const header = reinterpret_cast<_aarch64_ctx*>(base + offset);
        if (header->magic == 0 || header->size == 0) {
            break;
        }
        if (header->magic == EsrMagic) {
            u64 esr;
            std::memcpy(&esr, base + offset + sizeof(_aarch64_ctx), sizeof(esr));
            return esr;
        }
        offset += header->size;
    }
    return std::nullopt;
}

// NCE access-fault statistics, logged every 5 s while faults happen.
struct NceFaultStats {
    std::atomic<u64> reads{}, writes{}, unknown{}, invalidated{}, emulated{}, emulate_failed{},
        sync{};
    std::atomic<s64> last_log{};
    std::atomic<u32> failed_logged{};

    void MaybeLog() {
        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        s64 last = last_log.load(std::memory_order_relaxed);
        if (last == 0) {
            last_log.compare_exchange_strong(last, now);
            return;
        }
        if (now - last < 5000 || !last_log.compare_exchange_strong(last, now)) {
            return;
        }
        LOG_INFO(Core_ARM,
                 "NCE faults (5 s): {} reads, {} writes, {} unknown; writes: {} invalidated, {} "
                 "emulated, {} not emulatable; {} synced with the GPU",
                 reads.exchange(0), writes.exchange(0), unknown.exchange(0),
                 invalidated.exchange(0), emulated.exchange(0), emulate_failed.exchange(0),
                 sync.exchange(0));
    }
};
NceFaultStats g_fault_stats;

// The last page each guest thread had a store emulated on: it is still GPU-written while it
// faults, so later stores there skip the cache lookup (and its mutexes).
thread_local u64 t_last_emulated_page = ~0ULL;

bool NceEmulateGpuWrites() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NCE_EMULATE_GPU_WRITES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}
} // namespace

bool ArmNce::HandleGuestAccessFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto* info = static_cast<siginfo_t*>(raw_info);
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;

    // Try to handle an invalid access.
    // TODO: handle accesses which split a page?
    const Common::ProcessAddress addr =
        (reinterpret_cast<u64>(info->si_addr) & ~Memory::YUZU_PAGEMASK);
    auto& memory = guest_ctx->parent->m_running_thread->GetOwnerProcess()->GetMemory();

    // A guest store to a page the GPU wrote used to wait here for the GPU thread to download the
    // page (a full GPU sync per fault, every frame: ~1 fps after loading a save). Dynarmic instead
    // collects such writes and the GPU thread applies them before its next draw. Do the same:
    // emulate the store through Memory::WriteBlock (which collects it) and keep the page trapped.
    const auto esr = GetFaultEsr(host_ctx);
    // Data abort from a lower EL (EC 0x24), not a cache maintenance op (CM, bit 8).
    const bool is_data_abort = esr && ((*esr >> 26) & 0x3f) == 0x24 && ((*esr >> 8) & 1) == 0;
    const bool is_write = is_data_abort && ((*esr >> 6) & 1) != 0;
    if (!is_data_abort) {
        g_fault_stats.unknown.fetch_add(1, std::memory_order_relaxed);
    } else if (!is_write) {
        g_fault_stats.reads.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_fault_stats.writes.fetch_add(1, std::memory_order_relaxed);
    }
    if (is_write && NceEmulateGpuWrites()) {
        const u64 page = GetInteger(addr) >> Memory::YUZU_PAGEBITS;
        auto result = Memory::Memory::NceWriteFault::NeedsEmulation;
        if (page != t_last_emulated_page) {
            result = memory.InvalidateNCEForWrite(addr, Memory::YUZU_PAGESIZE);
        }
        if (result == Memory::Memory::NceWriteFault::Invalidated) {
            g_fault_stats.invalidated.fetch_add(1, std::memory_order_relaxed);
            g_fault_stats.MaybeLog();
            return true;
        }
        if (result == Memory::Memory::NceWriteFault::NeedsEmulation) {
            auto* fpctx = GetFloatingPointState(host_ctx);
            if (const auto next_pc = MatchAndExecuteOneInstruction(memory, &host_ctx, fpctx)) {
                host_ctx.pc = *next_pc;
                t_last_emulated_page = page;
                g_fault_stats.emulated.fetch_add(1, std::memory_order_relaxed);
                g_fault_stats.MaybeLog();
                return true;
            }
            t_last_emulated_page = ~0ULL;
            g_fault_stats.emulate_failed.fetch_add(1, std::memory_order_relaxed);
            if (g_fault_stats.failed_logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_WARNING(Core_ARM,
                            "NCE: store to a GPU-written page not emulated, syncing: pc={:#x} "
                            "insn={:#010x} addr={:#x}",
                            host_ctx.pc, memory.Read32(host_ctx.pc),
                            reinterpret_cast<u64>(info->si_addr));
            }
        }
        // Unmapped, NotRasterizer or not emulatable: the original path below.
    }

    if (memory.InvalidateNCE(addr, Memory::YUZU_PAGESIZE)) {
        if (is_write || !is_data_abort) {
            g_fault_stats.sync.fetch_add(1, std::memory_order_relaxed);
        }
        g_fault_stats.MaybeLog();
        // We handled the access successfully and are returning to guest code.
        return true;
    }

    // We couldn't handle the access.
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

namespace {
// A fault in host code is not NCE's: hand it to whoever had the signal before NCE. That is often
// the default action (SIG_DFL), whose sa_sigaction is null: calling it jumped to address 0 and hid
// where the fault was. Say where it was, put the previous action back and return, so the faulting
// instruction runs again under it (a core dump then shows the real frame).
void ChainHostFault(int sig, const struct sigaction& orig, void* raw_info, void* raw_context) {
    auto* info = static_cast<siginfo_t*>(raw_info);
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    {
        char name[17]{};
        prctl(PR_GET_NAME, name, 0, 0, 0);
        char buffer[320];
        const int length = std::snprintf(
            buffer, sizeof(buffer),
            "NCE: host %s in thread %ld (%s) at pc=%#llx addr=%#llx lr=%#llx sp=%#llx\n",
            sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", static_cast<long>(syscall(SYS_gettid)), name,
            static_cast<unsigned long long>(host_ctx.pc),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info->si_addr)),
            static_cast<unsigned long long>(host_ctx.regs[30]),
            static_cast<unsigned long long>(host_ctx.sp));
        if (length > 0) {
            [[maybe_unused]] const auto written =
                write(STDERR_FILENO, buffer, static_cast<size_t>(length));
        }
    }
    if ((orig.sa_flags & SA_SIGINFO) != 0 && orig.sa_sigaction != nullptr) {
        orig.sa_sigaction(sig, info, raw_context);
        return;
    }
    if (orig.sa_handler == SIG_IGN) {
        return;
    }
    if (orig.sa_handler != SIG_DFL && orig.sa_handler != nullptr) {
        orig.sa_handler(sig);
        return;
    }
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    Common::SigAction(sig, &dfl, nullptr);
}
} // namespace

void ArmNce::HandleHostAlignmentFault(int sig, void* raw_info, void* raw_context) {
    ChainHostFault(sig, g_orig_bus_action, raw_info, raw_context);
}

void ArmNce::HandleHostAccessFault(int sig, void* raw_info, void* raw_context) {
    ChainHostFault(sig, g_orig_segv_action, raw_info, raw_context);
}

void ArmNce::LockThread(Kernel::KThread* thread) {
    auto* thread_params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(thread_params);
}

void ArmNce::UnlockThread(Kernel::KThread* thread) {
    auto* thread_params = &thread->GetNativeExecutionParameters();
    m_guest_ctx.tpidr_el0 = thread_params->tpidr_el0;
    m_guest_ctx.tpidrro_el0 = thread_params->tpidrro_el0;
    thread_params->native_context = nullptr;
    UnlockThreadParameters(thread_params);
}

HaltReason ArmNce::RunThread(Kernel::KThread* thread) {
    // Check if we're already interrupted.
    // If we are, we can just return immediately.
    HaltReason hr = static_cast<HaltReason>(m_guest_ctx.esr_el1.exchange(0));
    if (True(hr)) {
        return hr;
    }

    // Pre-fetch thread context data to improve cache locality
    auto* thread_params = &thread->GetNativeExecutionParameters();
    auto* process = thread->GetOwnerProcess();

    // Move non-critical operations outside the locked section
    const u64 tpidr_el0_cache = m_guest_ctx.tpidr_el0;
    const u64 tpidrro_el0_cache = m_guest_ctx.tpidrro_el0;

    // Critical section begins - minimize operations here
    m_running_thread = thread;
    m_guest_ctx.parent = this;
    thread_params->native_context = &m_guest_ctx;
    thread_params->tpidr_el0 = tpidr_el0_cache;
    thread_params->tpidrro_el0 = tpidrro_el0_cache;

    // Memory barrier to ensure visibility of changes
    std::atomic_thread_fence(std::memory_order_release);
    thread_params->is_running = true;

    // TODO: finding and creating the post handler needs to be locked
    // to deal with dynamic loading of NROs.
    const auto& post_handlers = process->GetPostHandlers();
    if (auto it = post_handlers.find(m_guest_ctx.pc); it != post_handlers.end()) {
        hr = ReturnToRunCodeByTrampoline(thread_params, &m_guest_ctx, it->second);
    } else {
        hr = ReturnToRunCodeByExceptionLevelChange(m_thread_id, thread_params);  // Android: Use "process handle SIGUSR2 -n true -p true -s false" (and SIGURG) in LLDB when debugging
    }

    // Critical section for thread cleanup
    std::atomic_thread_fence(std::memory_order_acquire);

    // Cache values before releasing thread
    const u64 final_tpidr_el0 = thread_params->tpidr_el0;

    // Minimize critical section
    thread_params->is_running = false;
    thread_params->native_context = nullptr;
    m_running_thread = nullptr;

    // Non-critical updates can happen after releasing the thread
    m_guest_ctx.tpidr_el0 = final_tpidr_el0;

    // Return the halt reason.
    return hr;
}

HaltReason ArmNce::StepThread(Kernel::KThread* thread) {
    return HaltReason::StepThread;
}

u32 ArmNce::GetSvcNumber() const {
    return m_guest_ctx.svc;
}

void ArmNce::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; i++) {
        args[i] = m_guest_ctx.cpu_registers[i];
    }
}

void ArmNce::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; i++) {
        m_guest_ctx.cpu_registers[i] = args[i];
    }
}

ArmNce::ArmNce(System& system, bool uses_wall_clock, std::size_t core_index)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_core_index{core_index} {
    m_guest_ctx.system = &m_system;
}

ArmNce::~ArmNce() = default;

void ArmNce::Initialize() {
    if (m_thread_id == -1) {
        m_thread_id = gettid();
    }

    // Configure signal stack.
    // Guest faults run the memory invalidation (rasterizer and buffer cache work included) on
    // this stack, so it is larger than upstream's 128 KiB and has a guard page below it: an
    // overflow faults there instead of silently overwriting the heap. It is never freed, because
    // the thread may still take signals on it after this object is gone.
    if (!m_stack) {
        const long page = sysconf(_SC_PAGESIZE);
        const size_t guard = static_cast<size_t>(page > 0 ? page : 4096);
        void* const mem = mmap(nullptr, SignalStackSize + guard, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem != MAP_FAILED) {
            mprotect(mem, guard, PROT_NONE);
            m_stack = static_cast<u8*>(mem) + guard;
        } else {
            m_stack = new u8[SignalStackSize];
        }

        stack_t ss{};
        ss.ss_sp = m_stack;
        ss.ss_size = SignalStackSize;
        sigaltstack(&ss, nullptr);
    }

    // Set up signals.
    static std::once_flag flag;
    std::call_once(flag, [] {
        using HandlerType = decltype(sigaction::sa_sigaction);

        sigset_t signal_mask;
        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, ReturnToRunCodeByExceptionLevelChangeSignal);
        sigaddset(&signal_mask, BreakFromRunCodeSignal);
        sigaddset(&signal_mask, GuestAlignmentFaultSignal);
        sigaddset(&signal_mask, GuestAccessFaultSignal);

        struct sigaction return_to_run_code_action {};
        return_to_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        return_to_run_code_action.sa_sigaction = reinterpret_cast<HandlerType>(
            &ArmNce::ReturnToRunCodeByExceptionLevelChangeSignalHandler);
        return_to_run_code_action.sa_mask = signal_mask;
        Common::SigAction(ReturnToRunCodeByExceptionLevelChangeSignal, &return_to_run_code_action,
                          nullptr);

        struct sigaction break_from_run_code_action {};
        break_from_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        break_from_run_code_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::BreakFromRunCodeSignalHandler);
        break_from_run_code_action.sa_mask = signal_mask;
        Common::SigAction(BreakFromRunCodeSignal, &break_from_run_code_action, nullptr);

        struct sigaction alignment_fault_action {};
        alignment_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        alignment_fault_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::GuestAlignmentFaultSignalHandler);
        alignment_fault_action.sa_mask = signal_mask;
        Common::SigAction(GuestAlignmentFaultSignal, &alignment_fault_action, &g_orig_bus_action);

        struct sigaction access_fault_action {};
        access_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        access_fault_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::GuestAccessFaultSignalHandler);
        access_fault_action.sa_mask = signal_mask;
        Common::SigAction(GuestAccessFaultSignal, &access_fault_action, &g_orig_segv_action);

        struct sigaction illegal_action {};
        illegal_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        illegal_action.sa_sigaction = &GuestIllegalInstructionHandler;
        sigemptyset(&illegal_action.sa_mask);
        sigaction(SIGILL, &illegal_action, &g_orig_ill_action);
    });
}

void ArmNce::SetTpidrroEl0(u64 value) {
    m_guest_ctx.tpidrro_el0 = value;
}

void ArmNce::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    for (size_t i = 0; i < 29; i++) {
        ctx.r[i] = m_guest_ctx.cpu_registers[i];
    }
    ctx.fp = m_guest_ctx.cpu_registers[29];
    ctx.lr = m_guest_ctx.cpu_registers[30];
    ctx.sp = m_guest_ctx.sp;
    ctx.pc = m_guest_ctx.pc;
    ctx.pstate = m_guest_ctx.pstate;
    ctx.v = m_guest_ctx.vector_registers;
    ctx.fpcr = m_guest_ctx.fpcr;
    ctx.fpsr = m_guest_ctx.fpsr;
    ctx.tpidr = m_guest_ctx.tpidr_el0;
}

void ArmNce::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    for (size_t i = 0; i < 29; i++) {
        m_guest_ctx.cpu_registers[i] = ctx.r[i];
    }
    m_guest_ctx.cpu_registers[29] = ctx.fp;
    m_guest_ctx.cpu_registers[30] = ctx.lr;
    m_guest_ctx.sp = ctx.sp;
    m_guest_ctx.pc = ctx.pc;
    m_guest_ctx.pstate = ctx.pstate;
    m_guest_ctx.vector_registers = ctx.v;
    m_guest_ctx.fpcr = ctx.fpcr;
    m_guest_ctx.fpsr = ctx.fpsr;
    m_guest_ctx.tpidr_el0 = ctx.tpidr;
}

void ArmNce::SignalInterrupt(Kernel::KThread* thread) {
    constexpr u64 break_loop = static_cast<u64>(HaltReason::BreakLoop);
    if ((m_guest_ctx.esr_el1.fetch_or(break_loop) & break_loop) != 0) return;

    auto* params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(params);

    // Ensure visibility of is_running after lock acquire
    std::atomic_thread_fence(std::memory_order_acquire);

    if (params->is_running) {
        // We should signal to the running thread.
        // The running thread will unlock the thread context.
        syscall(SYS_tkill, m_thread_id, BreakFromRunCodeSignal);
    } else {
        // If the thread is no longer running, we have nothing to do.
        UnlockThreadParameters(params);
    }
}

[[maybe_unused]] const std::size_t CACHE_PAGE_SIZE = 4096;

void ArmNce::ClearInstructionCache() {
#ifdef __aarch64__
    // Ensure all previous memory operations complete
    asm volatile("dsb ish\n"
                 "dsb ish\n"
                 "isb" ::: "memory");
#endif
}

void ArmNce::InvalidateCacheRange(u64 addr, std::size_t size) {
    this->ClearInstructionCache();
}

} // namespace Core
