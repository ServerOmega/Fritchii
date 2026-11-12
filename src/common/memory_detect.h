// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>

#include "common/common_types.h"

namespace Common {

struct MemoryInfo {
    u64 TotalPhysicalMemory{};
    u64 TotalSwapMemory{};
};

/**
 * Gets the memory info of the host system
 * @return Reference to a MemoryInfo struct with the physical and swap memory sizes in bytes
 */
[[nodiscard]] const MemoryInfo& GetMemInfo();

/// Page faults of this process so far, soft and hard (0 where unknown).
[[nodiscard]] u64 GetProcessPageFaults();

/// This process's memory and the system's commit, for the memory log line (zeros where unknown;
/// only Windows fills them in).
/// Commit the system can still hand out, in bytes: its commit limit (RAM plus the page files)
/// minus what is committed. No value where the host reports no such limit (anything but Windows).
/// From zortos293/Lichii#1210.
[[nodiscard]] std::optional<u64> GetSystemCommitRoom();

struct ProcessMemoryUsage {
    u64 working_set{};
    u64 peak_working_set{};
    u64 private_bytes{}; ///< Private commit (mapped sections like the guest memory are not in it)
    u64 peak_private_bytes{};
    /// Resident pages not shareable with other mappings: the working set minus the guest memory
    /// section and mapped files (0 where Windows does not report it: older builds).
    u64 private_working_set{};
    /// Commit charge of the whole system (Windows CommitTotal, Linux Committed_AS).
    u64 system_commit{};
    /// Commit limit of the whole system (CommitLimit on both).
    u64 system_commit_limit{};
    u64 available_physical{};
};
[[nodiscard]] ProcessMemoryUsage GetProcessMemoryUsage();

/// The value of a "Key:   1234 kB" line of /proc/self/status or /proc/meminfo, in bytes. `key`
/// includes the colon and must start a line. 0 when the field is missing or malformed.
[[nodiscard]] u64 ProcKilobytesField(std::string_view text, std::string_view key);

/// This process's resident set around one TrimHeap call, in bytes (0 where it does nothing).
struct HeapTrimResult {
    u64 resident_before{};
    u64 resident_after{};
};

/// Linux with glibc: malloc_trim(0), which hands the free pages inside every malloc arena back to
/// the system, and one log line with the resident set before and after. glibc only gives back
/// free memory at the top of an arena by itself, so what a burst of frees leaves between live
/// blocks (thousands of destroyed pipelines, the boot builds' compiler scratch) stays resident.
/// It locks one arena at a time, so a thread that allocates from the arena being walked waits
/// for that slice: call it off the GPU thread. Does nothing elsewhere. From Lichii #1271.
HeapTrimResult TrimHeap(std::string_view reason);

/// This process's Win32 heaps (the C runtime's malloc heap and every other HeapCreate heap: Qt's,
/// drivers' that use one), summed with HeapSummary. Zeros elsewhere.
struct HeapUsage {
    u64 crt_committed{};   ///< The C runtime's heap (malloc, new)
    u64 crt_allocated{};
    u64 other_committed{}; ///< Every other heap of the process
    u64 other_allocated{};
    u32 heaps{};           ///< Heaps summed, the C runtime's included
    u32 micros{};          ///< What the pass took
};
/// Locks each heap for its summary: call it off the busy threads. Also kept for LastHeapUsage.
HeapUsage SampleHeapUsage();
/// The last SampleHeapUsage result (zeros before the first).
[[nodiscard]] HeapUsage LastHeapUsage();

} // namespace Common
