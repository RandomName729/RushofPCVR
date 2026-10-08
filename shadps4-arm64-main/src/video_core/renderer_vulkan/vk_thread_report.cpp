// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A measuring aid for SHADPS4_FRAME_STATS: how much processor time each thread of the emulator
// (and so of the game it runs) used since the last report. A thread near 100% is the one that
// limits the frame rate; threads that sleep show low numbers however much they wait on others.

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

} // namespace

void LogThreadCpuUsage(double seconds) {
    static std::unordered_map<DWORD, u64> last_total;
    const bool first_look = last_total.empty();

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    struct Row {
        double percent;
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
            // FILETIME counts 100 ns.
            const double percent = seconds > 0.0
                                       ? static_cast<double>(total - before) * 1e-7 / seconds * 100.0
                                       : 0.0;
            all += percent;
            if (percent >= 3.0) {
                rows.push_back({percent, ThreadName(thread), entry.th32ThreadID});
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    if (first_look) {
        return; // nothing to compare with yet
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.percent > b.percent; });
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
}

#else

void LogThreadCpuUsage(double) {}

#endif

} // namespace Vulkan
