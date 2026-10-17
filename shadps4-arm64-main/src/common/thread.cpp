// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <string>
#include <thread>

#include "core/libraries/kernel/threads/pthread.h"

#include "common/error.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "ntapi.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#include "common/string_util.h"
#else
#if defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#else
#include <pthread.h>
#endif
#include <sched.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __FreeBSD__
#define cpu_set_t cpuset_t
#endif

namespace Common {

#ifdef __APPLE__

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // CPU time to grant.
    const std::chrono::nanoseconds computation_ns = period_ns / 2;

    // Determine the timebase for converting time to ticks.
    struct mach_timebase_info timebase{};
    mach_timebase_info(&timebase);
    const auto ticks_per_ns =
        static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer);

    const auto period_ticks =
        static_cast<u32>(static_cast<double>(period_ns.count()) * ticks_per_ns);
    const auto computation_ticks =
        static_cast<u32>(static_cast<double>(computation_ns.count()) * ticks_per_ns);

    thread_time_constraint_policy policy = {
        .period = period_ticks,
        .computation = computation_ticks,
        // Should not matter since preemptible is false, but needs to be >= computation regardless.
        .constraint = computation_ticks,
        .preemptible = false,
    };

    int ret = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (ret != KERN_SUCCESS) {
        LOG_ERROR(Common, "Could not set thread to real-time with period {} ns: {}",
                  period_ns.count(), ret);
    }
}

#else

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // Not implemented
}

#endif

#ifdef _WIN32

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    auto handle = GetCurrentThread();
    int windows_priority = 0;
    switch (new_priority) {
    case ThreadPriority::Low:
        windows_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case ThreadPriority::Normal:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    case ThreadPriority::High:
        windows_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case ThreadPriority::VeryHigh:
        windows_priority = THREAD_PRIORITY_HIGHEST;
        break;
    case ThreadPriority::Critical:
        windows_priority = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    }
    SetThreadPriority(handle, windows_priority);
}

namespace {

// One waitable timer a thread, kept for as long as the thread lives. A high-resolution timer
// (Windows 10 1803 and later) wakes the thread within a fraction of a millisecond; the ordinary
// kind only on the next tick of the system clock, which is up to 15.6 ms away unless something
// has asked for a finer one. Sleeps of a few milliseconds, as the headset refresh makes them,
// are what that ruins.
struct SleepTimer {
    HANDLE handle{};
    bool high_resolution{};

    SleepTimer() {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        handle = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
        high_resolution = handle != nullptr;
        if (handle == nullptr) {
            handle = ::CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
    }
    ~SleepTimer() {
        if (handle != nullptr) {
            ::CloseHandle(handle);
        }
    }
    SleepTimer(const SleepTimer&) = delete;
    SleepTimer& operator=(const SleepTimer&) = delete;
};

} // namespace

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();

    thread_local SleepTimer sleep_timer;
    DWORD ret = WAIT_FAILED;
    if (sleep_timer.handle != nullptr) {
        LARGE_INTEGER interval{
            .QuadPart = -1 * std::max<s64>(duration.count() / 100, 1),
        };
        // (Setting the timer again also clears it if an earlier sleep was cut short.)
        if (::SetWaitableTimer(sleep_timer.handle, &interval, 0, nullptr, nullptr, FALSE)) {
            ret = WaitForSingleObjectEx(sleep_timer.handle, INFINITE, interruptible);
            if (ret != WAIT_OBJECT_0) {
                ::CancelWaitableTimer(sleep_timer.handle);
            }
        }
    }

    if (remaining) {
        const auto end_sleep = std::chrono::high_resolution_clock::now();
        const auto sleep_time = end_sleep - begin_sleep;
        *remaining = duration > sleep_time ? duration - sleep_time : std::chrono::nanoseconds(0);
    }
    return ret == WAIT_OBJECT_0;
}

namespace {
using NtSetTimerResolutionFn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
}

void TuneHostForGame() {
    const char* no_tuning = std::getenv("SHADPS4_NO_HOST_TUNING");
    if (no_tuning != nullptr && no_tuning[0] == '1') {
        LOG_INFO(Common, "Host tuning is off (SHADPS4_NO_HOST_TUNING)");
        return;
    }

    // The system clock ticks every 0.5 ms (as fine as it goes) instead of every 15.6 ms.
    // Since Windows 11 the setting of one process no longer applies to the others, and one
    // that is not in front may be ignored, so what follows asks to be left alone as well.
    ULONG resolution = 0;
    if (const auto ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
        if (const auto set = reinterpret_cast<NtSetTimerResolutionFn>(
                ::GetProcAddress(ntdll, "NtSetTimerResolution"))) {
            set(5000, TRUE, &resolution);
        }
    }

    // No power saving for the process: it is not put on slow cores or have its timers
    // stretched when the machine thinks it is in the background.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                             PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    const bool throttling_off = ::SetProcessInformation(::GetCurrentProcess(),
                                                        ProcessPowerThrottling, &throttling,
                                                        sizeof(throttling)) != 0;

    // On a processor with fast and slow cores, the title's threads stay on the fast ones: a
    // thread the system leaves on an efficiency core takes half as long again for the same work,
    // and the title's main thread is what a frame waits for. (SHADPS4_ALL_CORES=1 keeps all.)
    u32 fast_cores = 0;
    u32 all_cores = 0;
    const char* all = std::getenv("SHADPS4_ALL_CORES");
    if (all == nullptr || all[0] != '1') {
        ULONG size = 0;
        ::GetSystemCpuSetInformation(nullptr, 0, &size, ::GetCurrentProcess(), 0);
        if (size != 0) {
            std::vector<u8> buffer(size);
            if (::GetSystemCpuSetInformation(
                    reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), size, &size,
                    ::GetCurrentProcess(), 0)) {
                BYTE best = 0;
                for (ULONG off = 0; off < size;) {
                    const auto* info =
                        reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + off);
                    if (info->Type == CpuSetInformation) {
                        best = std::max(best, info->CpuSet.EfficiencyClass);
                    }
                    off += info->Size;
                }
                std::vector<ULONG> ids;
                for (ULONG off = 0; off < size;) {
                    const auto* info =
                        reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + off);
                    if (info->Type == CpuSetInformation) {
                        ++all_cores;
                        if (info->CpuSet.EfficiencyClass == best) {
                            ids.push_back(info->CpuSet.Id);
                        }
                    }
                    off += info->Size;
                }
                // (Only where the cores differ, and where enough remain for the title.)
                if (!ids.empty() && ids.size() < all_cores && ids.size() >= 8) {
                    if (::SetProcessDefaultCpuSets(::GetCurrentProcess(), ids.data(),
                                                   static_cast<ULONG>(ids.size()))) {
                        fast_cores = static_cast<u32>(ids.size());
                    }
                }
            }
        }
    }

    LOG_INFO(Common,
             "Host tuning: system clock {:.2f} ms, power throttling {}, {} of {} logical "
             "processors used",
             resolution / 10000.0, throttling_off ? "off" : "unchanged",
             fast_cores != 0 ? fast_cores : all_cores, all_cores);
}

#else

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    pthread_t this_thread = pthread_self();

    const auto scheduling_type = SCHED_OTHER;
    s32 max_prio = sched_get_priority_max(scheduling_type);
    s32 min_prio = sched_get_priority_min(scheduling_type);
    u32 level = std::max(static_cast<u32>(new_priority) + 1, 4U);

    struct sched_param params;
    if (max_prio > min_prio) {
        params.sched_priority = min_prio + ((max_prio - min_prio) * level) / 4;
    } else {
        params.sched_priority = min_prio - ((min_prio - max_prio) * level) / 4;
    }

    pthread_setschedparam(this_thread, scheduling_type, &params);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    timespec request = {
        .tv_sec = duration.count() / 1'000'000'000,
        .tv_nsec = duration.count() % 1'000'000'000,
    };
    timespec remain;
    int ret;
    while ((ret = nanosleep(&request, &remain)) < 0 && errno == EINTR) {
        if (interruptible) {
            break;
        }
        request = remain;
    }
    if (remaining) {
        *remaining = std::chrono::nanoseconds(remain.tv_sec * 1'000'000'000 + remain.tv_nsec);
    }
    return ret == 0 || errno != EINTR;
}

void TuneHostForGame() {}

#endif

#ifdef _WIN32

// Sets the debugger-visible name of the current thread.
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());
}

void SetThreadName(void* thread, const char* name) {
    SetThreadDescription(thread, UTF8ToUTF16W(name).data());
}

#else // !_WIN32, so must be POSIX threads

// MinGW with the POSIX threading model does not support pthread_setname_np
#if !defined(_WIN32) || defined(_MSC_VER)
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
#ifdef __APPLE__
    pthread_setname_np(name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__)
    // Linux limits thread names to 15 characters and will outright reject any
    // attempt to set a longer name with ERANGE.
    std::string truncated(name, std::min(strlen(name), static_cast<std::size_t>(15)));
    if (int e = pthread_setname_np(pthread_self(), truncated.c_str())) {
        errno = e;
        LOG_ERROR(Common, "Failed to set thread name to '{}': {}", truncated, GetLastErrorMsg());
    }
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void SetThreadName(void* thread, const char* name) {
    // TODO
}
#endif

#if defined(_WIN32)
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    // Do Nothing on MinGW
}

void SetThreadName(void* thread, const char* name) {
    // Do Nothing on MinGW
}
#endif

#endif

AccurateTimer::AccurateTimer(std::chrono::nanoseconds target_interval)
    : target_interval(target_interval) {}

void AccurateTimer::Start() {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();
    if (total_wait.count() > 0) {
        AccurateSleep(total_wait, nullptr, false);
    }
    start_time = std::chrono::high_resolution_clock::now();
    total_wait -= std::chrono::duration_cast<std::chrono::nanoseconds>(start_time - begin_sleep);
}

void AccurateTimer::End() {
    auto now = std::chrono::high_resolution_clock::now();
    total_wait +=
        target_interval - std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time);
}

std::string GetCurrentThreadName() {
    using namespace Libraries::Kernel;
    if (g_curthread && !g_curthread->name.empty()) {
        return g_curthread->name;
    }
#ifdef _WIN32
    PWSTR name;
    GetThreadDescription(GetCurrentThread(), &name);
    return Common::UTF16ToUTF8(name);
#else
    char name[256];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return std::string{name};
#endif
}

} // namespace Common
