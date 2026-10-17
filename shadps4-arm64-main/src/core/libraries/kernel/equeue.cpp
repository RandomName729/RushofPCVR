// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "core/guest_cpu/guest_wait_trace.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

extern boost::asio::io_context io_context;
extern void KernelSignalRequest();

static std::unordered_map<s32, EqueueInternal*> kqueues;

// DIAGNOSTIC (SHADPS4_FRAME_STATS): which event queues the game's threads wait on, for how long,
// and how often (and how evenly) the events they wait for are raised.
namespace {
bool WaitStatsOn() {
    static const bool on = [] {
        const char* value = std::getenv("SHADPS4_FRAME_STATS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}
using StatClock = std::chrono::steady_clock;
struct WaitStat {
    u64 waits{};
    u64 empty{};
    double total{};
    double longest{};
    std::map<std::pair<u64, s16>, u64> got;
};
struct RaiseStat {
    u64 raised{};
    double gaps{};
    double longest_gap{};
    StatClock::time_point last{};
};
std::mutex wait_stats_mutex;
std::map<std::string, WaitStat> wait_stats;
std::map<std::tuple<std::string, u64, s16>, RaiseStat> raise_stats;
} // namespace


// DIAGNOSTIC (SHADPS4_EQ_TRACE=1): a timeline of the main thread's waits, the events raised and
// the title's submits, logged once, 25 s after the first of them.
namespace {
bool TraceOn() {
    static const bool on = [] {
        const char* value = std::getenv("SHADPS4_EQ_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}
std::mutex trace_mutex;
std::vector<std::pair<double, std::string>> trace_lines;
StatClock::time_point trace_first{};
StatClock::time_point trace_origin{};
bool trace_done = false;
} // namespace

void EqueueTraceNote(const std::string& what) {
    if (!TraceOn()) {
        return;
    }
    const auto now = StatClock::now();
    std::string dump;
    {
        std::scoped_lock lock{trace_mutex};
        if (trace_done) {
            return;
        }
        if (trace_first == StatClock::time_point{}) {
            trace_first = now;
        }
        if (now - trace_first < std::chrono::seconds{25}) {
            return;
        }
        if (trace_origin == StatClock::time_point{}) {
            trace_origin = now;
        }
        trace_lines.emplace_back(std::chrono::duration<double>(now - trace_origin).count() * 1e3,
                                 fmt::format("{}: {}", Common::GetCurrentThreadName(), what));
        if (trace_lines.size() < 700) {
            return;
        }
        trace_done = true;
        for (const auto& [t, line] : trace_lines) {
            dump += fmt::format("  {:9.2f} ms  {}\n", t, line);
        }
        trace_lines.clear();
    }
    LOG_INFO(Kernel_Event, "EQ_TRACE timeline:\n{}", dump);
}


// DIAGNOSTIC (SHADPS4_FRAME_STATS): how often each thread asks for the time, the vblank and
// flip status, and hands audio over.
namespace {
std::map<std::string, u64> api_calls;
} // namespace

void ApiCallNote(const char* name) {
    if (!WaitStatsOn()) {
        return;
    }
    // Counted per thread and handed over now and then: some of these are called thousands of
    // times a second.
    struct Local {
        std::map<const char*, u64> counts;
        StatClock::time_point flushed{};
        u32 since{};
    };
    thread_local Local local;
    ++local.counts[name];
    if (++local.since < 64) {
        return;
    }
    local.since = 0;
    const auto now = StatClock::now();
    if (now - local.flushed < std::chrono::milliseconds{250}) {
        return;
    }
    local.flushed = now;
    const std::string thread = Common::GetCurrentThreadName();
    std::scoped_lock lock{wait_stats_mutex};
    for (auto& [what, count] : local.counts) {
        api_calls[fmt::format("{} {}", thread, what)] += count;
        count = 0;
    }
}

std::string EqueueWaitReport(double seconds) {
    std::map<std::string, WaitStat> waits;
    std::map<std::tuple<std::string, u64, s16>, RaiseStat> raises;
    std::map<std::string, u64> calls;
    {
        std::scoped_lock lock{wait_stats_mutex};
        waits.swap(wait_stats);
        raises.swap(raise_stats);
        calls.swap(api_calls);
    }
    std::string out;
    for (const auto& [key, count] : calls) {
        if (count >= 20) {
            out += fmt::format("  calls: {}: {} ({:.1f} a second)\n", key, count, count / seconds);
        }
    }
    for (const auto& [key, stat] : waits) {
        std::string got;
        for (const auto& [event, count] : stat.got) {
            got += fmt::format(" id {:#x} filter {}: {};", event.first, event.second, count);
        }
        out += fmt::format("  {}: {} waits, {:.1f} ms on average, longest {:.1f} ms, {:.0f} ms "
                           "in all ({:.0f}% of the time); events received:{}\n",
                           key, stat.waits, stat.waits != 0 ? stat.total / stat.waits * 1e3 : 0.0,
                           stat.longest * 1e3, stat.total * 1e3, 100.0 * stat.total / seconds,
                           got.empty() ? " none" : got);
    }
    for (const auto& [key, stat] : raises) {
        const u64 gaps = stat.raised > 1 ? stat.raised - 1 : 0;
        out += fmt::format("  event raised on '{}' id {:#x} filter {}: {} times, {:.1f} ms apart on "
                           "average, longest gap {:.1f} ms\n",
                           std::get<0>(key), std::get<1>(key), std::get<2>(key), stat.raised,
                           gaps != 0 ? stat.gaps / gaps * 1e3 : 0.0, stat.longest_gap * 1e3);
    }
    return out;
}
static constexpr auto HrTimerSpinlockThresholdNs = 1200000u;

EqueueInternal* GetEqueue(OrbisKernelEqueue eq) {
    if (!kqueues.contains(eq)) {
        return nullptr;
    }
    return kqueues[eq];
}

static void HrTimerCallback(OrbisKernelEqueue eq, const OrbisKernelEvent& kevent) {
    if (kqueues.contains(eq)) {
        kqueues[eq]->TriggerEvent(kevent.ident, OrbisKernelEvent::Filter::HrTimer, kevent.udata);
    }
}

static void TimerCallback(OrbisKernelEqueue eq, const OrbisKernelEvent& kevent) {
    if (kqueues.contains(eq) && kqueues[eq]->EventExists(kevent.ident, kevent.filter)) {
        kqueues[eq]->TriggerEvent(kevent.ident, OrbisKernelEvent::Filter::Timer, kevent.udata);
        if (!(kevent.flags & OrbisKernelEvent::Flags::OneShot)) {
            // Reschedule the event for its next period.
            kqueues[eq]->ScheduleEvent(kevent.ident, kevent.filter, TimerCallback);
        }
    }
}

// Events are uniquely identified by id and filter.
bool EqueueInternal::AddEvent(EqueueEvent& event) {
    // Save id and filter before event is moved into m_events.
    const u64 id = event.event.ident;
    const auto filter = event.event.filter;

    {
        std::scoped_lock lock{m_mutex};

        // Calculate timer interval
        event.time_added = std::chrono::steady_clock::now();
        if (event.event.filter == OrbisKernelEvent::Filter::Timer) {
            // Set timer interval, this is stored in milliseconds for timers.
            event.timer_interval = std::chrono::milliseconds(event.event.data);
        } else if (event.event.filter == OrbisKernelEvent::Filter::HrTimer) {
            // Retrieve inputted time, this is stored in the bintime format.
            OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(event.event.data);

            // Convert the bintime format to a timespec.
            OrbisKernelTimespec ts;
            ts.tv_sec = time->sec;
            ts.tv_nsec = (1000000000 * (time->frac >> 32)) >> 32;

            // Then use the timespec to set the timer interval.
            event.timer_interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
        }

        // First, check if there's already an event with the same id and filter.
        const auto& find_it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
            return ev.event.ident == id && ev.event.filter == filter;
        });
        // If there is a duplicate event, we need to update that instead.
        if (find_it != m_events.cend()) {
            // Specifically, update user data and timer_interval.
            // Trigger status and event data should remain intact.
            auto& old_event = *find_it;
            old_event.timer_interval = event.timer_interval;
            old_event.event.udata = event.event.udata;
            return true;
        }

        // Clear input data from event.
        event.event.data = 0;

        // Remove add flag from event
        event.event.flags &= ~OrbisKernelEvent::Flags::Add;

        // Clear flag is appended to most event types internally.
        if (event.event.filter != OrbisKernelEvent::Filter::User) {
            event.event.flags |= OrbisKernelEvent::Flags::Clear;
        }

        const auto& it = std::ranges::find(m_events, event);
        if (it != m_events.cend()) {
            *it = std::move(event);
        } else {
            m_events.emplace_back(std::move(event));
        }
    }

    // Schedule callbacks for timer events
    if (filter == OrbisKernelEvent::Filter::Timer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::Timer, TimerCallback);
    } else if (filter == OrbisKernelEvent::Filter::HrTimer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::HrTimer, HrTimerCallback);
    }

    return true;
}

bool EqueueInternal::ScheduleEvent(u64 id, s16 filter,
                                   void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&)) {
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it == m_events.cend()) {
        return false;
    }

    const auto& event = *it;
    ASSERT(event.event.filter == OrbisKernelEvent::Filter::Timer ||
           event.event.filter == OrbisKernelEvent::Filter::HrTimer);

    if (!it->timer) {
        it->timer = std::make_unique<boost::asio::steady_timer>(io_context, event.timer_interval);
    } else {
        // If the timer already exists we are scheduling a reoccurrence after the next period.
        // Set the expiration time to the previous occurrence plus the period.
        it->timer->expires_at(it->timer->expiry() + event.timer_interval);
    }

    it->timer->async_wait(
        [this, event_data = event.event, callback](const boost::system::error_code& ec) {
            if (ec) {
                if (ec != boost::system::errc::operation_canceled) {
                    LOG_ERROR(Kernel_Event, "Timer callback error: {}", ec.message());
                } else {
                    // Timer was cancelled (removed) before it triggered
                    LOG_DEBUG(Kernel_Event, "Timer cancelled");
                }
                return;
            }
            callback(this->m_handle, event_data);
        });
    KernelSignalRequest();

    return true;
}

bool EqueueInternal::RemoveEvent(u64 id, s16 filter) {
    bool has_found = false;
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it != m_events.cend()) {
        m_events.erase(it);
        has_found = true;
    }
    return has_found;
}

int EqueueInternal::WaitForEvents(OrbisKernelEvent* ev, int num, const OrbisKernelUseconds* timo) {
    if (timo != nullptr && *timo == 0) {
        // Effectively acts as a poll; only events that have already
        // arrived at the time of this function call can be received
        return GetTriggeredEvents(ev, num);
    }
    const auto micros = timo ? *timo : 0u;

    if (HasSmallTimer()) {
        // If a small timer is set, just wait for it to expire.
        return WaitForSmallTimer(ev, num, micros);
    }

    int count = 0;

    const auto predicate = [&] {
        count = GetTriggeredEvents(ev, num);
        return count > 0;
    };

    if (micros == 0) {
        // Wait indefinitely for events
        std::unique_lock lock{m_mutex};
        m_cond.wait(lock, predicate);
    } else {
        // Wait up until the timeout value
        std::unique_lock lock{m_mutex};
        m_cond.wait_for(lock, std::chrono::microseconds(micros), predicate);
    }

    return count;
}

bool EqueueInternal::TriggerEvent(u64 ident, s16 filter, void* trigger_data) {
    bool has_found = false;
    if (TraceOn()) {
        EqueueTraceNote(fmt::format("raise '{}' id {:#x}", m_name, ident));
    }
    if (WaitStatsOn()) {
        const auto now = StatClock::now();
        std::scoped_lock stats_lock{wait_stats_mutex};
        auto& stat = raise_stats[{std::string{m_name}, ident, filter}];
        if (stat.raised != 0) {
            const double gap = std::chrono::duration<double>(now - stat.last).count();
            stat.gaps += gap;
            stat.longest_gap = std::max(stat.longest_gap, gap);
        }
        ++stat.raised;
        stat.last = now;
    }
    {
        std::scoped_lock lock{m_mutex};
        for (auto& event : m_events) {
            if (event.event.ident == ident && event.event.filter == filter) {
                if (filter == OrbisKernelEvent::Filter::VideoOut) {
                    event.TriggerDisplay(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::User) {
                    event.TriggerUser(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::Timer ||
                           filter == OrbisKernelEvent::Filter::HrTimer) {
                    event.TriggerTimer();
                } else {
                    event.Trigger(trigger_data);
                }
                has_found = true;
            }
        }
    }
    m_cond.notify_one();
    return has_found;
}

int EqueueInternal::GetTriggeredEvents(OrbisKernelEvent* ev, int num) {
    int count = 0;
    for (auto it = m_events.begin(); it != m_events.end();) {
        if (it->IsTriggered()) {
            ev[count++] = it->event;
            if (it->event.flags & OrbisKernelEvent::Flags::Clear) {
                it->Clear();
            }
            if (it->event.flags & OrbisKernelEvent::Flags::OneShot) {
                it = m_events.erase(it);
            } else {
                ++it;
            }

            if (count == num) {
                break;
            }
        } else {
            ++it;
        }
    }

    return count;
}

bool EqueueInternal::AddSmallTimer(EqueueEvent& ev) {
    // Retrieve inputted time, this is stored in the bintime format
    OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(ev.event.data);
    OrbisKernelTimespec ts;
    ts.tv_sec = time->sec;
    ts.tv_nsec = ((1000000000 * (time->frac >> 32)) >> 32);

    // Create the small timer
    SmallTimer st;
    st.event = ev.event;
    st.added = std::chrono::steady_clock::now();
    st.interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
    {
        std::scoped_lock lock{m_mutex};
        m_small_timers[st.event.ident] = std::move(st);
    }
    return true;
}

int EqueueInternal::WaitForSmallTimer(OrbisKernelEvent* ev, int num, u32 micros) {
    ASSERT(num >= 1);

    auto curr_clock = std::chrono::steady_clock::now();
    const auto wait_end_us = (micros == 0) ? std::chrono::steady_clock::time_point::max()
                                           : curr_clock + std::chrono::microseconds{micros};
    int count = 0;
    do {
        curr_clock = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock{m_mutex};
            for (auto it = m_small_timers.begin(); it != m_small_timers.end() && count < num;) {
                const SmallTimer& st = it->second;

                if (curr_clock - st.added >= st.interval) {
                    ev[count++] = st.event;
                    it = m_small_timers.erase(it);
                } else {
                    ++it;
                }
            }

            if (count > 0)
                return count;
        }
        std::this_thread::yield();
    } while (curr_clock < wait_end_us);

    return 0;
}

bool EqueueInternal::EventExists(u64 id, s16 filter) {
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });

    return it != m_events.cend();
}

s32 PS4_SYSV_ABI posix_kqueue() {
    // Reserve a file handle for the kqueue
    auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    s32 kqueue_handle = handles->CreateHandle();
    auto* kqueue_file = handles->GetFile(kqueue_handle);
    kqueue_file->type = Core::FileSys::FileType::Equeue;

    // Plenty of equeue logic uses names to identify queues.
    // Create a unique name for the queue we create.
    char name[32];
    memset(name, 0, sizeof(name));
    snprintf(name, sizeof(name), "kqueue%i", kqueue_handle);

    // Create the queue
    kqueues[kqueue_handle] = new EqueueInternal(kqueue_handle, name);
    LOG_INFO(Kernel_Event, "kqueue created with name {}", name);

    // Return handle.
    return kqueue_handle;
}

// Helper method to detect supported filters.
// We don't want to allow adding events we don't handle properly.
bool SupportedEqueueFilter(OrbisKernelEvent::Filter filter) {
    return filter == OrbisKernelEvent::Filter::GraphicsCore ||
           filter == OrbisKernelEvent::Filter::HrTimer ||
           filter == OrbisKernelEvent::Filter::Timer || filter == OrbisKernelEvent::Filter::User ||
           filter == OrbisKernelEvent::Filter::VideoOut;
}

s32 PS4_SYSV_ABI posix_kevent(s32 handle, OrbisKernelEvent* changelist, u64 nchanges,
                              OrbisKernelEvent* eventlist, u64 nevents,
                              OrbisKernelTimespec* timeout) {
    GUEST_WAIT("kevent", handle, nevents);
    LOG_INFO(Kernel_Event, "called, eq = {}, nchanges = {}, nevents = {}", handle, nchanges,
             nevents);

    // Get the equeue
    if (!kqueues.contains(handle)) {
        *__Error() = POSIX_EBADF;
        return ORBIS_FAIL;
    }
    auto equeue = kqueues[handle];

    // First step is to apply all changes in changelist.
    for (u64 i = 0; i < nchanges; i++) {
        auto event = changelist[i];
        if (!SupportedEqueueFilter(event.filter)) {
            LOG_ERROR(Kernel_Event, "Unsupported event filter {}",
                      magic_enum::enum_name(event.filter));
            continue;
        }

        // Check the event flags to determine the appropriate action
        if (event.flags & OrbisKernelEvent::Flags::Add) {
            // The caller is requesting to add an event.
            EqueueEvent internal_event{};
            internal_event.event = event;
            if (!equeue->AddEvent(internal_event)) {
                // Failed to add event, return error.
                *__Error() = POSIX_ENOMEM;
                return ORBIS_FAIL;
            }
        }

        if (event.flags & OrbisKernelEvent::Flags::Delete) {
            // The caller is requesting to remove an event.
            if (!equeue->RemoveEvent(event.ident, event.filter)) {
                // Failed to remove event, return error.
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        }

        if (event.filter == OrbisKernelEvent::Filter::User && event.fflags == 0x1000000) {
            // For user events, this fflags value indicates we need to trigger the event.
            if (!equeue->TriggerEvent(event.ident, OrbisKernelEvent::Filter::User, event.udata)) {
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        } else if (event.fflags != 0) {
            // The title is using filter-specific flags. Right now, these are unhandled.
            LOG_ERROR(Kernel_Event, "Unhandled fflags {:#x} for event filter {}", event.fflags,
                      magic_enum::enum_name(event.filter));
            continue;
        }
    }

    // Now we need to wait on the event list.
    s32 count = 0;
    if (nevents > 0) {
        if (timeout != nullptr) {
            OrbisKernelUseconds micros = (timeout->tv_sec * 1000000) + (timeout->tv_nsec / 1000);
            count = equeue->WaitForEvents(eventlist, nevents, &micros);
        } else {
            count = equeue->WaitForEvents(eventlist, nevents, nullptr);
        }
    }
    return count;
}

int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name) {
    if (eq == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (name == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue name is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Maximum is 32 including null terminator
    static constexpr u64 MaxEventQueueNameSize = 32;
    if (std::strlen(name) > MaxEventQueueNameSize) {
        LOG_ERROR(Kernel_Event, "Event queue name exceeds 32 bytes!");
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    LOG_INFO(Kernel_Event, "name = {}", name);

    // Reserve a file handle for the kqueue
    auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    OrbisKernelEqueue kqueue_handle = handles->CreateHandle();
    auto* kqueue_file = handles->GetFile(kqueue_handle);
    kqueue_file->type = Core::FileSys::FileType::Equeue;

    // Create the equeue
    kqueues[kqueue_handle] = new EqueueInternal(kqueue_handle, name);
    *eq = kqueue_handle;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue eq) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    handles->DeleteHandle(eq);
    delete kqueues[eq];
    kqueues.erase(eq);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                                     OrbisKernelUseconds* timo) {
    GUEST_WAIT("sceKernelWaitEqueue", eq, num);
    HLE_TRACE;
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    auto& equeue = kqueues[eq];

    TRACE_HINT(equeue->GetName());

    if (ev == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    if (num < 1) {
        *out = 0;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // DIAGNOSTIC (Rush of Blood freeze): trace waits on the game's "LL ..." event queue.
    const bool trace_wait = equeue->GetName().find("LL") != std::string_view::npos;
    if (trace_wait) {
        static std::atomic<u32> begins{0};
        if (begins.fetch_add(1) < 60) {
            LOG_WARNING(Kernel_Event, "EQ trace: wait on '{}' begins (num={})", equeue->GetName(),
                        num);
        }
    }

    const bool stats_on = WaitStatsOn();
    const bool trace_main = TraceOn() && Common::GetCurrentThreadName() == "Game:Main";
    if (trace_main) {
        EqueueTraceNote(fmt::format("wait begins on '{}'", equeue->GetName()));
    }
    const auto wait_began = stats_on ? StatClock::now() : StatClock::time_point{};
    *out = equeue->WaitForEvents(ev, num, timo);
    if (trace_main) {
        EqueueTraceNote(fmt::format("wait ends on '{}' ({} events)", equeue->GetName(), *out));
    }
    if (stats_on) {
        const double waited = std::chrono::duration<double>(StatClock::now() - wait_began).count();
        const std::string key = fmt::format("{} on '{}'", Common::GetCurrentThreadName(),
                                            equeue->GetName());
        std::scoped_lock stats_lock{wait_stats_mutex};
        auto& stat = wait_stats[key];
        ++stat.waits;
        stat.total += waited;
        stat.longest = std::max(stat.longest, waited);
        if (*out <= 0) {
            ++stat.empty;
        }
        for (int i = 0; i < *out; ++i) {
            ++stat.got[{ev[i].ident, static_cast<s16>(ev[i].filter)}];
        }
    }

    if (trace_wait) {
        static std::atomic<u32> ends{0};
        if (ends.fetch_add(1) < 60) {
            LOG_WARNING(Kernel_Event, "EQ trace: wait on '{}' returned {} event(s)",
                        equeue->GetName(), *out);
        }
    }

    if (*out == 0) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAddHRTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelTimespec* ts,
                                          void* udata) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    const auto total_ns = ts->tv_sec * 1000000000 + ts->tv_nsec;

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::HrTimer;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::OneShot;
    event.event.fflags = 0;
    // Data is stored as the address of a OrbisKernelBintime struct.
    OrbisKernelBintime time{ts->tv_sec, ts->tv_nsec * 0x44b82fa09};
    event.event.data = reinterpret_cast<u64>(&time);
    event.event.udata = udata;

    // HR timers cannot be implemented within the existing event queue architecture due to the
    // slowness of the notification mechanism. For instance, a 100us timer will lose its precision
    // as the trigger time drifts by +50-700%, depending on the host PC and workload. To address
    // this issue, we use a spinlock for small waits (which can be adjusted using
    // `HrTimerSpinlockThresholdUs`) and fall back to boost asio timers if the time to tick is
    // large. Even for large delays, we truncate a small portion to complete the wait
    // using the spinlock, prioritizing precision.
    auto& equeue = kqueues[eq];
    if (total_ns < HrTimerSpinlockThresholdNs) {
        return equeue->AddSmallTimer(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
    }

    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteHRTimerEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    auto& equeue = kqueues[eq];
    if (equeue->HasSmallTimer()) {
        return equeue->RemoveSmallTimer(id) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOENT;
    } else {
        return equeue->RemoveEvent(id, OrbisKernelEvent::Filter::HrTimer)
                   ? ORBIS_OK
                   : ORBIS_KERNEL_ERROR_ENOENT;
    }
}

int PS4_SYSV_ABI sceKernelAddTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelUseconds usec,
                                        void* udata) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = static_cast<u64>(id);
    event.event.filter = OrbisKernelEvent::Filter::Timer;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = usec / 1000;
    event.event.udata = udata;

    auto& equeue = kqueues[eq];
    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteTimerEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    return kqueues[eq]->RemoveEvent(id, OrbisKernelEvent::Filter::Timer)
               ? ORBIS_OK
               : ORBIS_KERNEL_ERROR_ENOENT;
}

int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = 0;

    return kqueues[eq]->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

int PS4_SYSV_ABI sceKernelAddUserEventEdge(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::Clear;
    event.event.fflags = 0;
    event.event.data = 0;

    return kqueues[eq]->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

void* PS4_SYSV_ABI sceKernelGetEventUserData(const OrbisKernelEvent* ev) {
    ASSERT(ev);
    return ev->udata;
}

u64 PS4_SYSV_ABI sceKernelGetEventId(const OrbisKernelEvent* ev) {
    return ev->ident;
}

int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue eq, int id, void* udata) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (!kqueues[eq]->TriggerEvent(id, OrbisKernelEvent::Filter::User, udata)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (!kqueues[eq]->RemoveEvent(id, OrbisKernelEvent::Filter::User)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelGetEventFilter(const OrbisKernelEvent* ev) {
    return ev->filter;
}

u64 PS4_SYSV_ABI sceKernelGetEventData(const OrbisKernelEvent* ev) {
    return ev->data;
}

void RegisterEventQueue(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("nh2IFMgKTv8", "libScePosix", 1, "libkernel", posix_kqueue);
    LIB_FUNCTION("RW-GEfpnsqg", "libScePosix", 1, "libkernel", posix_kevent);
    LIB_FUNCTION("D0OdFMjp46I", "libkernel", 1, "libkernel", sceKernelCreateEqueue);
    LIB_FUNCTION("jpFjmgAC5AE", "libkernel", 1, "libkernel", sceKernelDeleteEqueue);
    LIB_FUNCTION("fzyMKs9kim0", "libkernel", 1, "libkernel", sceKernelWaitEqueue);
    LIB_FUNCTION("vz+pg2zdopI", "libkernel", 1, "libkernel", sceKernelGetEventUserData);
    LIB_FUNCTION("4R6-OvI2cEA", "libkernel", 1, "libkernel", sceKernelAddUserEvent);
    LIB_FUNCTION("WDszmSbWuDk", "libkernel", 1, "libkernel", sceKernelAddUserEventEdge);
    LIB_FUNCTION("R74tt43xP6k", "libkernel", 1, "libkernel", sceKernelAddHRTimerEvent);
    LIB_FUNCTION("J+LF6LwObXU", "libkernel", 1, "libkernel", sceKernelDeleteHRTimerEvent);
    LIB_FUNCTION("57ZK+ODEXWY", "libkernel", 1, "libkernel", sceKernelAddTimerEvent);
    LIB_FUNCTION("YWQFUyXIVdU", "libkernel", 1, "libkernel", sceKernelDeleteTimerEvent);
    LIB_FUNCTION("F6e0kwo4cnk", "libkernel", 1, "libkernel", sceKernelTriggerUserEvent);
    LIB_FUNCTION("LJDwdSNTnDg", "libkernel", 1, "libkernel", sceKernelDeleteUserEvent);
    LIB_FUNCTION("mJ7aghmgvfc", "libkernel", 1, "libkernel", sceKernelGetEventId);
    LIB_FUNCTION("23CPPI1tyBY", "libkernel", 1, "libkernel", sceKernelGetEventFilter);
    LIB_FUNCTION("kwGyyjohI50", "libkernel", 1, "libkernel", sceKernelGetEventData);
}

} // namespace Libraries::Kernel
