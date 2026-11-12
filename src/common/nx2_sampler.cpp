// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/nx2_sampler.h"
#include "common/jit_symbols.h"

#if defined(_WIN32)
#define NX2_SAMPLER_WINDOWS
#elif defined(__linux__) && defined(__GLIBC__) && (defined(__x86_64__) || defined(__aarch64__))
#define NX2_SAMPLER_LINUX
#endif

#if defined(NX2_SAMPLER_WINDOWS) || defined(NX2_SAMPLER_LINUX)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"

#ifdef NX2_SAMPLER_WINDOWS
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#else
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <cxxabi.h>
#include <elf.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace Nx2Sampler {
namespace {

constexpr size_t MaxDepth = 40;

struct Sample {
    unsigned thread;
    unsigned depth;
    std::array<u64, MaxDepth> frames;
};

double EnvSeconds(const char* name, double fallback) {
    const char* text = std::getenv(name);
    return text != nullptr ? std::strtod(text, nullptr) : fallback;
}

std::vector<std::string> SplitNames(const char* text) {
    std::vector<std::string> names;
    std::string current;
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == ',') {
            names.push_back(current);
            current.clear();
        } else {
            current += *c;
        }
    }
    if (!current.empty()) {
        names.push_back(current);
    }
    return names;
}

bool Matches(const std::string& thread_name, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        if (!pattern.empty() && pattern.back() == '*') {
            if (thread_name.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0) {
                return true;
            }
        } else if (thread_name == pattern) {
            return true;
        }
    }
    return false;
}

#ifdef NX2_SAMPLER_WINDOWS

struct Target {
    HANDLE handle;
    std::string name;
};

std::vector<Target> FindThreads(const std::vector<std::string>& patterns) {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleA("KernelBase.dll"), "GetThreadDescription"));
    std::vector<Target> targets;
    if (get_description == nullptr) {
        return targets;
    }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return targets;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ThreadID);
        if (handle == nullptr) {
            continue;
        }
        PWSTR wide{};
        std::string name;
        if (SUCCEEDED(get_description(handle, &wide)) && wide != nullptr) {
            char buffer[256]{};
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, sizeof(buffer) - 1, nullptr,
                                nullptr);
            name = buffer;
            LocalFree(wide);
        }
        if (Matches(name, patterns)) {
            targets.push_back({handle, name});
        } else {
            CloseHandle(handle);
        }
    }
    CloseHandle(snapshot);
    return targets;
}

void Close(const Target& target) {
    CloseHandle(target.handle);
}

// Walks a suspended thread's stack from its context. Nothing here may allocate: the thread
// could be paused holding the heap lock.
unsigned Walk(CONTEXT context, std::array<u64, MaxDepth>& frames) {
    unsigned depth = 0;
    while (depth < MaxDepth && context.Rip != 0) {
        frames[depth++] = context.Rip;
        DWORD64 image_base{};
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // A leaf function (no unwind data): its return address is on top of the stack. JIT
            // code has none either; past the first frame, stop there.
            if (depth != 1) {
                break;
            }
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        PVOID handler_data{};
        DWORD64 establisher_frame{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher_frame, nullptr);
    }
    return depth;
}

/// Pauses the thread for its call stack. False: nothing was recorded.
bool Capture(Target& target, Sample& sample) {
    if (SuspendThread(target.handle) == static_cast<DWORD>(-1)) {
        return false;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    const bool read = GetThreadContext(target.handle, &context) != 0;
    if (read) {
        sample.depth = Walk(context, sample.frames);
    }
    ResumeThread(target.handle);
    return read;
}

void Pause() {
    Sleep(1);
}

struct Symbolizer {
    using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
    using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);

    Symbolizer() {
        const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        if (dbghelp == nullptr) {
            return;
        }
        const auto set_options =
            reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions"));
        const auto initialize =
            reinterpret_cast<SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize"));
        from_address = reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
        if (set_options == nullptr || initialize == nullptr || from_address == nullptr) {
            from_address = nullptr;
            return;
        }
        set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (!initialize(GetCurrentProcess(), nullptr, TRUE)) {
            from_address = nullptr;
        }
    }

    const std::string& Name(u64 address) {
        const auto [it, is_new] = cache.try_emplace(address);
        if (!is_new) {
            return it->second;
        }
        if (from_address != nullptr) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 1024]{};
            auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 1024;
            DWORD64 displacement{};
            if (from_address(GetCurrentProcess(), address, &displacement, symbol)) {
                it->second.assign(symbol->Name, std::min<size_t>(symbol->NameLen, 160));
                return it->second;
            }
        }
        HMODULE module{};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module)) {
            char path[MAX_PATH] = "?";
            GetModuleFileNameA(module, path, sizeof(path));
            const char* base = std::strrchr(path, '\\');
            it->second = std::string{base != nullptr ? base + 1 : path} + "!?";
        } else if (auto block = Common::JitSymbols::Lookup(reinterpret_cast<const void*>(address))) {
            // JIT code is reused for other blocks after a cache clear: don't keep the name.
            jit_name = std::move(*block);
            cache.erase(it);
            return jit_name;
        } else {
            it->second = "(jit)";
        }
        return it->second;
    }

    /// The module holding the address and the address's offset in it ("?" and the address
    /// outside every module), for llvm-symbolizer --relative-address.
    std::pair<std::string, u64> Module(u64 address) {
        HMODULE module{};
        char module_path[MAX_PATH] = "?";
        u64 offset = address;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module)) {
            GetModuleFileNameA(module, module_path, sizeof(module_path));
            offset = address - reinterpret_cast<u64>(module);
        }
        const char* base = std::strrchr(module_path, '\\');
        return {base != nullptr ? base + 1 : module_path, offset};
    }

    SymFromAddrFn from_address{};
    std::unordered_map<u64, std::string> cache;
    std::string jit_name;
};

#else // NX2_SAMPLER_LINUX

struct Target {
    pid_t tid; ///< 0 once the thread stopped answering
    std::string name;
};

pid_t CurrentThreadId() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

std::vector<Target> FindThreads(const std::vector<std::string>& patterns) {
    // The kernel keeps 15 characters of a thread's name ("GPUFencingThrea").
    constexpr size_t MaxName = 15;
    std::vector<std::string> cut;
    for (const std::string& pattern : patterns) {
        const bool prefix = !pattern.empty() && pattern.back() == '*';
        std::string name = pattern.substr(0, pattern.size() - (prefix ? 1 : 0)).substr(0, MaxName);
        cut.push_back(prefix ? name + '*' : name);
    }
    std::vector<Target> targets;
    const pid_t self = CurrentThreadId();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/task", error}) {
        const pid_t tid =
            static_cast<pid_t>(std::strtol(entry.path().filename().c_str(), nullptr, 10));
        if (tid <= 0 || tid == self) {
            continue;
        }
        std::ifstream comm{entry.path() / "comm"};
        std::string name;
        std::getline(comm, name);
        if (Matches(name, cut)) {
            targets.push_back({tid, name});
        }
    }
    return targets;
}

void Close(const Target&) {}

// A thread records its own stack in the handler of a signal sent to it. One request at a time.
constexpr int SampleSignal = SIGPROF;

struct Request {
    std::atomic<pid_t> wanted{}; ///< The thread asked for its stack; 0: none
    std::atomic<pid_t> answered{};
    unsigned depth{};
    std::array<u64, MaxDepth> frames{};
    sem_t done;
};
Request g_request;

// Runs on the sampled thread, which may hold any lock (the heap's too): no allocation here.
// backtrace() unwinds without one once libgcc is loaded (InstallHandler does that), and
// without the loader's lock from glibc 2.35 on.
void OnSampleSignal(int, siginfo_t*, void* raw) {
    const int saved_errno = errno;
    const pid_t tid = CurrentThreadId();
    // Not asked: the answer to a request that timed out, or another SIGPROF.
    if (g_request.wanted.load(std::memory_order_acquire) != tid) {
        errno = saved_errno;
        return;
    }
    const auto* const context = static_cast<const ucontext_t*>(raw);
#if defined(__x86_64__)
    const u64 pc = static_cast<u64>(context->uc_mcontext.gregs[REG_RIP]);
#else
    const u64 pc = static_cast<u64>(context->uc_mcontext.pc);
#endif
    // This handler and the signal frame come first; the interrupted code starts at `pc`. In
    // JIT code (no unwind data) the walk ends there or never gets there.
    std::array<void*, MaxDepth + 8> stack;
    const int count = backtrace(stack.data(), static_cast<int>(stack.size()));
    int first = 0;
    while (first < count && reinterpret_cast<u64>(stack[first]) != pc) {
        ++first;
    }
    unsigned depth = 0;
    g_request.frames[depth++] = pc;
    for (int i = first + 1; i < count && depth < MaxDepth; ++i) {
        // Return addresses: the byte before is still in the call (a call that ends its function
        // returns into the next one).
        g_request.frames[depth++] = reinterpret_cast<u64>(stack[i]) - 1;
    }
    g_request.depth = depth;
    g_request.answered.store(tid, std::memory_order_release);
    g_request.wanted.store(0, std::memory_order_release);
    sem_post(&g_request.done);
    errno = saved_errno;
}

bool InstallHandler() {
    static const bool installed = [] {
        if (sem_init(&g_request.done, 0, 0) != 0) {
            return false;
        }
        // Loads libgcc's unwinder, which allocates, outside the handler.
        void* warm[2];
        backtrace(warm, 2);
        struct sigaction action{};
        action.sa_sigaction = OnSampleSignal;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        // Kept for the session: a late signal with the default action would end the process.
        return sigaction(SampleSignal, &action, nullptr) == 0;
    }();
    return installed;
}

/// Asks the thread for its call stack. False: nothing was recorded.
bool Capture(Target& target, Sample& sample) {
    if (target.tid == 0 || !InstallHandler()) {
        return false;
    }
    while (sem_trywait(&g_request.done) == 0) {
        // The answer to a request that timed out.
    }
    g_request.answered.store(0, std::memory_order_relaxed);
    g_request.wanted.store(target.tid, std::memory_order_release);
    if (syscall(SYS_tgkill, getpid(), target.tid, SampleSignal) != 0) {
        g_request.wanted.store(0, std::memory_order_release);
        target.tid = 0; // The thread ended
        return false;
    }
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 250'000'000;
    if (deadline.tv_nsec >= 1'000'000'000) {
        deadline.tv_nsec -= 1'000'000'000;
        ++deadline.tv_sec;
    }
    while (sem_timedwait(&g_request.done, &deadline) != 0) {
        if (errno == EINTR) {
            continue;
        }
        // No answer (the thread blocks the signal): leave it out from now on.
        g_request.wanted.store(0, std::memory_order_release);
        target.tid = 0;
        return false;
    }
    if (g_request.answered.load(std::memory_order_acquire) != target.tid) {
        return false;
    }
    sample.depth = g_request.depth;
    std::copy_n(g_request.frames.begin(), sample.depth, sample.frames.begin());
    return true;
}

void Pause() {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
}

struct Symbolizer {
    struct Function {
        u64 start; ///< Address in the file
        u64 size;
        u32 name; ///< Offset in the string table
    };

    struct Image {
        u64 begin; ///< Mapped range
        u64 end;
        u64 bias; ///< Mapped address - address in the file
        std::string path;
        std::string name;
        bool read{};
        std::vector<Function> functions; ///< Sorted by start
        const char* names{};             ///< String table, mapped for the session
    };

    static int AddImage(dl_phdr_info* info, size_t, void* data) {
        auto& images = static_cast<Symbolizer*>(data)->images;
        u64 begin = ~u64{};
        u64 end = 0;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const auto& header = info->dlpi_phdr[i];
            if (header.p_type == PT_LOAD) {
                const u64 start = static_cast<u64>(info->dlpi_addr) + header.p_vaddr;
                begin = (std::min)(begin, start);
                end = (std::max)(end, start + header.p_memsz);
            }
        }
        if (begin >= end || std::any_of(images.begin(), images.end(),
                                        [&](const Image& image) { return image.begin == begin; })) {
            return 0;
        }
        Image& image = images.emplace_back();
        image.begin = begin;
        image.end = end;
        image.bias = static_cast<u64>(info->dlpi_addr);
        image.path = info->dlpi_name != nullptr ? info->dlpi_name : "";
        if (image.path.empty()) {
            // The program itself
            std::error_code error;
            image.path = std::filesystem::read_symlink("/proc/self/exe", error).string();
        }
        const size_t slash = image.path.rfind('/');
        image.name = slash == std::string::npos ? image.path : image.path.substr(slash + 1);
        return 0;
    }

    Image* Find(u64 address) {
        const auto find = [&]() -> Image* {
            for (Image& image : images) {
                if (address >= image.begin && address < image.end) {
                    return &image;
                }
            }
            return nullptr;
        };
        Image* image = find();
        // JIT code is in no image: don't walk the loader's list for each of its addresses.
        if (const auto now = std::chrono::steady_clock::now();
            image == nullptr && now >= next_listing) {
            next_listing = now + std::chrono::seconds{1};
            dl_iterate_phdr(AddImage, this);
            image = find();
        }
        return image;
    }

    /// Function symbols of the file: .symtab, or the exported ones (.dynsym) once stripped.
    static void ReadFunctions(Image& image) {
        image.read = true;
        const int fd = open(image.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        struct stat status{};
        void* const map =
            fstat(fd, &status) == 0 && status.st_size > 0
                ? mmap(nullptr, static_cast<size_t>(status.st_size), PROT_READ, MAP_PRIVATE, fd, 0)
                : MAP_FAILED;
        close(fd);
        if (map == MAP_FAILED) {
            return;
        }
        const u64 size = static_cast<u64>(status.st_size);
        const auto* const bytes = static_cast<const char*>(map);
        const auto within = [size](u64 offset, u64 length) {
            return offset <= size && length <= size - offset;
        };
        const auto* const header = reinterpret_cast<const Elf64_Ehdr*>(bytes);
        if (size >= sizeof(Elf64_Ehdr) && std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 &&
            header->e_ident[EI_CLASS] == ELFCLASS64 && header->e_shentsize == sizeof(Elf64_Shdr) &&
            within(header->e_shoff, u64{header->e_shnum} * sizeof(Elf64_Shdr))) {
            const auto* const sections =
                reinterpret_cast<const Elf64_Shdr*>(bytes + header->e_shoff);
            const Elf64_Shdr* table = nullptr;
            for (unsigned i = 0; i < header->e_shnum; ++i) {
                if (sections[i].sh_type == SHT_SYMTAB) {
                    table = &sections[i];
                    break;
                }
                if (sections[i].sh_type == SHT_DYNSYM) {
                    table = &sections[i];
                }
            }
            const Elf64_Shdr* const strings = table != nullptr && table->sh_link < header->e_shnum
                                                  ? &sections[table->sh_link]
                                                  : nullptr;
            // The names must end inside the file.
            if (strings != nullptr && table->sh_entsize == sizeof(Elf64_Sym) &&
                within(table->sh_offset, table->sh_size) && strings->sh_size != 0 &&
                within(strings->sh_offset, strings->sh_size) &&
                bytes[strings->sh_offset + strings->sh_size - 1] == '\0') {
                const auto* const symbols =
                    reinterpret_cast<const Elf64_Sym*>(bytes + table->sh_offset);
                const u64 count = table->sh_size / sizeof(Elf64_Sym);
                for (u64 i = 0; i < count; ++i) {
                    const Elf64_Sym& symbol = symbols[i];
                    if (ELF64_ST_TYPE(symbol.st_info) == STT_FUNC && symbol.st_value != 0 &&
                        symbol.st_shndx != SHN_UNDEF && symbol.st_name < strings->sh_size) {
                        image.functions.push_back(
                            {symbol.st_value, symbol.st_size, static_cast<u32>(symbol.st_name)});
                    }
                }
                image.names = bytes + strings->sh_offset;
            }
        }
        if (image.functions.empty()) {
            munmap(map, static_cast<size_t>(size));
            image.names = nullptr;
            return;
        }
        std::sort(image.functions.begin(), image.functions.end(),
                  [](const Function& a, const Function& b) { return a.start < b.start; });
    }

    const std::string& Name(u64 address) {
        const auto [it, is_new] = cache.try_emplace(address);
        if (!is_new) {
            return it->second;
        }
        if (Image* const image = Find(address)) {
            if (!image->read) {
                ReadFunctions(*image);
            }
            const u64 in_file = address - image->bias;
            const auto next = std::upper_bound(
                image->functions.begin(), image->functions.end(), in_file,
                [](u64 value, const Function& function) { return value < function.start; });
            // Assembly routines carry no size: theirs runs to the next symbol.
            if (next != image->functions.begin() &&
                (std::prev(next)->size == 0 ||
                 in_file - std::prev(next)->start < std::prev(next)->size)) {
                const char* const mangled = image->names + std::prev(next)->name;
                int status = 0;
                char* const demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
                const char* const name = status == 0 && demangled != nullptr ? demangled : mangled;
                it->second.assign(name, std::min<size_t>(std::strlen(name), 160));
                std::free(demangled);
            } else {
                it->second = image->name + "!?";
            }
        } else if (auto block =
                       Common::JitSymbols::Lookup(reinterpret_cast<const void*>(address))) {
            // JIT code is reused for other blocks after a cache clear: don't keep the name.
            jit_name = std::move(*block);
            cache.erase(it);
            return jit_name;
        } else {
            it->second = "(jit)";
        }
        return it->second;
    }

    /// The file holding the address and the address in that file ("?" and the address outside
    /// every file), for addr2line -e or llvm-symbolizer --obj.
    std::pair<std::string, u64> Module(u64 address) {
        if (const Image* const image = Find(address)) {
            return {image->name, address - image->bias};
        }
        return {"?", address};
    }

    std::vector<Image> images;
    std::chrono::steady_clock::time_point next_listing{};
    std::unordered_map<u64, std::string> cache;
    std::string jit_name;
};

#endif

std::string Report(const std::vector<Target>& targets, const std::vector<Sample>& samples,
                   const char* path) {
    // SymInitialize fails on a second call without SymCleanup, so keep one symbolizer.
    static Symbolizer symbolizer;
    std::string text;
    for (unsigned thread = 0; thread < targets.size(); ++thread) {
        std::map<std::string, unsigned> self;
        std::map<std::string, unsigned> inclusive;
        std::map<std::string, unsigned> self_caller;
        unsigned total = 0;
        for (const Sample& sample : samples) {
            if (sample.thread != thread || sample.depth == 0) {
                continue;
            }
            ++total;
            const std::string top = symbolizer.Name(sample.frames[0]);
            ++self[top];
            if (sample.depth > 1) {
                ++self_caller[top + "  <-  " + symbolizer.Name(sample.frames[1])];
            }
            std::set<std::string> seen;
            for (unsigned i = 0; i < sample.depth; ++i) {
                const std::string& name = symbolizer.Name(sample.frames[i]);
                if (seen.insert(name).second) {
                    ++inclusive[name];
                }
            }
        }
        const auto print = [&](const char* title, const std::map<std::string, unsigned>& counts,
                               size_t limit) {
            std::vector<std::pair<unsigned, std::string>> sorted;
            for (const auto& [name, count] : counts) {
                sorted.emplace_back(count, name);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            text += fmt::format("-- {}\n", title);
            for (size_t i = 0; i < (std::min)(limit, sorted.size()); ++i) {
                text += fmt::format("{:6} {:5.1f}% {}\n", sorted[i].first,
                             100.0 * sorted[i].first / (std::max)(total, 1U),
                             sorted[i].second.c_str());
            }
        };
        text += fmt::format("=== thread \"{}\": {} samples\n", targets[thread].name, total);
        print("self (where the thread is)", self, 60);
        // The hottest instructions as module offsets, to look up which line of a function they
        // are, inlined code included (Symbolizer::Module names the tool).
        std::map<u64, unsigned> addresses;
        for (const Sample& sample : samples) {
            if (sample.thread == thread && sample.depth != 0) {
                ++addresses[sample.frames[0]];
            }
        }
        std::vector<std::pair<unsigned, u64>> hot;
        for (const auto& [address, count] : addresses) {
            hot.emplace_back(count, address);
        }
        std::sort(hot.rbegin(), hot.rend());
        text += "-- self by instruction (module+offset)\n";
        for (size_t i = 0; i < (std::min<size_t>)(hot.size(), 60); ++i) {
            const u64 address = hot[i].second;
            const auto [module, offset] = symbolizer.Module(address);
            text += fmt::format("{:6} {}+{:#x} {}\n", hot[i].first, module, offset,
                                symbolizer.Name(address));
        }
        print("self <- caller", self_caller, 50);
        print("inclusive (on the stack)", inclusive, 90);
        text += "\n";
    }
    if (std::FILE* const file = std::fopen(path, "w")) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }

    // <out>.folded: every distinct stack with its count, root first ("thread;a;b;c count").
    std::map<std::string, unsigned> folded;
    for (const Sample& sample : samples) {
        std::string line = targets[sample.thread].name;
        for (unsigned i = sample.depth; i-- > 0;) {
            line += ';';
            line += symbolizer.Name(sample.frames[i]);
        }
        ++folded[line];
    }
    if (std::FILE* const out = std::fopen((std::string{path} + ".folded").c_str(), "w")) {
        for (const auto& [line, count] : folded) {
            std::fprintf(out, "%s %u\n", line.c_str(), count);
        }
        std::fclose(out);
    }
    return text;
}

std::string SampleThreads(const std::vector<std::string>& patterns, double seconds,
                          const std::string& out) {
    std::vector<Target> targets = FindThreads(patterns);
    std::fprintf(stderr, "nx2 sampler: %zu threads match, sampling %.0f s\n", targets.size(),
                 seconds);
    if (targets.empty()) {
        return "no thread matches";
    }
    std::vector<Sample> samples;
    samples.reserve(static_cast<size_t>(seconds * 1100.0) * targets.size() + 16);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline &&
           samples.size() + targets.size() <= samples.capacity()) {
        for (unsigned i = 0; i < targets.size(); ++i) {
            // Stays within the reserved capacity: nothing allocates while a thread is paused.
            Sample& sample = samples.emplace_back();
            sample.thread = i;
            if (!Capture(targets[i], sample)) {
                samples.pop_back();
            }
        }
        Pause();
    }
    std::string text = Report(targets, samples, out.c_str());
    std::fprintf(stderr, "nx2 sampler: %zu samples written to %s\n", samples.size(), out.c_str());
    for (const auto& target : targets) {
        Close(target);
    }
    return text;
}

} // Anonymous namespace

std::string Sample(const char* names, double seconds, const std::string& out) {
    return SampleThreads(SplitNames(names), seconds, out);
}

void StartFromEnvironment() {
    const char* names = std::getenv("EDEN_NX2_SAMPLE");
    if (names == nullptr || *names == '\0') {
        return;
    }
    const char* out = std::getenv("EDEN_NX2_SAMPLE_OUT");
    std::thread{[patterns = SplitNames(names), start = EnvSeconds("EDEN_NX2_SAMPLE_START", 60.0),
                 seconds = EnvSeconds("EDEN_NX2_SAMPLE_SECONDS", 30.0),
                 out = std::string{out != nullptr ? out : "nx2_samples.txt"}] {
        std::this_thread::sleep_for(std::chrono::duration<double>(start));
        SampleThreads(patterns, seconds, out);
    }}.detach();
}

} // namespace Nx2Sampler

#else

namespace Nx2Sampler {
void StartFromEnvironment() {}
std::string Sample(const char*, double, const std::string&) {
    return "the sampler needs Windows or Linux (glibc, x86-64 or arm64)";
}
} // namespace Nx2Sampler

#endif
