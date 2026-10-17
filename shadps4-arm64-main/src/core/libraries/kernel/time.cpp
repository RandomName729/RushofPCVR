// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_cpu/guest_wait_trace.h"
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <chrono>
#include <memory>
#include <cstring>

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/native_clock.h"
#include "common/thread.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

#ifdef _WIN64
#include <windows.h>
#include <intrin.h>
#include "common/ntapi.h"
#else
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <date/tz.h>
#endif
#include <ctime>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace Libraries::Kernel {

void ApiCallNote(const char* name);

static u64 initial_ptc;
static std::unique_ptr<Common::NativeClock> clock;

// Host-side threads (e.g. the Bachata input reader) can query process time before
// RegisterTime runs during InitHLELibs; report 0/1 Hz rather than dereferencing null.
// DIAGNOSTIC / TEST: SHADPS4_TSC_HZ=<hz> makes sceKernelReadTsc and sceKernelGetTscFrequency
// behave like a time stamp counter of that frequency (the console's runs at about 1.6 GHz). A
// title that asks for the frequency does not notice; one that has it built in does.
static double TscScale() {
    static double scale = 0.0;
    if (scale == 0.0 && clock) {
        const char* value = std::getenv("SHADPS4_TSC_HZ");
        const double hz = value != nullptr ? std::atof(value) : 0.0;
        const double host = static_cast<double>(clock->GetTscFrequency());
        scale = hz >= 1e8 && host > 0.0 ? hz / host : 1.0;
        LOG_INFO(Lib_Kernel, "The host's time stamp counter runs at {:.0f} Hz; the title's is "
                             "made to run {:.4f} times as fast (SHADPS4_TSC_HZ)",
                 host, scale);
    }
    return scale == 0.0 ? 1.0 : scale;
}


// DIAGNOSTIC: bytes of guest code around a call, to see how the title keeps time.
static void DumpGuestCode(const char* why, const void* caller, size_t before, size_t after) {
    const auto* start = static_cast<const u8*>(caller) - before;
    std::string out = fmt::format("{}: called from {} ({:#x}) by {}; code from {} bytes before "
                                  "to {} after:\n",
                                  why, Core::GuestCpu::GuestWaitCaller(caller),
                                  reinterpret_cast<uintptr_t>(caller),
                                  Common::GetCurrentThreadName(), before, after);
    for (size_t i = 0; i < before + after; i += 32) {
        out += fmt::format("{:x}:", reinterpret_cast<uintptr_t>(start + i));
        for (size_t j = 0; j < 32 && i + j < before + after; ++j) {
            out += fmt::format(" {:02x}", start[i + j]);
        }
        out += "\n";
    }
    LOG_INFO(Lib_Kernel, "{}", out);
}


// DIAGNOSTIC: SHADPS4_DUMP_CODE="<hex address>:<bytes before>:<bytes after>;..." writes guest code
// to the log once the title has run for a while, and SHADPS4_SCAN_RDTSC=1 lists every rdtsc /
// rdtscp in its executable memory (to find how it keeps time).
static bool GuestReadable(const void* p, size_t size) {
#ifdef _WIN64
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(p, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    return static_cast<const u8*>(p) + size <=
           static_cast<const u8*>(info.BaseAddress) + info.RegionSize;
#else
    return true;
#endif
}

#ifdef _WIN64
// DIAGNOSTIC / TEST: SHADPS4_RDTSC_HZ=<hz> makes the title's own rdtsc instructions (those it
// executes without asking the kernel) see a counter of that frequency instead of the host's.
// Every rdtsc that looks like a real one is replaced by an undefined instruction that is caught
// and answered here. A value below 1e8 only counts the reads and leaves the counter unchanged.
namespace {
struct RdtscParams {
    u64 base_host;
    u64 base_guest;
    u64 num;
    u64 den;
};
struct RdtscTrap {
    std::vector<uintptr_t> sites;                    // sorted, never changed after install
    std::unique_ptr<std::atomic<u64>[]> counts;
    std::atomic<const RdtscParams*> params{nullptr};
    std::atomic<u64> total{0};
    // the game's timer objects seen at the two timer-update sites (their rdi)
    std::atomic<uintptr_t> timers[16]{};
    std::atomic<u32> timer_count{0};
};
RdtscTrap* g_trap = nullptr;

u64 ScaleTicks(const RdtscParams& p, u64 delta) {
    return (delta / p.den) * p.num + (delta % p.den) * p.num / p.den;
}

u64 GuestTicks(const RdtscParams& p, u64 now) {
    const s64 delta = static_cast<s64>(now - p.base_host);
    return delta >= 0 ? p.base_guest + ScaleTicks(p, static_cast<u64>(delta))
                      : p.base_guest - ScaleTicks(p, static_cast<u64>(-delta));
}

LONG CALLBACK RdtscHandler(PEXCEPTION_POINTERS info) {
    if (g_trap == nullptr || info->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t rip = static_cast<uintptr_t>(info->ContextRecord->Rip);
    const auto it = std::lower_bound(g_trap->sites.begin(), g_trap->sites.end(), rip);
    if (it == g_trap->sites.end() || *it != rip) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_trap->counts[it - g_trap->sites.begin()].fetch_add(1, std::memory_order_relaxed);
    g_trap->total.fetch_add(1, std::memory_order_relaxed);
    // The title's timer-update function (game+0x4e9190 / +0x4e9230): remember which timer.
    if (rip == 0x800000000ull + 0x4e9190 || rip == 0x800000000ull + 0x4e91bf ||
        rip == 0x800000000ull + 0x4e925d) {
        const uintptr_t timer = static_cast<uintptr_t>(info->ContextRecord->Rdi);
        const u32 n = g_trap->timer_count.load(std::memory_order_relaxed);
        bool known = false;
        for (u32 i = 0; i < n && i < 16; ++i) {
            known = known || g_trap->timers[i].load(std::memory_order_relaxed) == timer;
        }
        if (!known && n < 16) {
            g_trap->timers[n].store(timer, std::memory_order_relaxed);
            g_trap->timer_count.store(n + 1, std::memory_order_relaxed);
        }
    }
    const RdtscParams* p = g_trap->params.load(std::memory_order_acquire);
    const u64 guest = GuestTicks(*p, __rdtsc());
    info->ContextRecord->Rax = guest & 0xFFFFFFFFull;
    info->ContextRecord->Rdx = guest >> 32;
    info->ContextRecord->Rip = rip + 2;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void InstallRdtscTrap(double hz) {
    auto* trap = new RdtscTrap();
    std::vector<uintptr_t> sites;
    u32 candidates = 0;
    // Every rdtsc instruction in the US 1.00 game (found by disassembling it), except the
    // profiler's zone markers, which keep only the low half of the result for the profiler.
    static const u32 kRdtscSites[] = {
    0x40820, 0x409e7, 0xcac8e, 0xcac90, 0xcac92, 0xcad26, 0xcad9e, 0xcada0,
    0xcba6d, 0xcba6f, 0xcbaf3, 0xcbb4d, 0xcbb7f, 0xcbb81, 0xcbbcf, 0xcbc25,
    0xcc07c, 0xcc088, 0xcc0cd, 0xcc0cf, 0xcc0d1, 0xcc173, 0xcc1e7, 0xcc1e9,
    0xdf616, 0xdf702, 0xdf816, 0xdfbf7, 0xe058b, 0xe071a, 0xe0f07, 0xe11bf,
    0xf96d7, 0xf9705, 0xf9831, 0xf9865, 0xf9b20, 0xf9eca, 0xf9ee0, 0xfa760,
    0xfaa0c, 0xfaa22, 0xfaa55, 0xfaa72, 0xfaae7, 0xfac28, 0xfb0c8, 0x15ccdf,
    0x15e29e, 0x187db7, 0x187e00, 0x19a1cf, 0x1f2274, 0x1f22c8, 0x1f22e9, 0x1f26f3,
    0x1f2720, 0x1f2775, 0x1f27ef, 0x2a094f, 0x2a0f04, 0x2a0f67, 0x2a22b2, 0x2b4061,
    0x2b435b, 0x2b44ef, 0x2b744e, 0x2b7769, 0x2b7ab0, 0x2b7b66, 0x2c6a6e, 0x2c6f54,
    0x2d644d, 0x2d6789, 0x2d6e0a, 0x363756, 0x36379d, 0x36396a, 0x3639be, 0x3639e8,
    0x363b9e, 0x363d38, 0x363d7b, 0x363da1, 0x363f3b, 0x3640fd, 0x36414e, 0x364182,
    0x364591, 0x364776, 0x3647bb, 0x3647e1, 0x364979, 0x364b63, 0x364bae, 0x364be2,
    0x3650d0, 0x42fa21, 0x42fb66, 0x42fc34, 0x42fc68, 0x43077f, 0x4a9d69, 0x4b642d,
    0x4b70b4, 0x4b7360, 0x4b9e40, 0x4bbf7a, 0x4bca59, 0x4bdb70, 0x4bdb90, 0x4d4a92,
    0x4d4aa0, 0x4d4ab5, 0x4e9150, 0x4e9190, 0x4e91bf, 0x4e925d, 0x51da77, 0x51dc2e,
    0x529564, 0x529d63, 0x7a80bb, 0x7a80ff, 0x7a816a, 0x7a819d, 0x8e6cd1, 0x8e6ce1,
    0x8e6d0b, 0x8e6d32, 0x8e6d44, 0x8e7dc2, 0x955cca, 0x955cde, 0x955d67, 0x95e0bc,
    0x95e627, 0x95ec7c, 0x95efdb, 0xb81de2, 0xbf0fea, 0xceb9ce, 0xceba31, 0xceba78,
    0xcebad8, 0xcebce6, 0xcebd5e, 0xcebd8f, 0xcebe06, 0xcec28e, 0xcec303, 0xcec9ca,
    0xceca29, 0xcece7f, 0xceced7, 0xced05f, 0xced0b8, 0xced25f, 0xced2d9, 0xced336,
    0xced3b1, 0x129d02a, 0x129d040, 0x129d059, 0x12ae5ff, 0x12ffda6, 0x1344194, 0x13441b9,
    0x134ca35, 0x138e18b, 0x146d609, 0x14bc76b, 0x14d859d, 0x14ddc71, 0x15a4042, 0x15a4d91,
    0x15a5bb7, 0x15a6874, 0x15a76d7, 0x15a83f9, 0x15a8fb7, 0x15a9e07, 0x15aab38, 0x15ab7ad,
    0x15ac426, 0x15add3f, 0x15ae37b, 0x15aea99, 0x15af002, 0x15af6ed, 0x15afd02, 0x15b0158,
    0x15b083d, 0x15b0e4a, 0x15b136b, 0x15b1894, 0x15b6d97, 0x161f7ab, 0x16631c3, 0x1741329,
    0x1aeec5e,
    };
    for (const u32 offset : kRdtscSites) {
        ++candidates;
        const u8* q = reinterpret_cast<const u8*>(0x800000000ull + offset);
        if (GuestReadable(q, 2) && q[0] == 0x0F && q[1] == 0x31) {
            sites.push_back(reinterpret_cast<uintptr_t>(q));
        }
    }
    std::sort(sites.begin(), sites.end());
    trap->sites = std::move(sites);
    trap->counts = std::make_unique<std::atomic<u64>[]>(trap->sites.size() + 1);
    const u64 host = clock ? clock->GetTscFrequency() : 0;
    const u64 den = host != 0 ? host : 1;
    const u64 target_num = hz >= 1e8 ? static_cast<u64>(hz) : den;
    // SHADPS4_RDTSC_SWITCH_S=<seconds>: run at the host's own rate first, then change to the
    // wanted one after that long (to compare the two in one run).
    const char* switch_text = std::getenv("SHADPS4_RDTSC_SWITCH_S");
    const double switch_after = switch_text != nullptr ? std::atof(switch_text) : 0.0;
    const u64 start = __rdtsc();
    trap->params.store(new RdtscParams{start, start, switch_after > 0 ? den : target_num, den});
    g_trap = trap;
    AddVectoredExceptionHandler(1, RdtscHandler);
    u32 patched = 0;
    for (const uintptr_t site : trap->sites) {
        void* p = reinterpret_cast<void*>(site);
        DWORD old = 0;
        if (VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) {
            u8* b = static_cast<u8*>(p);
            if (b[0] == 0x0F && b[1] == 0x31) {
                b[0] = 0x0F;
                b[1] = 0x0B; // ud2
                ++patched;
            }
            DWORD ignored = 0;
            VirtualProtect(p, 2, old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), p, 2);
        }
    }
    LOG_INFO(Lib_Kernel,
             "rdtsc trap: {} of {} known rdtsc instructions found, {} patched; the "
             "title's counter will run at {:.0f} Hz (host {} Hz, SHADPS4_RDTSC_HZ){}",
             trap->sites.size(), candidates, patched, static_cast<double>(target_num), den,
             switch_after > 0 ? fmt::format(", after {:.0f} s at the host's own rate", switch_after)
                              : std::string{});
    // The report (and the switch of SHADPS4_RDTSC_SWITCH_S) is a diagnostic: it runs only with
    // SHADPS4_FRAME_STATS or SHADPS4_RDTSC_REPORT set.
    if (std::getenv("SHADPS4_FRAME_STATS") == nullptr && std::getenv("SHADPS4_RDTSC_REPORT") == nullptr &&
        switch_after <= 0) {
        return;
    }
    std::thread([trap, switch_after, target_num, den] {
        Common::SetCurrentThreadName("RdtscReport");
        std::vector<u64> last(trap->sites.size() + 1, 0);
        u64 last_total = 0;
        double elapsed = 0.0;
        bool switched = switch_after <= 0;
        struct TimerSeen {
            u64 virtual_ticks = 0;
            bool valid = false;
        };
        TimerSeen seen[16]{};
        auto last_time = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const auto now_time = std::chrono::steady_clock::now();
            const double real = std::chrono::duration<double>(now_time - last_time).count();
            last_time = now_time;
            elapsed += real;
            if (!switched && elapsed >= switch_after) {
                switched = true;
                const RdtscParams* old = trap->params.load();
                const u64 now = __rdtsc();
                trap->params.store(new RdtscParams{now, GuestTicks(*old, now), target_num, den},
                                   std::memory_order_release);
                LOG_INFO(Lib_Kernel, "rdtsc trap: the title's counter now runs at {} Hz", target_num);
            }
            const u64 total = trap->total.load();
            std::vector<std::pair<u64, size_t>> top;
            for (size_t i = 0; i < trap->sites.size(); ++i) {
                const u64 now = trap->counts[i].load();
                if (now != last[i]) {
                    top.emplace_back(now - last[i], i);
                    last[i] = now;
                }
            }
            std::sort(top.rbegin(), top.rend());
            std::string text = fmt::format("rdtsc trap: {} reads in the last 5 s from {} places",
                                           total - last_total, top.size());
            last_total = total;
            for (size_t i = 0; i < top.size() && i < 8; ++i) {
                text += fmt::format("\n  {} x {}", top[i].first,
                                    Core::GuestCpu::GuestWaitCaller(
                                        reinterpret_cast<const void*>(trap->sites[top[i].second])));
            }
            // How fast each of the title's timers advances (game seconds per real second); the
            // title converts its counter at 1/1.594 GHz.
            const u32 n = std::min<u32>(trap->timer_count.load(), 16);
            for (u32 i = 0; i < n; ++i) {
                const uintptr_t timer = trap->timers[i].load();
                if (!GuestReadable(reinterpret_cast<const void*>(timer), 0x34)) {
                    continue;
                }
                const u64 ticks = *reinterpret_cast<const u64*>(timer + 8);
                float scale = 0.0f;
                std::memcpy(&scale, reinterpret_cast<const void*>(timer + 0x18), sizeof(scale));
                if (seen[i].valid && real > 0.0) {
                    text += fmt::format("\n  timer {:#x}: {:.3f} game seconds per real second "
                                        "(scale {:.3f})",
                                        timer,
                                        static_cast<double>(ticks - seen[i].virtual_ticks) *
                                            6.273525587907613e-10 / real,
                                        scale);
                }
                seen[i].virtual_ticks = ticks;
                seen[i].valid = true;
            }
            LOG_INFO(Lib_Kernel, "{}", text);
        }
    }).detach();
}
} // namespace
#endif

/// Until Dawn: Rush of Blood (US 1.00) times its frames, animations and captions by reading the
/// processor's time stamp counter itself, and converts the ticks to seconds with the console's
/// 1.594 GHz built in. The counter of a PC runs faster (2.4 GHz here), so everything the game
/// times ran about 1.5 times as fast as it should: the captions finished far ahead of the
/// voice, which is played in real time. Every place that reads the counter is made to see one
/// that runs at the console's rate. SHADPS4_RDTSC_HZ=<hz> sets another rate, 0 leaves it alone.
void InstallGuestClockFix() {
#ifdef _WIN64
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA03683") {
        return;
    }
    const char* setting = std::getenv("SHADPS4_RDTSC_HZ");
    const double hz = setting != nullptr ? std::atof(setting) : 1594000000.0;
    if (setting != nullptr && hz == 0.0 && setting[0] == '0') {
        LOG_INFO(Lib_Kernel, "The game's own clock is left as the PC's (SHADPS4_RDTSC_HZ=0)");
        return;
    }
    InstallRdtscTrap(hz);
#endif
}

void GuestCodeProbe() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    if (const char* spec = std::getenv("SHADPS4_DUMP_CODE"); spec != nullptr) {
        std::string text{spec};
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find_first_of(";,", pos);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string item = text.substr(pos, end - pos);
            pos = end + 1;
            unsigned long long address = 0;
            unsigned before = 0;
            unsigned after = 0;
            if (std::sscanf(item.c_str(), "%llx:%u:%u", &address, &before, &after) == 3 &&
                before + after <= 4096) {
                const auto* p = reinterpret_cast<const u8*>(static_cast<uintptr_t>(address));
                if (GuestReadable(p - before, before + after)) {
                    DumpGuestCode("probe", p, before, after);
                } else {
                    LOG_INFO(Lib_Kernel, "probe: {:#x} is not readable", address);
                }
            }
        }
    }
#ifdef _WIN64
    if (const char* scan = std::getenv("SHADPS4_SCAN_RDTSC"); scan != nullptr && scan[0] != '0') {
        std::string out;
        u32 hits = 0;
        u32 total = 0;
        const u8* address = reinterpret_cast<const u8*>(0x800000000ull);
        const u8* const limit = reinterpret_cast<const u8*>(0x810000000ull);
        while (address < limit) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(address, &info, sizeof(info)) == 0) {
                break;
            }
            const u8* base = static_cast<const u8*>(info.BaseAddress);
            const u8* region_end = base + info.RegionSize;
            const bool code = info.State == MEM_COMMIT &&
                              (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 &&
                              (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
            if (code) {
                for (const u8* q = base; q + 3 <= region_end; ++q) {
                    if (q[0] == 0x0F && (q[1] == 0x31 || (q[1] == 0x01 && q[2] == 0xF9))) {
                        ++total;
                        if (hits < 400) {
                            ++hits;
                            out += fmt::format("  {:#x} {}:", reinterpret_cast<uintptr_t>(q),
                                               q[1] == 0x31 ? "rdtsc" : "rdtscp");
                            const u8* from = q - 12 >= base ? q - 12 : base;
                            for (const u8* b = from; b < q + 8 && b < region_end; ++b) {
                                out += fmt::format(" {:02x}", *b);
                            }
                            out += "\n";
                        }
                    }
                }
            }
            address = region_end;
        }
        LOG_INFO(Lib_Kernel, "rdtsc / rdtscp in the title's executable memory: {} found\n{}",
                 total, out);
    }
#endif
}

u64 PS4_SYSV_ABI sceKernelGetTscFrequency() {
    static std::atomic<u32> calls{0};
    const double scale = TscScale();
    const u64 frequency = clock ? static_cast<u64>(clock->GetTscFrequency() * scale) : 0;
    if (calls.fetch_add(1) < 10) {
        LOG_INFO(Lib_Kernel, "sceKernelGetTscFrequency called by {}: {} Hz",
                 Common::GetCurrentThreadName(), frequency);
    }
    return frequency;
}

u64 PS4_SYSV_ABI sceKernelGetProcessTime() {
    ApiCallNote("sceKernelGetProcessTime");
    // TODO: this timer should support suspends, so initial ptc needs to be updated on wake up
    return clock ? clock->GetTimeUS(initial_ptc) : 0;
}

u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() {
    ApiCallNote("sceKernelGetProcessTimeCounter");
    return clock ? clock->GetUptime() - initial_ptc : 0;
}

u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() {
    static std::atomic<u32> calls{0};
    if (calls.fetch_add(1) < 10) {
        LOG_INFO(Lib_Kernel, "sceKernelGetProcessTimeCounterFrequency called by {}: {} Hz",
                 Common::GetCurrentThreadName(), clock ? clock->GetTscFrequency() : 1);
    }
    return clock ? clock->GetTscFrequency() : 1;
}

u64 PS4_SYSV_ABI sceKernelReadTsc() {
    ApiCallNote("sceKernelReadTsc");
    {
        // The first few places the game's own threads read the counter from.
        thread_local const void* last_caller = nullptr;
        const void* caller = __builtin_return_address(0);
        if (caller != last_caller) {
            last_caller = caller;
            static std::mutex seen_mutex;
            static std::vector<const void*> seen;
            std::scoped_lock lock{seen_mutex};
            if (seen.size() < 6 && std::ranges::find(seen, caller) == seen.end() &&
                reinterpret_cast<uintptr_t>(caller) >= 0x800000000ull &&
                reinterpret_cast<uintptr_t>(caller) < 0x810000000ull) {
                seen.push_back(caller);
                DumpGuestCode("sceKernelReadTsc", caller, 192, 192);
            }
        }
    }
    if (!clock) {
        return 0;
    }
    const double scale = TscScale();
    const u64 uptime = clock->GetUptime();
    return scale == 1.0 ? uptime : static_cast<u64>(static_cast<double>(uptime) * scale);
}

static s32 posix_nanosleep_impl(const OrbisKernelTimespec* rqtp, OrbisKernelTimespec* rmtp,
                                const bool interruptible) {
    if (!rqtp || rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec >= 1'000'000'000) {
        SetPosixErrno(EINVAL);
        return -1;
    }
    const auto duration = std::chrono::nanoseconds(rqtp->tv_sec * 1'000'000'000 + rqtp->tv_nsec);
    std::chrono::nanoseconds remain;
    const auto uninterrupted = Common::AccurateSleep(duration, &remain, interruptible);
    if (rmtp) {
        rmtp->tv_sec = remain.count() / 1'000'000'000;
        rmtp->tv_nsec = remain.count() % 1'000'000'000;
    }
    if (!uninterrupted) {
        SetPosixErrno(EINTR);
        return -1;
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_nanosleep(const OrbisKernelTimespec* rqtp, OrbisKernelTimespec* rmtp) {
    GUEST_WAIT("nanosleep", 0, 0);
    return posix_nanosleep_impl(rqtp, rmtp, true);
}

s32 PS4_SYSV_ABI sceKernelNanosleep(const OrbisKernelTimespec* rqtp, OrbisKernelTimespec* rmtp) {
    GUEST_WAIT("sceKernelNanosleep", 0, 0);
    if (const auto ret = posix_nanosleep_impl(rqtp, rmtp, false); ret < 0) {
        return ErrnoToSceKernelError(*__Error());
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_usleep(u32 microseconds) {
    GUEST_WAIT("usleep", 0, 0);
    const OrbisKernelTimespec ts = {
        .tv_sec = microseconds / 1'000'000,
        .tv_nsec = (microseconds % 1'000'000) * 1'000,
    };
    return posix_nanosleep(&ts, nullptr);
}

s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    GUEST_WAIT("sceKernelUsleep", 0, 0);
    {
        static std::atomic<u32> scaleform_sleeps{0};
        static std::atomic<bool> dumped{false};
        const void* caller = __builtin_return_address(0);
        if (scaleform_sleeps.load() < 8 && Common::GetCurrentThreadName() == "Scaleform::Setup") {
            scaleform_sleeps.fetch_add(1);
            LOG_INFO(Lib_Kernel, "Scaleform::Setup sleeps {} us from {}", microseconds,
                     Core::GuestCpu::GuestWaitCaller(caller));
            if (!dumped.exchange(true)) {
                DumpGuestCode("sceKernelUsleep of Scaleform::Setup", caller, 1536, 256);
            }
        }
    }
    const OrbisKernelTimespec ts = {
        .tv_sec = microseconds / 1'000'000,
        .tv_nsec = (microseconds % 1'000'000) * 1'000,
    };
    return sceKernelNanosleep(&ts, nullptr);
}

u32 PS4_SYSV_ABI posix_sleep(u32 seconds) {
    const OrbisKernelTimespec ts = {
        .tv_sec = seconds,
        .tv_nsec = 0,
    };
    OrbisKernelTimespec rm;
    if (const auto ret = posix_nanosleep(&ts, &rm); ret < 0) {
        return *__Error() == POSIX_EINTR ? rm.tv_sec + (rm.tv_nsec == 0 ? 0 : 1) : seconds;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelSleep(u32 seconds) {
    GUEST_WAIT("sceKernelSleep", 0, 0);
    return sceKernelUsleep(seconds * 1'000'000);
}

s32 PS4_SYSV_ABI posix_clock_gettime(u32 clock_id, OrbisKernelTimespec* ts) {
    ApiCallNote("clock_gettime");
    if (ts == nullptr) {
        SetPosixErrno(EFAULT);
        return -1;
    }

    if (clock_id == ORBIS_CLOCK_PROCTIME) {
        const auto us = sceKernelGetProcessTime();
        ts->tv_sec = static_cast<s64>(us / 1'000'000);
        ts->tv_nsec = static_cast<s64>((us % 1'000'000) * 1000);
        return 0;
    }
    if (clock_id == ORBIS_CLOCK_EXT_NETWORK || clock_id == ORBIS_CLOCK_EXT_DEBUG_NETWORK ||
        clock_id == ORBIS_CLOCK_EXT_AD_NETWORK || clock_id == ORBIS_CLOCK_EXT_RAW_NETWORK) {
        LOG_ERROR(Lib_Kernel, "Unsupported clock type {}, using CLOCK_MONOTONIC", clock_id);
        clock_id = ORBIS_CLOCK_MONOTONIC;
    }

#ifdef _WIN32
    static const auto FileTimeTo100Ns = [](FILETIME& ft) { return *reinterpret_cast<u64*>(&ft); };
    switch (clock_id) {
    case ORBIS_CLOCK_REALTIME:
    case ORBIS_CLOCK_REALTIME_PRECISE: {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        static constexpr u64 DeltaEpochIn100ns = 116444736000000000ULL;
        const u64 ns = FileTimeTo100Ns(ft) - DeltaEpochIn100ns;
        ts->tv_sec = ns / 10'000'000;
        ts->tv_nsec = (ns % 10'000'000) * 100;
        return 0;
    }
    case ORBIS_CLOCK_SECOND:
    case ORBIS_CLOCK_REALTIME_FAST: {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        static constexpr u64 DeltaEpochIn100ns = 116444736000000000ULL;
        const u64 ns = FileTimeTo100Ns(ft) - DeltaEpochIn100ns;
        ts->tv_sec = ns / 10'000'000;
        ts->tv_nsec = (ns % 10'000'000) * 100;
        return 0;
    }
    case ORBIS_CLOCK_UPTIME:
    case ORBIS_CLOCK_UPTIME_PRECISE:
    case ORBIS_CLOCK_MONOTONIC:
    case ORBIS_CLOCK_MONOTONIC_PRECISE:
    case ORBIS_CLOCK_UPTIME_FAST:
    case ORBIS_CLOCK_MONOTONIC_FAST: {
        static LARGE_INTEGER pf = [] {
            LARGE_INTEGER res{};
            QueryPerformanceFrequency(&res);
            return res;
        }();

        LARGE_INTEGER pc{};
        if (!QueryPerformanceCounter(&pc)) {
            SetPosixErrno(EFAULT);
            return -1;
        }
        ts->tv_sec = pc.QuadPart / pf.QuadPart;
        ts->tv_nsec = ((pc.QuadPart % pf.QuadPart) * 1000'000'000) / pf.QuadPart;
        return 0;
    }
    case ORBIS_CLOCK_THREAD_CPUTIME_ID: {
        FILETIME ct, et, kt, ut;
        if (!GetThreadTimes(GetCurrentThread(), &ct, &et, &kt, &ut)) {
            SetPosixErrno(EFAULT);
            return -1;
        }
        const u64 ns = FileTimeTo100Ns(ut) + FileTimeTo100Ns(kt);
        ts->tv_sec = ns / 10'000'000;
        ts->tv_nsec = (ns % 10'000'000) * 100;
        return 0;
    }
    case ORBIS_CLOCK_VIRTUAL: {
        FILETIME ct, et, kt, ut;
        if (!GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
            SetPosixErrno(EFAULT);
            return -1;
        }
        const u64 ns = FileTimeTo100Ns(ut);
        ts->tv_sec = ns / 10'000'000;
        ts->tv_nsec = (ns % 10'000'000) * 100;
        return 0;
    }
    case ORBIS_CLOCK_PROF: {
        FILETIME ct, et, kt, ut;
        if (!GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
            SetPosixErrno(EFAULT);
            return -1;
        }
        const u64 ns = FileTimeTo100Ns(kt);
        ts->tv_sec = ns / 10'000'000;
        ts->tv_nsec = (ns % 10'000'000) * 100;
        return 0;
    }
    default:
        SetPosixErrno(EFAULT);
        return -1;
    }
#else
    clockid_t pclock_id;
    switch (clock_id) {
    case ORBIS_CLOCK_REALTIME:
    case ORBIS_CLOCK_REALTIME_PRECISE:
        pclock_id = CLOCK_REALTIME;
        break;
    case ORBIS_CLOCK_SECOND:
    case ORBIS_CLOCK_REALTIME_FAST:
#ifdef CLOCK_REALTIME_COARSE
        pclock_id = CLOCK_REALTIME_COARSE;
#else
        pclock_id = CLOCK_REALTIME;
#endif
        break;
    case ORBIS_CLOCK_UPTIME:
    case ORBIS_CLOCK_UPTIME_PRECISE:
    case ORBIS_CLOCK_MONOTONIC:
    case ORBIS_CLOCK_MONOTONIC_PRECISE:
        pclock_id = CLOCK_MONOTONIC;
        break;
    case ORBIS_CLOCK_UPTIME_FAST:
    case ORBIS_CLOCK_MONOTONIC_FAST:
#ifdef CLOCK_MONOTONIC_COARSE
        pclock_id = CLOCK_MONOTONIC_COARSE;
#else
        pclock_id = CLOCK_MONOTONIC;
#endif
        break;
    case ORBIS_CLOCK_THREAD_CPUTIME_ID:
        pclock_id = CLOCK_THREAD_CPUTIME_ID;
        break;
    case ORBIS_CLOCK_VIRTUAL: {
        rusage ru;
        const auto res = getrusage(RUSAGE_SELF, &ru);
        if (res < 0) {
            SetPosixErrno(EFAULT);
            return -1;
        }
        ts->tv_sec = ru.ru_utime.tv_sec;
        ts->tv_nsec = ru.ru_utime.tv_usec * 1000;
        return 0;
    }
    case ORBIS_CLOCK_PROF: {
        rusage ru;
        const auto res = getrusage(RUSAGE_SELF, &ru);
        if (res < 0) {
            SetPosixErrno(EFAULT);
            return -1;
        }
        ts->tv_sec = ru.ru_stime.tv_sec;
        ts->tv_nsec = ru.ru_stime.tv_usec * 1000;
        return 0;
    }
    default:
        SetPosixErrno(EFAULT);
        return -1;
    }

    timespec t{};
    const auto result = clock_gettime(pclock_id, &t);
    ts->tv_sec = t.tv_sec;
    ts->tv_nsec = t.tv_nsec;
    if (result < 0) {
        SetPosixErrno(errno);
        return -1;
    }
    return 0;
#endif
}

s32 PS4_SYSV_ABI sceKernelClockGettime(const u32 clock_id, OrbisKernelTimespec* ts) {
    if (const auto ret = posix_clock_gettime(clock_id, ts); ret < 0) {
        return ErrnoToSceKernelError(*__Error());
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_clock_getres(u32 clock_id, OrbisKernelTimespec* res) {
    if (res == nullptr) {
        SetPosixErrno(EFAULT);
        return -1;
    }

    if (clock_id == ORBIS_CLOCK_EXT_NETWORK || clock_id == ORBIS_CLOCK_EXT_DEBUG_NETWORK ||
        clock_id == ORBIS_CLOCK_EXT_AD_NETWORK || clock_id == ORBIS_CLOCK_EXT_RAW_NETWORK) {
        LOG_ERROR(Lib_Kernel, "Unsupported clock type {}, using CLOCK_MONOTONIC", clock_id);
        clock_id = ORBIS_CLOCK_MONOTONIC;
    }

#ifdef _WIN32
    switch (clock_id) {
    case ORBIS_CLOCK_SECOND:
    case ORBIS_CLOCK_REALTIME_FAST: {
        DWORD timeAdjustment;
        DWORD timeIncrement;
        BOOL isTimeAdjustmentDisabled;
        if (!GetSystemTimeAdjustment(&timeAdjustment, &timeIncrement, &isTimeAdjustmentDisabled)) {
            SetPosixErrno(EFAULT);
            return -1;
        }
        res->tv_sec = 0;
        res->tv_nsec = timeIncrement * 100;
        return 0;
    }
    case ORBIS_CLOCK_REALTIME:
    case ORBIS_CLOCK_REALTIME_PRECISE:
    case ORBIS_CLOCK_UPTIME:
    case ORBIS_CLOCK_UPTIME_PRECISE:
    case ORBIS_CLOCK_MONOTONIC:
    case ORBIS_CLOCK_MONOTONIC_PRECISE:
    case ORBIS_CLOCK_UPTIME_FAST:
    case ORBIS_CLOCK_MONOTONIC_FAST: {
        LARGE_INTEGER pf;
        if (!QueryPerformanceFrequency(&pf)) {
            SetPosixErrno(EFAULT);
            return -1;
        }
        res->tv_sec = 0;
        res->tv_nsec =
            std::max(static_cast<s32>((1000000000 + (pf.QuadPart >> 1)) / pf.QuadPart), 1);
        return 0;
    }
    default:
        UNREACHABLE();
    }
#else
    clockid_t pclock_id;
    switch (clock_id) {
    case ORBIS_CLOCK_REALTIME:
    case ORBIS_CLOCK_REALTIME_PRECISE:
        pclock_id = CLOCK_REALTIME;
        break;
    case ORBIS_CLOCK_SECOND:
    case ORBIS_CLOCK_REALTIME_FAST:
#ifdef CLOCK_REALTIME_COARSE
        pclock_id = CLOCK_REALTIME_COARSE;
#else
        pclock_id = CLOCK_REALTIME;
#endif
        break;
    case ORBIS_CLOCK_UPTIME:
    case ORBIS_CLOCK_UPTIME_PRECISE:
    case ORBIS_CLOCK_MONOTONIC:
    case ORBIS_CLOCK_MONOTONIC_PRECISE:
        pclock_id = CLOCK_MONOTONIC;
        break;
    case ORBIS_CLOCK_UPTIME_FAST:
    case ORBIS_CLOCK_MONOTONIC_FAST:
#ifdef CLOCK_MONOTONIC_COARSE
        pclock_id = CLOCK_MONOTONIC_COARSE;
#else
        pclock_id = CLOCK_MONOTONIC;
#endif
        break;
    default:
        UNREACHABLE();
    }

    timespec t{};
    const auto result = clock_getres(pclock_id, &t);
    res->tv_sec = t.tv_sec;
    res->tv_nsec = t.tv_nsec;
    if (result < 0) {
        SetPosixErrno(errno);
        return -1;
    }
    return 0;
#endif
}

s32 PS4_SYSV_ABI sceKernelClockGetres(const u32 clock_id, OrbisKernelTimespec* res) {
    if (const auto ret = posix_clock_getres(clock_id, res); ret < 0) {
        return ErrnoToSceKernelError(*__Error());
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_gettimeofday(OrbisKernelTimeval* tp, OrbisKernelTimezone* tz) {
    ApiCallNote("gettimeofday");
#ifdef _WIN64
    if (tp) {
        FILETIME filetime;
        GetSystemTimePreciseAsFileTime(&filetime);

        constexpr u64 UNIX_TIME_START = 0x295E9648864000;
        constexpr u64 TICKS_PER_SECOND = 1000000;

        u64 ticks = filetime.dwHighDateTime;
        ticks <<= 32;
        ticks |= filetime.dwLowDateTime;
        ticks /= 10;
        ticks -= UNIX_TIME_START;

        tp->tv_sec = ticks / TICKS_PER_SECOND;
        tp->tv_usec = ticks % TICKS_PER_SECOND;
    }
    if (tz) {
        static int tzflag = 0;
        if (!tzflag) {
            _tzset();
            tzflag++;
        }
        tz->tz_minuteswest = _timezone / 60;
        tz->tz_dsttime = _daylight;
    }
    return 0;
#else
    struct timezone tzz;
    timeval tv;
    const auto ret = gettimeofday(&tv, &tzz);
    if (tp) {
        tp->tv_sec = tv.tv_sec;
        tp->tv_usec = tv.tv_usec;
    }
    if (tz) {
        tz->tz_dsttime = tzz.tz_dsttime;
        tz->tz_minuteswest = tzz.tz_minuteswest;
    }
    if (ret < 0) {
        SetPosixErrno(errno);
        return -1;
    }
    return 0;
#endif
}

s32 PS4_SYSV_ABI sceKernelGettimeofday(OrbisKernelTimeval* tp) {
    if (const auto ret = posix_gettimeofday(tp, nullptr); ret < 0) {
        return ErrnoToSceKernelError(*__Error());
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGettimezone(OrbisKernelTimezone* tz) {
    if (const auto ret = posix_gettimeofday(nullptr, tz); ret < 0) {
        return ErrnoToSceKernelError(*__Error());
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelConvertLocaltimeToUtc(time_t param_1, int64_t param_2, time_t* seconds,
                                                OrbisKernelTimezone* timezone, s32* dst_seconds) {
    LOG_INFO(Kernel, "called");
    if (timezone) {
        sceKernelGettimezone(timezone);
        param_1 -= (timezone->tz_minuteswest + timezone->tz_dsttime) * 60;
        if (seconds) {
            *seconds = param_1;
        }
        if (dst_seconds) {
            *dst_seconds = timezone->tz_dsttime * 60;
        }
    } else {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return ORBIS_OK;
}

namespace Dev {
u64& GetInitialPtc() {
    return initial_ptc;
}

Common::NativeClock* GetClock() {
    return clock.get();
}

} // namespace Dev

s32 PS4_SYSV_ABI sceKernelConvertUtcToLocaltime(time_t time, time_t* local_time,
                                                struct OrbisTimesec* st, u64* dst_sec) {
    LOG_TRACE(Kernel, "Called");
#ifdef _WIN32
    TIME_ZONE_INFORMATION tz{};
    DWORD res = GetTimeZoneInformation(&tz);
    *local_time = time - tz.Bias;

    if (st != nullptr) {
        st->t = time;
        st->west_sec = -tz.Bias * 60;
        st->dst_sec = res == TIME_ZONE_ID_DAYLIGHT ? -_dstbias : 0;
    }

    if (dst_sec != nullptr) {
        *dst_sec = res == TIME_ZONE_ID_DAYLIGHT ? -_dstbias : 0;
    }
#else
#if defined(__APPLE__) || defined(__FreeBSD__)
    // std::chrono::current_zone() not available yet.
    const auto* time_zone = date::current_zone();
#else
    const auto* time_zone = std::chrono::current_zone();
#endif // __APPLE__
    auto info = time_zone->get_info(std::chrono::system_clock::now());

    *local_time = info.offset.count() + info.save.count() * 60 + time;

    if (st != nullptr) {
        st->t = time;
        st->west_sec = info.offset.count();
        st->dst_sec = info.save.count() * 60;
    }

    if (dst_sec != nullptr) {
        *dst_sec = info.save.count() * 60;
    }
#endif // _WIN32

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_clock_settime(s32 clock_id, OrbisKernelTimespec* tp) {
    LOG_ERROR(Lib_Kernel, "(STUBBED) called, clock_id: {}", clock_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_settimeofday(OrbisKernelTimeval* _tv, OrbisKernelTimezone* _tz) {
    LOG_ERROR(Lib_Kernel, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelSettimeofday(OrbisKernelTimeval* _tv, OrbisKernelTimezone* _tz) {
    s32 ret = posix_settimeofday(_tv, _tz);
    if (ret < 0) {
        return ErrnoToSceKernelError(ret);
    }
    return ret;
}

void RegisterTime(Core::Loader::SymbolsResolver* sym) {
    clock = std::make_unique<Common::NativeClock>();
    initial_ptc = clock->GetUptime();

    // POSIX
    LIB_FUNCTION("NhpspxdjEKU", "libkernel", 1, "libkernel", posix_nanosleep);
    LIB_FUNCTION("NhpspxdjEKU", "libScePosix", 1, "libkernel", posix_nanosleep);
    LIB_FUNCTION("yS8U2TGCe1A", "libkernel", 1, "libkernel", posix_nanosleep);
    LIB_FUNCTION("yS8U2TGCe1A", "libScePosix", 1, "libkernel", posix_nanosleep);
    LIB_FUNCTION("QcteRwbsnV0", "libkernel", 1, "libkernel", posix_usleep);
    LIB_FUNCTION("QcteRwbsnV0", "libScePosix", 1, "libkernel", posix_usleep);
    LIB_FUNCTION("0wu33hunNdE", "libkernel", 1, "libkernel", posix_sleep);
    LIB_FUNCTION("0wu33hunNdE", "libScePosix", 1, "libkernel", posix_sleep);
    LIB_FUNCTION("lLMT9vJAck0", "libkernel", 1, "libkernel", posix_clock_gettime);
    LIB_FUNCTION("lLMT9vJAck0", "libScePosix", 1, "libkernel", posix_clock_gettime);
    LIB_FUNCTION("smIj7eqzZE8", "libkernel", 1, "libkernel", posix_clock_getres);
    LIB_FUNCTION("smIj7eqzZE8", "libScePosix", 1, "libkernel", posix_clock_getres);
    LIB_FUNCTION("n88vx3C5nW8", "libkernel", 1, "libkernel", posix_gettimeofday);
    LIB_FUNCTION("n88vx3C5nW8", "libScePosix", 1, "libkernel", posix_gettimeofday);
    LIB_FUNCTION("ChCOChPU-YM", "libkernel", 1, "libkernel", sceKernelSettimeofday);
    LIB_FUNCTION("VdXIDAbJ3tQ", "libScePosix", 1, "libkernel", posix_settimeofday);
    LIB_FUNCTION("d7nUj1LOdDU", "libScePosix", 1, "libkernel", posix_clock_settime);

    // Orbis
    LIB_FUNCTION("4J2sUJmuHZQ", "libkernel", 1, "libkernel", sceKernelGetProcessTime);
    LIB_FUNCTION("fgxnMeTNUtY", "libkernel", 1, "libkernel", sceKernelGetProcessTimeCounter);
    LIB_FUNCTION("BNowx2l588E", "libkernel", 1, "libkernel",
                 sceKernelGetProcessTimeCounterFrequency);
    LIB_FUNCTION("-2IRUCO--PM", "libkernel", 1, "libkernel", sceKernelReadTsc);
    LIB_FUNCTION("1j3S3n-tTW4", "libkernel", 1, "libkernel", sceKernelGetTscFrequency);
    LIB_FUNCTION("QvsZxomvUHs", "libkernel", 1, "libkernel", sceKernelNanosleep);
    LIB_FUNCTION("1jfXLRVzisc", "libkernel", 1, "libkernel", sceKernelUsleep);
    LIB_FUNCTION("-ZR+hG7aDHw", "libkernel", 1, "libkernel", sceKernelSleep);
    LIB_FUNCTION("QBi7HCK03hw", "libkernel", 1, "libkernel", sceKernelClockGettime);
    LIB_FUNCTION("wRYVA5Zolso", "libkernel", 1, "libkernel", sceKernelClockGetres);
    LIB_FUNCTION("ejekcaNQNq0", "libkernel", 1, "libkernel", sceKernelGettimeofday);
    LIB_FUNCTION("kOcnerypnQA", "libkernel", 1, "libkernel", sceKernelGettimezone);
    LIB_FUNCTION("0NTHN1NKONI", "libkernel", 1, "libkernel", sceKernelConvertLocaltimeToUtc);
    LIB_FUNCTION("-o5uEDpN+oY", "libkernel", 1, "libkernel", sceKernelConvertUtcToLocaltime);
}

} // namespace Libraries::Kernel
