// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "core/vr/quest_move.h"
#include "move.h"

namespace Libraries::Move {

static bool g_library_initialized = false;

// with SHADPS4_QUEST_MOVE=1 the wands the game opens are the Quest controllers.
// Move index 0 is the right hand, index 1 the left; only the first user that opens wands gets
// them (the second player's wands stay "not connected").
namespace {
struct OpenedWand {
    s32 handle;
    s32 user_id;
    s32 index;
};
std::mutex g_wand_mutex;
std::vector<OpenedWand> g_wands;
s32 g_player_user = -1;
bool g_logged_first_state = false;

/// The hand (0 = right, 1 = left) a handle is driven by, -1 for none.
int HandOf(s32 handle) {
    if (!Core::Vr::QuestMove::Enabled()) {
        return -1;
    }
    std::scoped_lock lock{g_wand_mutex};
    for (const OpenedWand& wand : g_wands) {
        if (wand.handle == handle) {
            return wand.user_id == g_player_user && wand.index >= 0 && wand.index <= 1
                       ? wand.index
                       : -1;
        }
    }
    return -1;
}

void FillState(const Core::Vr::QuestMove::Wand& wand, OrbisMoveData* data) {
    using namespace Core::Vr;
    std::memset(data, 0, sizeof(OrbisMoveData));
    data->button_data.button_data = wand.buttons;
    data->button_data.trigger_data = wand.trigger;
    // The wand's motion sensors, in its own frame: gravity (in g) and the turning rate (rad/s).
    // Derived from how the controller is placed; nothing here is measured by a sensor.
    const Quat to_wand = Conjugate(wand.pose.orientation);
    const Vec3 gravity = Rotate(to_wand, {0.0f, 1.0f, 0.0f});
    const Vec3 turning = Rotate(to_wand, wand.angular_velocity);
    data->accelerometer[0] = gravity.x;
    data->accelerometer[1] = gravity.y;
    data->accelerometer[2] = gravity.z;
    data->gyro[0] = turning.x;
    data->gyro[1] = turning.y;
    data->gyro[2] = turning.z;
    data->timestamp = static_cast<s64>(Libraries::Kernel::sceKernelGetProcessTime());
    data->count = 1;
    data->temperature = 25.0f;
}
} // namespace

int QuestHandOfHandle(s32 handle) {
    return HandOf(handle);
}

s32 PS4_SYSV_ABI sceMoveInit() {
    if (g_library_initialized) {
        return ORBIS_MOVE_ERROR_ALREADY_INIT;
    }
    LOG_WARNING(Lib_Move, "Move controllers are not supported yet");
    g_library_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                             s32 index) {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Even when no controllers are connected, this returns a proper handle.
    // Internal libSceVrTracker logic requires this handle to be different from other devices.
    static s32 handle = 0x30b0000;
    handle += 0x100;
    if (Core::Vr::QuestMove::Enabled()) {
        std::scoped_lock lock{g_wand_mutex};
        if (g_player_user == -1) {
            g_player_user = user_id;
        }
        g_wands.push_back({handle, user_id, index});
        LOG_INFO(Lib_Move, "wand {:#x} (user {}, index {}) is the Quest controller of the {} hand",
                 handle, user_id, index,
                 user_id != g_player_user ? "(nobody's)" : (index == 0 ? "right" : "left"));
    }
    return handle;
}

s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (info == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (HandOf(handle) >= 0) {
        std::memset(info, 0, sizeof(*info));
        info->sphere_radius = 0.022f;
        return ORBIS_OK;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data) {
    LOG_TRACE(Lib_Move, "(called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (const int hand = HandOf(handle); hand >= 0) {
        const auto wand = Core::Vr::QuestMove::Hub::Instance().Get(hand);
        if (wand.connected) {
            FillState(wand, data);
            if (!g_logged_first_state) {
                g_logged_first_state = true;
                LOG_INFO(Lib_Move, "the game reads the Quest controllers as Move wands now");
            }
            return ORBIS_OK;
        }
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                        s32* out_count) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (timestamp < 0 || data == nullptr || out_count == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (const int hand = HandOf(handle); hand >= 0) {
        const auto wand = Core::Vr::QuestMove::Hub::Instance().Get(hand);
        if (wand.connected) {
            // One sample: the latest.
            FillState(wand, data);
            *out_count = 1;
            return ORBIS_OK;
        }
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveGetExtensionPortInfo(s32 handle, void* data) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (const int hand = HandOf(handle); hand >= 0) {
        Core::Vr::QuestMove::Hub::Instance().SetRumble(hand, intensity);
        return ORBIS_OK;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (HandOf(handle) >= 0) {
        // (The Quest controllers have no light sphere to colour.)
        return ORBIS_OK;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveResetLightSphere(s32 handle) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Returns success, even if no controllers are connected.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveClose(s32 handle) {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (const int hand = HandOf(handle); hand >= 0) {
        Core::Vr::QuestMove::Hub::Instance().SetRumble(hand, 0);
    }
    {
        std::scoped_lock lock{g_wand_mutex};
        std::erase_if(g_wands, [handle](const OpenedWand& wand) { return wand.handle == handle; });
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveTerm() {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    g_library_initialized = false;
    {
        std::scoped_lock lock{g_wand_mutex};
        g_wands.clear();
        g_player_user = -1;
    }
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("j1ITE-EoJmE", "libSceMove", 1, "libSceMove", sceMoveInit);
    LIB_FUNCTION("HzC60MfjJxU", "libSceMove", 1, "libSceMove", sceMoveOpen);
    LIB_FUNCTION("GWXTyxs4QbE", "libSceMove", 1, "libSceMove", sceMoveGetDeviceInfo);
    LIB_FUNCTION("ttU+JOhShl4", "libSceMove", 1, "libSceMove", sceMoveReadStateLatest);
    LIB_FUNCTION("f2bcpK6kJfg", "libSceMove", 1, "libSceMove", sceMoveReadStateRecent);
    LIB_FUNCTION("y5h7f8H1Jnk", "libSceMove", 1, "libSceMove", sceMoveGetExtensionPortInfo);
    LIB_FUNCTION("IFQwtT2CeY0", "libSceMove", 1, "libSceMove", sceMoveSetVibration);
    LIB_FUNCTION("T8KYHPs1JE8", "libSceMove", 1, "libSceMove", sceMoveSetLightSphere);
    LIB_FUNCTION("zuxWAg3HAac", "libSceMove", 1, "libSceMove", sceMoveResetLightSphere);
    LIB_FUNCTION("XX6wlxpHyeo", "libSceMove", 1, "libSceMove", sceMoveClose);
    LIB_FUNCTION("tsZi60H4ypY", "libSceMove", 1, "libSceMove", sceMoveTerm);
};

} // namespace Libraries::Move