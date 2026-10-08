// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/vr/vr_runtime.h"

namespace Libraries::HmdSetupDialog {

s32 PS4_SYSV_ABI sceHmdSetupDialogInitialize() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogClose() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

// DIAGNOSTIC (Rush of Blood, forced pause menu): the game opens this dialog every frame when
// the first dword of its VR manager object (at 0x24404b0 in the game's image) has bit 0 set and
// bit 1 clear, or when the headset status is not "ready". Log that dword, and the ones next to
// it, the first few times and whenever it changes.
static void TraceGameHeadsetFlags() {
    constexpr u64 FlagsAddress = 0x800000000ull + 0x24404b0ull;
    static std::atomic<u32> calls{0};
    static std::atomic<u32> last{0xdeadbeef};
    auto* memory = Core::Memory::Instance();
    if (!memory->IsValidMapping(FlagsAddress, 16)) {
        return;
    }
    const auto* words = reinterpret_cast<const volatile u32*>(FlagsAddress);
    const u32 flags = words[0];
    if (calls.fetch_add(1) < 6 || flags != last.exchange(flags)) {
        LOG_WARNING(Lib_HmdSetupDialog,
                    "the game's headset flags are {:#x} (bit 0 = {}, bit 1 = {}); the next dwords "
                    "are {:#x} {:#x} {:#x}",
                    flags, flags & 1, (flags >> 1) & 1, words[1], words[2], words[3]);
    }
    last.store(flags);
}

s32 PS4_SYSV_ABI sceHmdSetupDialogOpen(const OrbisHmdSetupDialogParam* param) {
    TraceGameHeadsetFlags();
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    // On real hardware, a dialog would show up telling the user to connect a PSVR headset.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogGetResult(OrbisHmdSetupDialogResult* result) {
    LOG_DEBUG(Lib_HmdSetupDialog, "called");
    // Result::OK means a headset was connected. Without one, simulate the user pressing circle
    // to cancel the dialog.
    result->result = Core::Vr::Runtime::Instance().IsHeadsetConnected()
                         ? Libraries::CommonDialog::Result::OK
                         : Libraries::CommonDialog::Result::USER_CANCELED;
    return ORBIS_OK;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogUpdateStatus() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return Libraries::CommonDialog::Status::FINISHED;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogGetStatus() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return Libraries::CommonDialog::Status::FINISHED;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogTerminate() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("nmHzU4Gh0xs", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogClose);
    LIB_FUNCTION("6lVRHMV5LY0", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetResult);
    LIB_FUNCTION("J9eBpW1udl4", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetStatus);
    LIB_FUNCTION("NB1Y2kA2jCY", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogInitialize);
    LIB_FUNCTION("NNgiV4T+akU", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogOpen);
    LIB_FUNCTION("+z4OJmFreZc", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogTerminate);
    LIB_FUNCTION("Ud7j3+RDIBg", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogUpdateStatus);
};

} // namespace Libraries::HmdSetupDialog