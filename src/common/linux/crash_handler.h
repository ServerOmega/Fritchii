// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Linux host crash handler. A fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) flushes the
// log file, writes "Host crash: SIG<name> pc=... addr=... thread=<name> build=<hash>" and a
// backtrace (module+offset) to the log and stderr, then dies by the same signal so
// systemd-coredump still records it. Installed in main() before any JIT starts: Dynarmic and NCE
// save it as their previous handler and chain to it for faults that are not theirs.
// Elsewhere (Windows, Android, macOS) every function is a no-op.

namespace Common::CrashHandler {

/// Installs the handlers and an alternate signal stack for the calling thread. Idempotent.
void Install();

/// Gives the calling thread its own alternate signal stack (so a stack overflow still reports).
/// Called from SetCurrentThreadName; keeps a stack the thread already has (Dynarmic, NCE).
void PrepareThread();

/// EDEN_NX2_CRASH_TEST=segv|bus|ill|fpe|abort: crashes on a thread named "CrashTest"
/// EDEN_NX2_CRASH_TEST_DELAY seconds (default 5) after this call. A test hook only.
/// `when` names the call site ("boot", "start") and must match EDEN_NX2_CRASH_TEST_AT
/// (default "boot"; "start" fires without a game, used to test the handler on a desktop).
void StartTestFromEnvironment(const char* when);

} // namespace Common::CrashHandler
