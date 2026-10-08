// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include "common/types.h"

namespace VideoCore {

/// Loads renderdoc dynamic library module.
void LoadRenderDoc();

/// Begins a capture if a renderdoc instance is attached.
void StartCapture();

/// Ends current renderdoc capture.
void EndCapture();

/// Triggers capturing process.
void TriggerCapture();

/// Tells RenderDoc which Vulkan instance (VkInstance handle) a capture is of, so that it does not
/// capture another graphics API in the process, such as the headset runtime's Direct3D 11.
void SetCaptureVkInstance(void* vk_instance);

/// Starts a RenderDoc capture of the given Vulkan instance now, and ends it after the given number
/// of calls to PresentCaptureTick (one per presented frame), so that the game's own draws, which
/// are submitted from other threads, fall inside it.
void StartPresentCapture(void* vk_instance, u32 frames);

/// Counts one presented frame of a StartPresentCapture capture, and ends it when due.
void PresentCaptureTick();

/// Sets output directory for captures
void SetOutputDir(const std::filesystem::path& path, const std::string& prefix);

/// Returns true when RenderDoc API was loaded and is usable.
bool IsRenderDocLoaded();

enum class ScreenshotRequest : u32 {
    None = 0,
    GameOnly = 1,
    WithOverlays = 2,
};

struct ScreenshotRequests {
    u32 game_only_count{};
    u32 with_overlays_count{};
};

/// Queues an in-emulator screenshot request to be consumed by the presenter.
void RequestScreenshot(ScreenshotRequest request);

/// Atomically consumes and returns pending "game only" screenshot request counter.
u32 ConsumeGameOnlyScreenshotRequests();

/// Atomically consumes and returns pending "with overlays" screenshot request counter.
u32 ConsumeWithOverlaysScreenshotRequests();

/// Atomically consumes and returns pending screenshot request counters.
ScreenshotRequests ConsumeScreenshotRequests();

} // namespace VideoCore
