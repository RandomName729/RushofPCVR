// DIAGNOSTIC (Until Dawn: Rush of Blood freeze): remembers what every game thread is waiting on, so
// the stall watchdog can print it. Header only.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/thread.h"
#include "common/types.h"

namespace Core::GuestCpu {

// Implemented in guest_watchdog.cpp (Windows only; elsewhere they do nothing).
void* CaptureCurrentThreadHandle();
std::string DescribeThreadContext(void* native_handle);

// Where a thread spends its blocked time: per wait site (what + calling guest address), kept per
// thread and written only by that thread. Read by TopSites() for the frame statistics.
struct GuestWaitSite {
    std::atomic<const char*> what{nullptr};
    std::atomic<const void*> caller{nullptr};
    std::atomic<u64> count{0};
    std::atomic<u64> ns{0};
    u64 reported_ns{0}; // only touched by the reporter
    u64 reported_count{0};
};

struct GuestWaitSlot {
    static constexpr size_t SiteCount = 64;
    GuestWaitSite sites[SiteCount];
    void NoteSite(const char* w, const void* c, s64 ns) {
        const auto h = (reinterpret_cast<std::uintptr_t>(c) * 0x9E3779B97F4A7C15ull) >> 58;
        for (size_t i = 0; i < 8; ++i) {
            GuestWaitSite& site = sites[(h + i) % SiteCount];
            const void* cur = site.caller.load(std::memory_order_relaxed);
            if (cur == nullptr) {
                site.what.store(w, std::memory_order_relaxed);
                site.caller.store(c, std::memory_order_relaxed);
                cur = c;
            }
            if (cur == c && site.what.load(std::memory_order_relaxed) == w) {
                site.count.fetch_add(1, std::memory_order_relaxed);
                site.ns.fetch_add(static_cast<u64>(ns), std::memory_order_relaxed);
                return;
            }
        }
    }
    std::string thread_name;
    void* native_handle{nullptr};
    std::atomic<const char*> what{nullptr}; // non-null while the thread is inside a traced wait
    std::atomic<u64> a{0};
    std::atomic<u64> b{0};
    std::atomic<const void*> caller{nullptr};
    std::atomic<s64> since_ns{0};
    std::atomic<const char*> last_what{nullptr}; // the last wait that finished
    std::atomic<const void*> last_caller{nullptr};
    std::atomic<s64> last_end_ns{0};
    std::atomic<u64> count{0};
    int depth{0}; // only touched by the owning thread
};

inline s64 GuestWaitNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The per-site accounting is only for the frame statistics (SHADPS4_FRAME_STATS).
inline bool GuestWaitStatsOn() {
    static const bool on = [] {
        const char* value = std::getenv("SHADPS4_FRAME_STATS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

inline std::string GuestWaitCaller(const void* p) {
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    char buf[64];
    if (a >= 0x800000000ull && a < 0x810000000ull) {
        std::snprintf(buf, sizeof(buf), "game+0x%llx", static_cast<unsigned long long>(a - 0x800000000ull));
    } else {
        std::snprintf(buf, sizeof(buf), "%#llx", static_cast<unsigned long long>(a));
    }
    return buf;
}

class GuestWaitTable {
public:
    static GuestWaitTable& Instance() {
        static GuestWaitTable table;
        return table;
    }

    GuestWaitSlot& Slot() {
        thread_local GuestWaitSlot* slot = Register();
        return *slot;
    }

    // For the frame statistics: the threads that were blocked for a noticeable part of the last
    // `seconds`, and the wait sites they spent it at.
    std::string TopSites(double seconds) {
        std::string out;
        std::scoped_lock lock{mutex};
        for (const auto& s : slots) {
            struct Row {
                u64 ns, count;
                const char* what;
                const void* caller;
            };
            std::vector<Row> rows;
            u64 total = 0;
            for (auto& site : s->sites) {
                const u64 ns = site.ns.load(std::memory_order_relaxed);
                const u64 count = site.count.load(std::memory_order_relaxed);
                if (ns > site.reported_ns) {
                    rows.push_back({ns - site.reported_ns, count - site.reported_count,
                                    site.what.load(), site.caller.load()});
                    total += ns - site.reported_ns;
                }
                site.reported_ns = ns;
                site.reported_count = count;
            }
            if (static_cast<double>(total) < seconds * 1e9 * 0.05) {
                continue;
            }
            std::sort(rows.begin(), rows.end(),
                      [](const Row& x, const Row& y) { return x.ns > y.ns; });
            char head[160];
            std::snprintf(head, sizeof(head), "  [%s] blocked %.0f%% of the time:",
                          s->thread_name.c_str(), static_cast<double>(total) / (seconds * 1e7));
            out += head;
            for (size_t i = 0; i < rows.size() && i < 4; ++i) {
                char part[200];
                std::snprintf(part, sizeof(part), " %s from %s %.0f ms (%llu waits);", rows[i].what,
                              GuestWaitCaller(rows[i].caller).c_str(),
                              static_cast<double>(rows[i].ns) / 1e6,
                              static_cast<unsigned long long>(rows[i].count));
                out += part;
            }
            out += "\n";
        }
        return out;
    }

    std::string Dump() {
        std::string out;
        const s64 now = GuestWaitNowNs();
        std::scoped_lock lock{mutex};
        for (const auto& s : slots) {
            char line[512];
            if (const char* w = s->what.load()) {
                std::snprintf(line, sizeof(line),
                              "  [%s] WAITING %lld ms in %s(a=%#llx, b=%#llx) called from %s\n",
                              s->thread_name.c_str(),
                              static_cast<long long>((now - s->since_ns.load()) / 1000000), w,
                              static_cast<unsigned long long>(s->a.load()),
                              static_cast<unsigned long long>(s->b.load()),
                              GuestWaitCaller(s->caller.load()).c_str());
            } else if (const char* lw = s->last_what.load()) {
                std::snprintf(line, sizeof(line),
                              "  [%s] not in a traced wait; last wait: %s from %s ended %lld ms ago "
                              "(%llu waits so far)\n",
                              s->thread_name.c_str(), lw,
                              GuestWaitCaller(s->last_caller.load()).c_str(),
                              static_cast<long long>((now - s->last_end_ns.load()) / 1000000),
                              static_cast<unsigned long long>(s->count.load()));
            } else {
                std::snprintf(line, sizeof(line), "  [%s] never entered a traced wait\n",
                              s->thread_name.c_str());
            }
            out += line;
            // A thread that is not blocked is running (maybe spinning): say where.
            if (!s->what.load()) {
                out += "      " + DescribeThreadContext(s->native_handle) + "\n";
            }
        }
        return out;
    }

private:
    GuestWaitSlot* Register() {
        auto slot = std::make_unique<GuestWaitSlot>();
        slot->thread_name = Common::GetCurrentThreadName();
        slot->native_handle = CaptureCurrentThreadHandle();
        std::scoped_lock lock{mutex};
        slots.push_back(std::move(slot));
        return slots.back().get();
    }

    std::mutex mutex;
    std::vector<std::unique_ptr<GuestWaitSlot>> slots;
};

struct ScopedGuestWait {
    GuestWaitSlot& slot;

    ScopedGuestWait(const char* what, u64 a, u64 b, const void* caller)
        : slot(GuestWaitTable::Instance().Slot()) {
        if (slot.depth++ == 0) {
            slot.a.store(a);
            slot.b.store(b);
            slot.caller.store(caller);
            slot.since_ns.store(GuestWaitNowNs());
            slot.what.store(what);
        }
    }

    ~ScopedGuestWait() {
        if (--slot.depth == 0) {
            const s64 end_ns = GuestWaitNowNs();
            slot.last_what.store(slot.what.load());
            slot.last_caller.store(slot.caller.load());
            slot.last_end_ns.store(end_ns);
            slot.count.fetch_add(1);
            if (GuestWaitStatsOn()) {
                slot.NoteSite(slot.what.load(), slot.caller.load(), end_ns - slot.since_ns.load());
            }
            slot.what.store(nullptr);
        }
    }

    ScopedGuestWait(const ScopedGuestWait&) = delete;
    ScopedGuestWait& operator=(const ScopedGuestWait&) = delete;
};

} // namespace Core::GuestCpu

// Use at the top of a blocking guest-facing function.
#define GUEST_WAIT(what, a, b)                                                                     \
    ::Core::GuestCpu::ScopedGuestWait guest_wait_scope_{what, static_cast<u64>(a),                 \
                                                        static_cast<u64>(b),                       \
                                                        __builtin_return_address(0)}
