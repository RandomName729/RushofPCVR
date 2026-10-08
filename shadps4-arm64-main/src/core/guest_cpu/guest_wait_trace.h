// DIAGNOSTIC (Until Dawn: Rush of Blood freeze): remembers what every game thread is waiting on, so
// the stall watchdog can print it. Header only.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

struct GuestWaitSlot {
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
            slot.last_what.store(slot.what.load());
            slot.last_caller.store(slot.caller.load());
            slot.last_end_ns.store(GuestWaitNowNs());
            slot.count.fetch_add(1);
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
