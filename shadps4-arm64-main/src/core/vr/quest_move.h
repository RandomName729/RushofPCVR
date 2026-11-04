// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Quest (Touch) controllers standing in for PlayStation Move wands.
//
// The OpenXR host (openxr_host.cpp) reads the headset's controllers and publishes what each hand
// does here; libSceMove (move.cpp) and libSceVrTracker (vr_tracker.cpp) hand it to the game as if
// it came from Move wands. The other direction (the game's rumble request) is published here by
// libSceMove and played by the OpenXR host.
//
// Header only on purpose: nothing has to be added to CMakeLists.txt.
//
// Switch: the environment variable SHADPS4_QUEST_MOVE=1 (the launcher sets it when "controllers=quest"
// is chosen in its start-up window). Without it nothing in here is used and the emulator behaves
// as before.

#pragma once

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common/logging/log.h"
#include "common/types.h"
#include "core/vr/vr_runtime.h"

namespace Core::Vr::QuestMove {

/// PlayStation Move button bits as the game tests them (OrbisMoveButtonData::button_data).
/// Found in the game's own button table (Rush of Blood, CUSA03683): mask -> logical button.
enum Button : u16 {
    SELECT = 0x0001,
    T = 0x0002, // the trigger; trigger_data carries the analog value
    MOVE = 0x0004, // the big button on the front of the wand
    START = 0x0008,
    TRIANGLE = 0x0010,
    CIRCLE = 0x0020,
    CROSS = 0x0040,
    SQUARE = 0x0080,
};

inline bool Enabled() {
    static const bool on = [] {
        const char* value = std::getenv("SHADPS4_QUEST_MOVE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

/// One wand. Hand 0 is the player's right hand, hand 1 the left (the game opens Move index 0
/// first, which is the hand that holds the gun).
struct Wand {
    bool connected{};     ///< The headset's controller exists (is on and awake).
    bool tracked{};       ///< Its place is known right now.
    u16 buttons{};        ///< Button bits.
    u8 trigger{};         ///< Analog trigger, 0..255.
    Pose pose;            ///< Tracker space, where the glowing sphere would be.
    Vec3 linear_velocity; ///< Tracker space, m/s.
    Vec3 angular_velocity;///< Tracker space, rad/s.
    u64 sequence{};       ///< Counts publications.
};

class Hub {
public:
    static Hub& Instance() {
        static Hub hub;
        return hub;
    }

    void Publish(int hand, const Wand& wand) {
        std::scoped_lock lock{mutex};
        const u64 sequence = wands[hand].sequence + 1;
        wands[hand] = wand;
        wands[hand].sequence = sequence;
    }

    Wand Get(int hand) const {
        std::scoped_lock lock{mutex};
        return wands[hand];
    }

    /// What the game asks of the wand's rumble motor, 0..255.
    void SetRumble(int hand, u8 intensity) {
        rumble[hand].store(intensity, std::memory_order_relaxed);
    }
    u8 GetRumble(int hand) const {
        return rumble[hand].load(std::memory_order_relaxed);
    }

private:
    mutable std::mutex mutex;
    std::array<Wand, 2> wands{};
    std::array<std::atomic<u8>, 2> rumble{};
};

} // namespace Core::Vr::QuestMove
