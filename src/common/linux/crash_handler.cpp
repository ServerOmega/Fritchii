// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/linux/crash_handler.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <array>
#include <cerrno>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>

#include <execinfo.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/thread.h"

namespace Common::CrashHandler {
namespace {

constexpr std::array FatalSignals{SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr std::size_t AltStackSize = 128 * 1024;

struct sigaction g_previous[NSIG]{};
std::atomic<int> g_crashing_tid{0};
std::atomic_bool g_installed{false};

const char* SignalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "SIG?";
    }
}

/// A fixed buffer formatted without malloc or locale (snprintf is not async-signal-safe).
struct Line {
    char text[512];
    std::size_t size = 0;
    void Add(std::string_view s) {
        for (char c : s) {
            if (size < sizeof(text) - 1) text[size++] = c;
        }
    }
    void Hex(unsigned long long v) {
        char digits[2 + 16];
        int n = 0;
        do {
            digits[n++] = "0123456789abcdef"[v & 0xf];
            v >>= 4;
        } while (v != 0);
        Add("0x");
        while (n > 0) {
            const char c = digits[--n];
            Add(std::string_view{&c, 1});
        }
    }
    void Dec(long long v) {
        if (v < 0) {
            Add("-");
            v = -v;
        }
        char digits[24];
        int n = 0;
        do {
            digits[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0);
        while (n > 0) {
            const char c = digits[--n];
            Add(std::string_view{&c, 1});
        }
    }
};

void WriteAll(int fd, const char* data, std::size_t size) {
    while (fd >= 0 && size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
}

unsigned long long ContextPc(void* raw_context) {
    if (raw_context == nullptr) return 0;
    const auto* uc = static_cast<const ucontext_t*>(raw_context);
#if defined(__x86_64__)
    return static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return static_cast<unsigned long long>(uc->uc_mcontext.pc);
#else
    (void)uc;
    return 0;
#endif
}

void Die(int sig, const siginfo_t* info) {
    // Back to the default action. A fault the CPU raised (si_code > 0) re-runs the instruction on
    // return and dies of it; anything sent (abort(), kill) is raised again, delivered once this
    // handler returns and unblocks it. Either way the kernel sees the real signal and dumps core.
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    if (info == nullptr || info->si_code <= 0 || sig == SIGABRT) {
        raise(sig);
    }
}

void OnFatalSignal(int sig, siginfo_t* info, void* raw_context) {
    const int saved_errno = errno;
    const int tid = static_cast<int>(::syscall(SYS_gettid));
    int expected = 0;
    if (!g_crashing_tid.compare_exchange_strong(expected, tid)) {
        if (expected == tid) {
            // The report itself crashed: die without another one.
            Die(sig, info);
            return;
        }
        // Another thread is writing its report; let it finish and end the process.
        for (int i = 0; i < 50; ++i) ::usleep(100 * 1000);
        Die(sig, info);
        return;
    }

    const int log_fd = Common::Log::CrashFlush();

    char thread_name[17]{};
    prctl(PR_GET_NAME, thread_name, 0, 0, 0);

    Line line;
    line.Add("Host crash: ");
    line.Add(SignalName(sig));
    line.Add(" pc=");
    line.Hex(ContextPc(raw_context));
    line.Add(" addr=");
    line.Hex(info != nullptr ? reinterpret_cast<unsigned long long>(info->si_addr) : 0);
    line.Add(" code=");
    line.Dec(info != nullptr ? info->si_code : 0);
    line.Add(" thread=");
    line.Add(thread_name);
    line.Add(" tid=");
    line.Dec(tid);
    line.Add(" build=");
    line.Add(Common::g_scm_rev);
    line.Add(" (");
    line.Add(Common::g_build_version);
    line.Add(")\n");

    static constexpr std::string_view trace_head = "Host crash backtrace (module(+offset) [address]):\n";
    static constexpr std::string_view trace_tail =
        "Host crash: end of report; symbols: addr2line -e <unstripped lichii> -f -C <offset>\n";
    void* frames[64];
    const int depth = backtrace(frames, 64);
    for (const int fd : {log_fd, STDERR_FILENO}) {
        if (fd < 0) continue;
        WriteAll(fd, line.text, line.size);
        WriteAll(fd, trace_head.data(), trace_head.size());
        backtrace_symbols_fd(frames, depth, fd);
        WriteAll(fd, trace_tail.data(), trace_tail.size());
    }
    if (log_fd >= 0) ::fsync(log_fd);

    // A handler that was there before ours (a debugger helper, a crash reporter) gets its turn.
    const struct sigaction& previous = g_previous[sig];
    if ((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction != nullptr &&
        previous.sa_sigaction != &OnFatalSignal) {
        previous.sa_sigaction(sig, info, raw_context);
    } else if (!(previous.sa_flags & SA_SIGINFO) && previous.sa_handler != SIG_DFL &&
               previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
        previous.sa_handler(sig);
    }
    Die(sig, info);
    errno = saved_errno;
}

struct ThreadStack {
    void* memory = nullptr;
    ~ThreadStack() {
        if (memory == nullptr) return;
        stack_t current{};
        if (sigaltstack(nullptr, &current) == 0 && current.ss_sp == memory) {
            stack_t off{};
            off.ss_flags = SS_DISABLE;
            sigaltstack(&off, nullptr);
        }
        munmap(memory, AltStackSize);
    }
};
thread_local ThreadStack t_stack;

[[noreturn]] void Crash(std::string_view kind) {
    LOG_CRITICAL(Common, "EDEN_NX2_CRASH_TEST={}: crashing on purpose now", kind);
    if (kind == "bus") {
        // A real SIGBUS on x86 too: touching a mapped page past the end of an empty file.
        const int fd = memfd_create("nx2-crash-test", 0);
        auto* page = static_cast<volatile char*>(mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0));
        if (page != MAP_FAILED) {
            (void)page[0];
        }
        raise(SIGBUS);
    } else if (kind == "abort") {
        std::abort();
    } else if (kind == "ill") {
        __builtin_trap();
    } else if (kind == "fpe") {
        volatile int zero = 0;
        volatile int value = 1 / zero;
        (void)value;
        raise(SIGFPE);
    }
    volatile int* null_pointer = nullptr;
    *null_pointer = 0x4e58; // segv
    std::abort();
}

} // Anonymous namespace

void PrepareThread() {
    if (t_stack.memory != nullptr) return;
    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE)) {
        return; // Dynarmic and NCE bring their own.
    }
    void* memory = mmap(nullptr, AltStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                        -1, 0);
    if (memory == MAP_FAILED) return;
    stack_t stack{};
    stack.ss_sp = memory;
    stack.ss_size = AltStackSize;
    if (sigaltstack(&stack, nullptr) != 0) {
        munmap(memory, AltStackSize);
        return;
    }
    t_stack.memory = memory;
}

void Install() {
    if (g_installed.exchange(true)) return;
    if (const char* off = std::getenv("EDEN_NX2_CRASH_HANDLER"); off != nullptr && off[0] == '0') {
        return;
    }
    // backtrace() loads libgcc's unwinder (which allocates) on first use: do that here.
    void* warm[2];
    backtrace(warm, 2);
    PrepareThread();

    struct sigaction action{};
    action.sa_sigaction = &OnFatalSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGPROF); // the nx2 sampler's signal must not interrupt a report
    for (const int sig : FatalSignals) {
        sigaction(sig, &action, &g_previous[sig]);
    }
}

void StartTestFromEnvironment(const char* when) {
    const char* kind = std::getenv("EDEN_NX2_CRASH_TEST");
    if (kind == nullptr || kind[0] == '\0') return;
    const char* at = std::getenv("EDEN_NX2_CRASH_TEST_AT");
    if (std::string_view{at != nullptr && at[0] != '\0' ? at : "boot"} != when) return;
    double delay = 5.0;
    if (const char* text = std::getenv("EDEN_NX2_CRASH_TEST_DELAY"); text != nullptr) {
        delay = std::strtod(text, nullptr);
    }
    LOG_WARNING(Common, "EDEN_NX2_CRASH_TEST={}: crashing in {} s (after {})", kind, delay, when);
    std::thread{[kind = std::string{kind}, delay] {
        Common::SetCurrentThreadName("CrashTest");
        std::this_thread::sleep_for(std::chrono::duration<double>(delay));
        Crash(kind);
    }}.detach();
}

} // namespace Common::CrashHandler

#else

namespace Common::CrashHandler {
void Install() {}
void PrepareThread() {}
void StartTestFromEnvironment(const char*) {}
} // namespace Common::CrashHandler

#endif
