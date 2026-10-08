// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include <boost/preprocessor/stringize.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/pm4_packet.h"
#include "video_core/amdgpu/pm4_type0.h"
#include "video_core/amdgpu/pm4_type1.h"
#include "video_core/amdgpu/stall_log.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace AmdGpu {

// Label writes of the guest used to be logged here while the game's freeze was being tracked
// down. Kept as a no-op so the call sites stay in place; nothing is logged any more.
static void TraceLabelWrite(const char*, const void*, u64) {}

static void MaybeRecordVideoOutLabelLock(Libraries::VideoOut::VideoOutPort* vo_port, u64* address,
                                         const void* data, u32 data_size) {
    if (vo_port == nullptr || address == nullptr || !vo_port->IsVoLabel(address)) {
        return;
    }
    u64 value = 0;
    std::memcpy(&value, data, std::min<size_t>(data_size, sizeof(value)));
    if (value != 1) {
        return;
    }
    std::scoped_lock lock{vo_port->port_mutex};
    vo_port->flip_labels.RecordGpuLock(vo_port->LabelIndex(address));
}

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

Liverpool::Liverpool() {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
}

void Liverpool::ProcessCommands() {
    // Process incoming commands with high priority
    while (num_commands) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        callback();
    }
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
    gpu_id = std::this_thread::get_id();

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(submit_cv, lk, stoken,
                                [this] { return num_commands || num_submits || submit_done; });
        }
        if (stoken.stop_requested()) {
            break;
        }

        VideoCore::StartCapture();

        curr_qid = -1;

        while (num_submits || num_commands) {
            ProcessCommands();

            curr_qid = (curr_qid + 1) % num_mapped_queues;

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            in_gfx_task.store(true, std::memory_order_relaxed);
            task.resume();
            in_gfx_task.store(false, std::memory_order_relaxed);
#ifdef ENABLE_BACHATA_RUNTIME
            if (++debug_resume_count % 1'000 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "GPU coroutine active resumes={} queue={} opcode={:#x} submits={}",
                            debug_resume_count, curr_qid, debug_last_opcode, num_submits.load());
            }
#endif

            if (task.done()) {
                task.destroy();

                std::scoped_lock lock{queue.m_access};
                queue.submits.pop();

                --num_submits;
                std::scoped_lock lock2{submit_mutex};
                submit_cv.notify_all();
            }
        }

        if (submit_done) {
            VideoCore::EndCapture();
            if (rasterizer) {
                rasterizer->OnSubmit();
                rasterizer->Flush();
            }
            submit_done = false;
        }

        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
    }
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (!Pm4PacketFits(ccb)) {
            LOG_ERROR(Lib_GnmDriver,
                      "CE PM4 packet exceeds remaining submission. type={} size={} remaining={}",
                      type, Pm4PacketDwords(header->raw), ccb.size());
            break;
        }
        if (type == 2) {
            ccb = NextPacket(ccb, 1);
            continue;
        }
        if (type == 0) {
            const auto type0 = ConsumePm4Type0(ccb, regs.reg_array);
            if (!type0.consumed) {
                LOG_ERROR(Lib_GnmDriver,
                          "CE PM4 type 0 exceeds remaining submission. size={} remaining={}",
                          header->type0.NumWords(), ccb.size());
                break;
            }
            LOG_WARNING(Lib_GnmDriver, "CE PM4 type 0 register write base={} size={} written={}",
                        header->type0.base.Value(), header->type0.NumWords(), type0.words_written);
            ccb = NextPacket(ccb, type0.packet_dwords);
            continue;
        }
        if (type == 1) {
            const auto type1 = ConsumePm4Type1(ccb, regs.reg_array);
            if (!type1.consumed) {
                LOG_ERROR(Lib_GnmDriver, "CE PM4 type 1 exceeds remaining submission. remaining={}",
                          ccb.size());
                break;
            }
            LOG_WARNING(Lib_GnmDriver, "CE PM4 type 1 register write a={} b={} written={}",
                        Pm4Type1RegA(header->raw), Pm4Type1RegB(header->raw), type1.words_written);
            ccb = NextPacket(ccb, type1.packet_dwords);
            continue;
        }
        if (type != 3) {
            LOG_ERROR(Lib_GnmDriver, "CE invalid PM4 type {} — skipping remainder", type);
            break;
        }

        const PM4ItOpcode opcode = header->type3.opcode;
#ifdef ENABLE_BACHATA_RUNTIME
        debug_last_opcode = static_cast<u32>(opcode);
#endif
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const u32 diff = it_body[0];
            u64 wdecd_iters = 0;
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
#if 1 // PC diagnostics: report stalled GPU waits (was ENABLE_BACHATA_RUNTIME only)
                if (ShouldLogStallIteration(++wdecd_iters)) {
                    LOG_WARNING(Render_Vulkan,
                                "PM4 WAIT_ON_DE_COUNTER_DIFF stalled (CE) iterations={} diff={} "
                                "ce_count={} de_count={}",
                                wdecd_iters, diff, cblock.ce_count, cblock.de_count);
                }
#endif
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            if (!Pm4IndirectBufferUsable(indirect_buffer->Address<const u32>(),
                                         indirect_buffer->ib_size)) {
                LOG_ERROR(Lib_GnmDriver, "CE IT_INDIRECT_BUFFER_CONST skipped: addr={} size={}",
                          fmt::ptr(indirect_buffer->Address<const u32>()),
                          indirect_buffer->ib_size.Value());
                break;
            }
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                           bool is_submission) {
    FIBER_ENTER(dcb_task_name);
    if (is_submission) {
        gfx_pred = {}; // predication does not carry over into a new submission
    }

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }
    const bool host_markers_enabled = rasterizer && EmulatorSettings.IsVkHostMarkersEnabled();
    const bool guest_markers_enabled = rasterizer && EmulatorSettings.IsVkGuestMarkersEnabled();

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    while (!dcb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;
        if (!Pm4PacketFits(dcb)) {
            LOG_ERROR(Lib_GnmDriver,
                      "PM4 packet exceeds remaining submission. type={} size={} remaining={}", type,
                      Pm4PacketDwords(header->raw), dcb.size());
            break;
        }

        switch (type) {
        default:
            LOG_ERROR(Lib_GnmDriver, "Wrong PM4 type {} — skipping remainder", type);
            dcb = {};
            break;
        case 0: {
            const auto type0 = ConsumePm4Type0(dcb, regs.reg_array);
            if (!type0.consumed) {
                LOG_ERROR(Lib_GnmDriver,
                          "PM4 type 0 exceeds remaining submission. base={} size={} remaining={}",
                          header->type0.base.Value(), header->type0.NumWords(), dcb.size());
                dcb = {};
                break;
            }
            LOG_WARNING(Lib_GnmDriver, "PM4 type 0 register write base={} size={} written={}",
                        header->type0.base.Value(), header->type0.NumWords(), type0.words_written);
            dcb = NextPacket(dcb, type0.packet_dwords);
            continue;
        }
        case 1: {
            const auto type1 = ConsumePm4Type1(dcb, regs.reg_array);
            if (!type1.consumed) {
                LOG_ERROR(Lib_GnmDriver, "PM4 type 1 exceeds remaining submission. remaining={}",
                          dcb.size());
                dcb = {};
                break;
            }
            LOG_WARNING(Lib_GnmDriver, "PM4 type 1 register write a={} b={} written={}",
                        Pm4Type1RegA(header->raw), Pm4Type1RegB(header->raw), type1.words_written);
            dcb = NextPacket(dcb, type1.packet_dwords);
            continue;
        }
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
#ifdef ENABLE_BACHATA_RUNTIME
            debug_last_opcode = static_cast<u32>(opcode);
#endif
            // A packet with its predicate bit set is skipped while SET_PREDICATION says so.
            if (gfx_pred.active && !gfx_pred.execute &&
                header->type3.predicate.Value() == PM4Predicate::PredEnable &&
                opcode != PM4ItOpcode::SetPredication) {
                if (gfx_pred.skipped++ < 40) {
                    LOG_WARNING(Render_Vulkan,
                                "predication: skipped a packet with opcode {:#x} ({} skipped so far)",
                                static_cast<u32>(opcode), gfx_pred.skipped);
                }
                dcb = NextPacket(dcb, header->type3.NumWords() + 1);
                break;
            }
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip,
                                                       nop->data_block[1]);
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState: {
                regs.SetDefaults();
                break;
            }
            case PM4ItOpcode::SetConfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ConfigRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                break;
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } else {
                    std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                                header + 2, set_size);
                }
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset],
                            header + 2, (count - 1) * sizeof(u32));
                break;
            }
            case PM4ItOpcode::SetPredication: {
                // Same layout as the GCN SET_PREDICATION packet. Dword 1: address bits 31:0.
                // Dword 2: address bits 39:32 in [7:0]; bit 8 = "draw when visible"; bit 12 =
                // wait hint; [18:16] = operation (0 clear, 1 z-pass counts, 2 primitive count,
                // 3 64-bit boolean, 4 32-bit boolean); bit 31 = continue (combine with the previous
                // result). The result is worked out once, when the packet is processed.
                const u32* body = reinterpret_cast<const u32*>(header);
                const u32 dw1 = body[1];
                const u32 dw2 = body[2];
                const u64 pred_addr = (u64{dw2 & 0xffu} << 32) | dw1;
                const u32 pred_op = (dw2 >> 16) & 0x7;
                const bool draw_visible = ((dw2 >> 8) & 1) != 0;
                const bool pred_continue = (dw2 >> 31) != 0;
                bool visible = true;
                u64 mem0 = 0;
                u64 mem1 = 0;
                if (pred_op != 0 && pred_addr != 0) {
                    const auto* mem = reinterpret_cast<const u64*>(pred_addr);
                    mem0 = mem[0];
                    mem1 = mem[1];
                    constexpr u64 ValidBit = 1ull << 63;
                    if (pred_op == 1) {
                        // z-pass query: pairs of (begin, end) sample counts, visible when they differ
                        // in any pair. A result that is not ready counts as visible (no-wait hint).
                        bool any_visible = false;
                        bool any_pending = false;
                        for (u32 i = 0; i < std::max<u32>(num_counter_pairs, 1); ++i) {
                            const u64 begin = mem[i * 2];
                            const u64 end = mem[i * 2 + 1];
                            if (!(begin & ValidBit) || !(end & ValidBit)) {
                                any_pending = true;
                            } else if ((end & ~ValidBit) != (begin & ~ValidBit)) {
                                any_visible = true;
                            }
                        }
                        visible = any_visible || any_pending;
                    } else if (pred_op == 3) {
                        visible = mem0 != 0;
                    } else if (pred_op == 4) {
                        visible = static_cast<u32>(mem0) != 0;
                    } // primitive count (2) and unknown operations: treat as visible
                }
                if (pred_op == 0) {
                    gfx_pred.active = false;
                    gfx_pred.visible = true;
                    gfx_pred.execute = true;
                } else {
                    if (pred_continue && gfx_pred.active) {
                        visible = gfx_pred.visible || visible;
                    }
                    gfx_pred.active = true;
                    gfx_pred.visible = visible;
                    gfx_pred.execute = draw_visible ? visible : !visible;
                }
                static std::atomic<u32> traced_predications{0};
                if (traced_predications.fetch_add(1) < 120) {
                    LOG_WARNING(Render_Vulkan,
                                "SET_PREDICATION: dwords={:#x} {:#x} address={:#x} op={} "
                                "draw_visible={} continue={} memory={:#x} {:#x} -> visible={} "
                                "packets are {}",
                                dw1, dw2, pred_addr, pred_op, draw_visible, pred_continue, mem0,
                                mem1, visible,
                                (pred_op == 0 || gfx_pred.execute) ? "executed" : "skipped");
                }
                break;
            }
            case PM4ItOpcode::IndexType: {
                const auto* index_type = reinterpret_cast<const PM4CmdDrawIndexType*>(header);
                regs.index_buffer_type.raw = index_type->raw;
                break;
            }
            case PM4ItOpcode::DrawIndex2: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
                regs.max_index_size = draw_index->max_size;
                regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
                regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(fmt::format("gfx:{}:DrawIndex2", cmd_address));
                        rasterizer->Draw(true);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->Draw(true);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                regs.max_index_size = draw_index_off->max_size;
                regs.num_indices = draw_index_off->index_count;
                regs.draw_initiator = draw_index_off->draw_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexOffset2", cmd_address));
                        rasterizer->Draw(true, draw_index_off->index_offset);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->Draw(true, draw_index_off->index_offset);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexAuto", cmd_address));
                        rasterizer->Draw(false);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->Draw(false);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndirect", cmd_address));
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndirectMulti", cmd_address));
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexIndirect", cmd_address));
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexIndirectMulti", cmd_address));
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexIndirectCountMulti", cmd_address));
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count,
                                                 draw_index_indirect->count_indirect_enable.Value()
                                                     ? draw_index_indirect->count_addr
                                                     : 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count,
                                                 draw_index_indirect->count_indirect_enable.Value()
                                                     ? draw_index_indirect->count_addr
                                                     : 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DispatchDirect", cmd_address));
                        rasterizer->DispatchDirect();
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DispatchDirect();
                    }
                }
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DispatchIndirect", cmd_address));
                        rasterizer->DispatchIndirect(indirect_args_addr, offset, size);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DispatchIndirect(indirect_args_addr, offset, size);
                    }
                }
                break;
            }
            case PM4ItOpcode::NumInstances: {
                const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
                regs.num_instances.num_instances = num_instances->num_instances;
                break;
            }
            case PM4ItOpcode::IndexBase: {
                const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
                regs.index_base_address.base_addr_lo = index_base->addr_lo;
                regs.index_base_address.base_addr_hi = index_base->addr_hi;
                break;
            }
            case PM4ItOpcode::IndexBufferSize: {
                const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
                regs.num_indices = index_size->num_indices;
                break;
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // TODO: handle proper synchronization, for now signal that update is done
                    // immediately
                    regs.cp_strmout_cntl.offset_update_done = 1;
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        static constexpr u64 OcclusionCounterStep = 0x2FFFFFFULL;
                        u64* results = event->Address<u64*>();
                        for (s32 i = 0; i < num_counter_pairs; ++i, results += 2) {
                            *results = pixel_counter | OcclusionCounterValidMask;
                        }
                        pixel_counter += OcclusionCounterStep;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                event_eos->SignalFence([](void* address, u64 data, u32 num_bytes) {
                    TraceLabelWrite("gfx EVENT_WRITE_EOS", address, data);
                    auto* memory = Core::Memory::Instance();
                    if (!memory->TryWriteBacking(address, &data, num_bytes)) {
                        memcpy(address, &data, num_bytes);
                    }
                });
                if (event_eos->command == PM4CmdEventWriteEos::Command::GdsStore) {
                    ASSERT(event_eos->size == 1);
                    if (rasterizer) {
                        rasterizer->Finish();
                        const u32 value = rasterizer->ReadDataFromGds(event_eos->gds_index);
                        *event_eos->Address() = value;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                event_eop->SignalFence(
                    [](void* address, u64 data, u32 num_bytes) {
                        TraceLabelWrite("gfx EVENT_WRITE_EOP", address, data);
                        auto* memory = Core::Memory::Instance();
                        if (!memory->TryWriteBacking(address, &data, num_bytes)) {
                            memcpy(address, &data, num_bytes);
                        }
                    },
                    [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                    rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                                           dma_data->data, true);
                } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                            dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                           dma_data->dst_sel == DmaDataDst::Gds) {
                    rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                           dma_data->NumBytes(), true, false);
                } else if (dma_data->src_sel == DmaDataSrc::Data &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                           dma_data->data, false);
                } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                           dma_data->NumBytes(), false, true);
                } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                            dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(),
                                           dma_data->SrcAddress<VAddr>(), dma_data->NumBytes(),
                                           false, false);
                } else {
                    UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}",
                                    u32(dma_data->src_sel.Value()), u32(dma_data->dst_sel.Value()));
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                const u32 data_size = (header->type3.count.Value() - 2) * 4;
                u64* address = write_data->Address<u64*>();
                {
                    u64 traced = 0;
                    std::memcpy(&traced, write_data->data,
                                std::min<size_t>(data_size, sizeof(traced)));
                    TraceLabelWrite("gfx WRITE_DATA", address, traced);
                }
                if (!write_data->wr_one_addr.Value()) {
                    MaybeRecordVideoOutLabelLock(vo_port, address, write_data->data, data_size);
                    std::memcpy(address, write_data->data, data_size);
                } else {
                    UNREACHABLE();
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    mem_semaphore->Signal();
                } else {
                    u64 sem_iters = 0;
                    while (!mem_semaphore->Signaled()) {
#if 1 // PC diagnostics: report stalled GPU waits (was ENABLE_BACHATA_RUNTIME only)
                        if (ShouldLogStallIteration(++sem_iters)) {
                            LOG_WARNING(Render_Vulkan,
                                        "PM4 MEM_SEMAPHORE stalled queue=gfx iterations={}",
                                        sem_iters);
                        }
#endif
                        if (sem_iters > 1000) {
                            std::this_thread::yield();
                        }
                        YIELD_GFX();
                    }
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
                u64 rewind_iters = 0;
                while (!rewind->Valid()) {
#if 1 // PC diagnostics: report stalled GPU waits (was ENABLE_BACHATA_RUNTIME only)
                    if (ShouldLogStallIteration(++rewind_iters)) {
                        LOG_WARNING(Render_Vulkan, "PM4 REWIND stalled queue=gfx iterations={}",
                                    rewind_iters);
                    }
#endif
                    if (rewind_iters > 1000) {
                        std::this_thread::yield();
                    }
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                if (wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory &&
                    !Pm4GuestPointerUsable(wait_reg_mem->Address<const void*>())) {
                    LOG_ERROR(Lib_GnmDriver, "IT_WAIT_REG_MEM null address — skipping");
                    break;
                }
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    if (!wait_reg_mem->Test(regs.reg_array)) {
                        LOG_WARNING(Render_Vulkan,
                                    "VO label wait addr={:#x} value={:#x} ref={:#x} index={}",
                                    reinterpret_cast<uintptr_t>(wait_addr), *wait_addr,
                                    wait_reg_mem->ref, vo_port->LabelIndex(wait_addr));
                    }
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); });
                    break;
                }
                u64 wait_iterations = 0;
                const auto wait_start = std::chrono::steady_clock::now();
                // Rush of Blood: the game's label block (first 0x100 bytes of its GPU memory pool)
                // holds slots that the graphics queue waits on and that never get cleared here. For
                // those waits only, give up after a few milliseconds instead of two seconds.
                const bool wait_is_memory =
                    wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory;
                const uintptr_t wait_address =
                    wait_is_memory ? reinterpret_cast<uintptr_t>(wait_reg_mem->Address<const u32*>())
                                   : uintptr_t{0};
                const bool in_label_block = wait_address >= 0x250800000ull && wait_address < 0x250800100ull;
                const s64 breakout_ms = in_label_block ? 3 : 2000;
                while (!wait_reg_mem->Test(regs.reg_array)) {
#if 1 // PC diagnostics: report stalled GPU waits (was ENABLE_BACHATA_RUNTIME only)
                    if (ShouldLogStallIteration(++wait_iterations)) {
                        const bool is_memory =
                            wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory;
                        const u32 value = is_memory ? *wait_reg_mem->Address<const u32*>()
                                                    : regs.reg_array[wait_reg_mem->Reg()];
                        LOG_WARNING(Render_Vulkan,
                                    "PM4 WAIT_REG_MEM stalled queue=gfx iterations={} space={} "
                                    "address={:#x} value={:#x} ref={:#x} mask={:#x} function={}",
                                    wait_iterations, is_memory ? "memory" : "register",
                                    is_memory ? reinterpret_cast<uintptr_t>(
                                                    wait_reg_mem->Address<const u32*>())
                                              : wait_reg_mem->Reg(),
                                    value, wait_reg_mem->ref, wait_reg_mem->mask,
                                    static_cast<u32>(wait_reg_mem->function.Value()));
                        if (is_memory && wait_iterations == 100000) {
                            const auto* b = reinterpret_cast<const u32*>(
                                reinterpret_cast<uintptr_t>(wait_reg_mem->Address<const u32*>()) &
                                ~uintptr_t{0x3f});
                            LOG_WARNING(Render_Vulkan,
                                        "label neighbourhood {:#x}: {:08x} {:08x} {:08x} {:08x} "
                                        "{:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} "
                                        "{:08x} {:08x} {:08x} {:08x}",
                                        reinterpret_cast<uintptr_t>(b), b[0], b[1], b[2], b[3],
                                        b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
                                        b[13], b[14], b[15]);
                        }
                    }
#endif
                    // Prevent CPU starvation of guest threads
                    if (wait_iterations > 1000) {
                        std::this_thread::yield();
                    }
                    if (wait_iterations > 50000) {
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }

                    // Safety timeout: if stalled for >= 2000ms, force continue to prevent hard
                    // freeze
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - wait_start)
                                                .count();
                    if (elapsed_ms >= breakout_ms) {
                        if (in_label_block) {
                            // Release the slot the way the wait wanted it (function 3 = "equal").
                            static std::atomic<u32> label_breakouts{0};
                            const u32 n = label_breakouts.fetch_add(1) + 1;
                            const u32 seen = *wait_reg_mem->Address<const u32*>();
                            if (static_cast<u32>(wait_reg_mem->function.Value()) == 3 &&
                                wait_reg_mem->mask == 0xffffffffu) {
                                *wait_reg_mem->Address<u32*>() = wait_reg_mem->ref;
                            }
                            if (n <= 40 || (n % 500) == 0) {
                                LOG_WARNING(Render_Vulkan,
                                            "label breakout #{}: address={:#x} was {:#x}, wanted "
                                            "ref={:#x} (function {}) after {}ms",
                                            n, wait_address, seen, wait_reg_mem->ref,
                                            static_cast<u32>(wait_reg_mem->function.Value()),
                                            elapsed_ms);
                            }
                        } else {
                            LOG_ERROR(Render_Vulkan,
                                      "PM4 WAIT_REG_MEM timeout reached ({}ms, iterations={}) on "
                                      "queue=gfx — forcing breakout",
                                      elapsed_ms, wait_iterations);
                        }
                        break;
                    }

                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                if (!Pm4IndirectBufferUsable(indirect_buffer->Address<const u32>(),
                                             indirect_buffer->ib_size)) {
                    LOG_ERROR(Lib_GnmDriver, "IT_INDIRECT_BUFFER skipped: addr={} size={}",
                              fmt::ptr(indirect_buffer->Address<const u32>()),
                              indirect_buffer->ib_size.Value());
                    break;
                }
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                RESUME_GFX(task);

                u64 ib_iters = 0;
                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
#if 1 // PC diagnostics: report stalled GPU waits (was ENABLE_BACHATA_RUNTIME only)
                    if (ShouldLogStallIteration(++ib_iters)) {
                        LOG_WARNING(Render_Vulkan,
                                    "PM4 INDIRECT_BUFFER nested gfx stalled iterations={}",
                                    ib_iters);
                    }
#endif
                }
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                u64 woce_iters = 0;
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
#if 1 // PC diagnostics: report stalled GPU waits (was ENABLE_BACHATA_RUNTIME only)
                    if (ShouldLogStallIteration(++woce_iters)) {
                        LOG_WARNING(Render_Vulkan,
                                    "PM4 WAIT_ON_CE_COUNTER stalled queue=gfx iterations={} "
                                    "ce_count={} de_count={} ce_task_done={}",
                                    woce_iters, cblock.ce_count, cblock.de_count, 0);
                    }
#endif
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                if (rasterizer) {
                    rasterizer->CpSync();
                }
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                if (!Pm4GuestPointerUsable(cond_exec->Address())) {
                    LOG_ERROR(Lib_GnmDriver, "IT_COND_EXEC null predicate — skipping remainder");
                    dcb = {};
                    break;
                }
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    if (ce_task.handle) {
        u64 cedrain_iters = 0;
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
#if 1 // PC diagnostics: report stalled GPU waits (was ENABLE_BACHATA_RUNTIME only)
            if (ShouldLogStallIteration(++cedrain_iters)) {
                LOG_WARNING(Render_Vulkan,
                            "PM4 CE_DRAIN stalled (post-dcb ce_task) iterations={} "
                            "ce_count={} de_count={}",
                            cedrain_iters, cblock.ce_count, cblock.de_count);
            }
#endif
        }
        ce_task.handle.destroy();
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];
    const bool host_markers_enabled = rasterizer && EmulatorSettings.IsVkHostMarkersEnabled();

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        ProcessCommands();

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        // Type-1 REG_B lives in the type-3 count field. Size from type first.
        u32 next_dw_off = header->type == 1 ? kPm4Type1PacketDwords : header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            const u32 packet_dwords =
                header->type == 1 ? kPm4Type1PacketDwords : header->type3.NumWords() + 1;
            next_dw_off = packet_dwords - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type == 0) {
            const auto type0 = ConsumePm4Type0(acb, regs.reg_array);
            if (!type0.consumed) {
                LOG_ERROR(Lib_GnmDriver,
                          "ASC PM4 type 0 exceeds remaining submission. size={} remaining={}",
                          header->type0.NumWords(), acb.size());
                break;
            }
            LOG_WARNING(Lib_GnmDriver, "ASC PM4 type 0 register write base={} size={} written={}",
                        header->type0.base.Value(), header->type0.NumWords(), type0.words_written);
            next_dw_off = type0.packet_dwords;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type == 1) {
            const auto type1 = ConsumePm4Type1(acb, regs.reg_array);
            if (!type1.consumed) {
                LOG_ERROR(Lib_GnmDriver,
                          "ASC PM4 type 1 exceeds remaining submission. remaining={}", acb.size());
                break;
            }
            LOG_WARNING(Lib_GnmDriver, "ASC PM4 type 1 register write a={} b={} written={}",
                        Pm4Type1RegA(header->raw), Pm4Type1RegB(header->raw), type1.words_written);
            next_dw_off = type1.packet_dwords;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            LOG_ERROR(Lib_GnmDriver, "ASC invalid PM4 type {} — skipping remainder",
                      header->type.Value());
            break;
        }

        const PM4ItOpcode opcode = header->type3.opcode;
#ifdef ENABLE_BACHATA_RUNTIME
        debug_last_opcode = static_cast<u32>(opcode);
#endif

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            if (!Pm4IndirectBufferUsable(indirect_buffer->Address<const u32>(),
                                         indirect_buffer->ib_size)) {
                LOG_ERROR(Lib_GnmDriver, "ASC IT_INDIRECT_BUFFER skipped: addr={} size={}",
                          fmt::ptr(indirect_buffer->Address<const u32>()),
                          indirect_buffer->ib_size.Value());
                break;
            }
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(), dma_data->data,
                                       true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                       dma_data->NumBytes(), true, false);
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                       dma_data->data, false);
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                       dma_data->NumBytes(), false, true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}",
                                u32(dma_data->src_sel.Value()), u32(dma_data->dst_sel.Value()));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            u64 rewind_iters = 0;
            while (!rewind->Valid()) {
                if (++rewind_iters > 1000) {
                    std::this_thread::yield();
                }
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                rasterizer->DispatchIndirect(it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                const auto cmd_address = reinterpret_cast<const void*>(header);
                if (host_markers_enabled) {
                    rasterizer->ScopeMarkerBegin(
                        fmt::format("asc[{}]:{}:DispatchDirect", vqid, cmd_address));
                    rasterizer->DispatchDirect();
                    rasterizer->ScopeMarkerEnd();
                } else {
                    rasterizer->DispatchDirect();
                }
            }
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                const auto cmd_address = reinterpret_cast<const void*>(header);
                if (host_markers_enabled) {
                    rasterizer->ScopeMarkerBegin(
                        fmt::format("asc[{}]:{}:DispatchIndirect", vqid, cmd_address));
                    rasterizer->DispatchIndirect(ib_address, 0, size);
                    rasterizer->ScopeMarkerEnd();
                } else {
                    rasterizer->DispatchIndirect(ib_address, 0, size);
                }
            }
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            {
                u64 traced = 0;
                std::memcpy(&traced, write_data->data, std::min<size_t>(data_size, sizeof(traced)));
                TraceLabelWrite("asc WRITE_DATA", write_data->Address<void*>(), traced);
            }
            if (!write_data->wr_one_addr.Value()) {
                MaybeRecordVideoOutLabelLock(vo_port, write_data->Address<u64*>(), write_data->data,
                                             data_size);
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                mem_semaphore->Signal();
            } else {
                u64 sem_iters = 0;
                while (!mem_semaphore->Signaled()) {
                    if (++sem_iters > 1000) {
                        std::this_thread::yield();
                    }
                    YIELD_ASC(vqid);
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            if (wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory &&
                !Pm4GuestPointerUsable(wait_reg_mem->Address<const void*>())) {
                LOG_ERROR(Lib_GnmDriver, "ASC IT_WAIT_REG_MEM null address — skipping");
                break;
            }
            u64 wait_iterations = 0;
            const auto wait_start = std::chrono::steady_clock::now();
            while (!wait_reg_mem->Test(regs.reg_array)) {
#if 1 // PC diagnostics: report stalled GPU waits (was ENABLE_BACHATA_RUNTIME only)
                if (ShouldLogStallIteration(++wait_iterations)) {
                    const bool is_memory =
                        wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory;
                    const u32 value = is_memory ? *wait_reg_mem->Address<const u32*>()
                                                : regs.reg_array[wait_reg_mem->Reg()];
                    LOG_WARNING(
                        Render_Vulkan,
                        "PM4 WAIT_REG_MEM stalled queue=asc{} iterations={} space={} "
                        "address={:#x} value={:#x} ref={:#x} mask={:#x} function={}",
                        vqid, wait_iterations, is_memory ? "memory" : "register",
                        is_memory ? reinterpret_cast<uintptr_t>(wait_reg_mem->Address<const u32*>())
                                  : wait_reg_mem->Reg(),
                        value, wait_reg_mem->ref, wait_reg_mem->mask,
                        static_cast<u32>(wait_reg_mem->function.Value()));
                }
#endif
                if (wait_iterations > 1000) {
                    std::this_thread::yield();
                }
                if (wait_iterations > 50000) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }

                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - wait_start)
                                            .count();
                if (elapsed_ms >= 2000) {
                    LOG_ERROR(Render_Vulkan,
                              "PM4 WAIT_REG_MEM timeout reached ({}ms, iterations={}) on "
                              "queue=asc{} — forcing breakout",
                              elapsed_ms, wait_iterations, vqid);
                    break;
                }

                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            TraceLabelWrite("asc RELEASE_MEM", release_mem->Address<const void*>(),
                            release_mem->DataQWord());
            release_mem->SignalFence(
                [pipe_id = queue.pipe_id] {
                    Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(pipe_id));
                },
                [this](VAddr dst, u16 gds_index, u16 num_dwords) {
                    rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false, true);
                });
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];

    // A single submission must always fit the reserved copy space.
    ASSERT_MSG(dcb.size() <= queue.dcb_buffer.capacity(),
               "dcb submission larger than reserved copy space");
    ASSERT_MSG(ccb.size() <= queue.ccb_buffer.capacity(),
               "ccb submission larger than reserved copy space");

    // The copy buffers persist across submissions so the GPU worker can parse a
    // command buffer even after the guest reuses its source ring. Their offsets
    // therefore grow monotonically and are only reset once the buffered data is no
    // longer needed. The guest is expected to signal that point with
    // sceGnmSubmitDone (-> SubmitDone), but some titles (e.g. Dark Souls
    // Remastered) never call it, so the offset would otherwise overflow the
    // reserved space and trip the assert below. When the next append would exceed
    // the reservation, drain the GPU first (all previously buffered spans have
    // then been consumed and can be discarded) and rewind the offsets to zero.
    const bool dcb_overflows = queue.dcb_buffer_offset + dcb.size() > queue.dcb_buffer.capacity();
    const bool ccb_overflows = queue.ccb_buffer_offset + ccb.size() > queue.ccb_buffer.capacity();
    if (dcb_overflows || ccb_overflows) {
        WaitGpuIdle();
        queue.dcb_buffer_offset = 0;
        queue.ccb_buffer_offset = 0;
    }

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    Vulkan::FrameStats::GuestSubmit();
    auto& queue = mapped_queues[GfxQueueId];

    if (EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    }

    auto task = ProcessGraphics(dcb, ccb, true);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    {
        static std::atomic<u32> traced_submits{0};
        if (traced_submits.fetch_add(1) < 60) {
            LOG_WARNING(Render_Vulkan, "ASC submit: queue={} dwords={}", vqid, acb.size());
        }
    }
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    ++num_submits;
    submit_cv.notify_one();
}

} // namespace AmdGpu