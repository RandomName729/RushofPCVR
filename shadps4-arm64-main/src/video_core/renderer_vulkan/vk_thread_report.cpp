// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A measuring aid for SHADPS4_FRAME_STATS: how much processor time each thread of the emulator
// (and so of the game it runs) used since the last report. A thread near 100% is the one that
// limits the frame rate; threads that sleep show low numbers however much they wait on others.
//
// With SHADPS4_FRAME_STATS_SAMPLE=1 the busiest thread (over 40%) is also looked at: where it
// started, how much of its time is spent in the system (the graphics driver, mostly), and,
// several dozen times at every report, where it is running at that moment (the function and the
// functions that called it, as module+offset: shadps4.exe's offsets are looked up in its map
// file). The counts add up over the whole run.

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cstdlib>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/types.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace Vulkan {

#ifdef _WIN32

namespace {

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

std::string ThreadName(HANDLE thread) {
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),
                                               "GetThreadDescription")));
    if (get_description == nullptr) {
        return {};
    }
    PWSTR wide = nullptr;
    if (FAILED(get_description(thread, &wide)) || wide == nullptr) {
        return {};
    }
    std::string name;
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
        name.resize(static_cast<size_t>(size) - 1);
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), size, nullptr, nullptr);
    }
    LocalFree(wide);
    return name;
}

u64 ToU64(const FILETIME& time) {
    return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
}

bool SamplingWanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("SHADPS4_FRAME_STATS_SAMPLE");
        return value != nullptr && std::atoi(value) != 0;
    }();
    return wanted;
}

/// "name.dll+0x1234" for a code address, with the offset of the function that holds it (or of the
/// address itself, where nothing is known of its function).
std::string Where(u64 address) {
    static std::unordered_map<u64, std::string> names; // module base -> file name
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        module == nullptr) {
        return fmt::format("{:#x}", address);
    }
    const u64 base = reinterpret_cast<u64>(module);
    auto it = names.find(base);
    if (it == names.end()) {
        wchar_t path[MAX_PATH]{};
        std::string name = "?";
        if (GetModuleFileNameW(module, path, MAX_PATH) != 0) {
            std::wstring wide{path};
            const size_t slash = wide.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                wide.erase(0, slash + 1);
            }
            name.clear();
            for (const wchar_t c : wide) {
                name.push_back(c < 128 ? static_cast<char>(c) : '?');
            }
        }
        it = names.emplace(base, std::move(name)).first;
    }
    return fmt::format("{}+{:#x}", it->second, address - base);
}

constexpr int MaxFrames = 12;

struct StackSample {
    int count{};
    std::array<u64, MaxFrames> function{}; // where each function starts
};

#if defined(_M_X64) || defined(__x86_64__)
/// The thread's stack as the functions on it (the innermost first), taken while it stands still.
/// Nothing in here may need a lock the thread could be holding: no allocation, no module lookups.
StackSample TakeSample(HANDLE thread) {
    StackSample sample;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        return sample;
    }
    if (GetThreadContext(thread, &context)) {
        __try {
            for (int i = 0; i < MaxFrames && context.Rip != 0; ++i) {
                DWORD64 image = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
                if (function != nullptr) {
                    sample.function[i] = image + function->BeginAddress;
                    PVOID handler_data = nullptr;
                    DWORD64 frame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context,
                                     &handler_data, &frame, nullptr);
                } else {
                    sample.function[i] = context.Rip;
                    context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                }
                sample.count = i + 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    ResumeThread(thread);
    return sample;
}
#else
StackSample TakeSample(HANDLE) {
    return {};
}
#endif

/// Where the thread started: the module and offset of its entry point.
std::string StartOf(HANDLE thread) {
    using QueryFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    PVOID start = nullptr;
    // ThreadQuerySetWin32StartAddress
    if (query == nullptr || query(thread, 9, &start, sizeof(start), nullptr) != 0 ||
        start == nullptr) {
        return "unknown";
    }
    return Where(reinterpret_cast<u64>(start));
}

/// What the sampling of the busiest threads has found, over the whole run.
struct Sampling {
    std::unordered_map<std::string, u64> leaf;      // where the thread was, innermost function
    std::unordered_map<std::string, u64> inclusive; // functions anywhere on its stack
    std::unordered_map<DWORD, std::string> started;
    u64 samples{};
    int reports{};
};

Sampling g_sampling;

std::string Top(const std::unordered_map<std::string, u64>& counts, size_t how_many, u64 of) {
    std::vector<std::pair<std::string, u64>> rows{counts.begin(), counts.end()};
    std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (size_t i = 0; i < rows.size() && i < how_many; ++i) {
        text += fmt::format("{}{} {:.0f}%", i == 0 ? "" : ", ", rows[i].first,
                            100.0 * static_cast<double>(rows[i].second) / static_cast<double>(of));
    }
    return text;
}

/// Looks at one thread a few dozen times, a millisecond apart.
void SampleThread(DWORD id, double percent, double kernel_percent) {
    static bool announced = false;
    if (!announced) {
        announced = true;
        // Which build this is, to find the right map file for its offsets.
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const u8*>(dos) + dos->e_lfanew);
        LOG_INFO(Render_Vulkan,
                 "thread sampling is on; shadps4.exe is at {:#x}, linked at {:08x} (the stamp "
                 "at the top of its map file)",
                 reinterpret_cast<u64>(dos), static_cast<u32>(headers->FileHeader.TimeDateStamp));
    }
    if (id == GetCurrentThreadId()) {
        return;
    }
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                         THREAD_QUERY_INFORMATION,
                                     FALSE, id);
    if (thread == nullptr) {
        return;
    }
    if (!g_sampling.started.contains(id)) {
        g_sampling.started[id] = StartOf(thread);
    }
    constexpr int Samples = 40;
    std::array<StackSample, Samples> taken;
    LARGE_INTEGER frequency{}, now{}, until{};
    QueryPerformanceFrequency(&frequency);
    for (int i = 0; i < Samples; ++i) {
        taken[i] = TakeSample(thread);
        QueryPerformanceCounter(&now);
        until.QuadPart = now.QuadPart + frequency.QuadPart / 1000; // about a millisecond
        do {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < until.QuadPart);
    }
    CloseHandle(thread);
    // The names are looked up now that the thread runs again.
    std::unordered_map<u64, std::string> looked_up;
    const auto name_of = [&](u64 address) -> const std::string& {
        auto it = looked_up.find(address);
        if (it == looked_up.end()) {
            it = looked_up.emplace(address, Where(address)).first;
        }
        return it->second;
    };
    for (const StackSample& sample : taken) {
        if (sample.count == 0) {
            continue;
        }
        ++g_sampling.samples;
        ++g_sampling.leaf[name_of(sample.function[0])];
        std::unordered_set<std::string> seen;
        for (int i = 0; i < sample.count; ++i) {
            if (seen.insert(name_of(sample.function[i])).second) {
                ++g_sampling.inclusive[name_of(sample.function[i])];
            }
        }
    }
    if (++g_sampling.reports % 4 == 1 && g_sampling.samples > 0) {
        LOG_INFO(Render_Vulkan,
                 "busiest thread {} ({:.0f}%, of which in the system {:.0f}%) started in {}; "
                 "{} samples so far. Innermost function: {}. Anywhere on its stack: {}",
                 id, percent, kernel_percent, g_sampling.started[id], g_sampling.samples,
                 Top(g_sampling.leaf, 8, g_sampling.samples),
                 Top(g_sampling.inclusive, 14, g_sampling.samples));
    }
}

} // namespace

void LogThreadCpuUsage(double seconds) {
    static std::unordered_map<DWORD, u64> last_total;
    static std::unordered_map<DWORD, u64> last_kernel;
    const bool first_look = last_total.empty();

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    struct Row {
        double percent;
        double kernel_percent;
        std::string name;
        DWORD id;
    };
    std::vector<Row> rows;
    double all = 0.0;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) {
            continue;
        }
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            const u64 total = ToU64(kernel) + ToU64(user);
            const auto it = last_total.find(entry.th32ThreadID);
            const u64 before = it != last_total.end() ? it->second : total;
            last_total[entry.th32ThreadID] = total;
            const auto kernel_it = last_kernel.find(entry.th32ThreadID);
            const u64 kernel_before = kernel_it != last_kernel.end() ? kernel_it->second
                                                                      : ToU64(kernel);
            last_kernel[entry.th32ThreadID] = ToU64(kernel);
            const double kernel_percent =
                seconds > 0.0 ? static_cast<double>(ToU64(kernel) - kernel_before) * 1e-7 /
                                    seconds * 100.0
                              : 0.0;
            // FILETIME counts 100 ns.
            const double percent = seconds > 0.0
                                       ? static_cast<double>(total - before) * 1e-7 / seconds * 100.0
                                       : 0.0;
            all += percent;
            if (percent >= 3.0) {
                rows.push_back({percent, kernel_percent, ThreadName(thread), entry.th32ThreadID});
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    if (first_look) {
        return; // nothing to compare with yet
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.percent > b.percent; });
    const bool sample_busiest = SamplingWanted() && !rows.empty() && rows[0].percent >= 40.0;
    const Row busiest = sample_busiest ? rows[0] : Row{};
    std::string line;
    for (size_t i = 0; i < rows.size() && i < 10; ++i) {
        line += fmt::format("{}{} {:.0f}%", i == 0 ? "" : ", ",
                            rows[i].name.empty() ? fmt::format("thread {}", rows[i].id)
                                                 : rows[i].name,
                            rows[i].percent);
    }
    LOG_INFO(Render_Vulkan,
             "thread load (percent of one core, threads above 3%): {}; all threads together "
             "{:.0f}% ({:.1f} cores)",
             line.empty() ? std::string{"none"} : line, all, all / 100.0);
    if (sample_busiest) {
        SampleThread(busiest.id, busiest.percent, busiest.kernel_percent);
    }
}

#else

void LogThreadCpuUsage(double) {}

#endif

} // namespace Vulkan
