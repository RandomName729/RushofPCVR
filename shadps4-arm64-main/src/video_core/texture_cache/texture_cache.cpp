// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/guest_cpu/guest_wait_trace.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/staging_diag.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;
static constexpr std::size_t MaxViewDimensionMismatchRecords = 64;

// SHADPS4_DBG_TEX_AUDIT=1 looks for the causes of wrong or garbled textures (lines "TEXAUDIT:"):
//   STALE:  an image the cache believes is up to date, whose memory has changed since it was
//           uploaded and was never reported as changed (a write the tracking missed);
//   ZERO:   an image uploaded from memory that is all zero where it starts (data not there yet);
//   ALIAS:  a request answered with another, overlapping image (the usual way a clean picture
//           of one texture turns up where another belongs).
namespace TexAudit {
static bool Enabled() {
    static const bool enabled = std::getenv("SHADPS4_DBG_TEX_AUDIT") != nullptr;
    return enabled;
}

struct Record {
    u64 hash;
    std::chrono::steady_clock::time_point uploaded;
    std::chrono::steady_clock::time_point checked;
};
static std::map<std::pair<u64, u64>, Record> records;
static std::set<std::tuple<u64, u64, u64, u64>> aliases;
static std::set<std::pair<u64, u64>> zeros;
static u32 stale_logged = 0;
static u32 alias_logged = 0;
static u32 zero_logged = 0;
static u32 heal_logged = 0;
static u32 missed_logged = 0;
static u32 still_logged = 0;
static u32 sampled_logged = 0;
static u32 partial_logged = 0;

// An image uploaded from memory that was empty: what happens to it afterwards.
struct ZeroState {
    std::chrono::steady_clock::time_point first;
    std::chrono::steady_clock::time_point checked;
    u64 uses = 0;
    bool reported = false;
};
static std::map<std::pair<u64, u64>, ZeroState> zero_state;
static std::map<std::tuple<u64, u64, u32, u32>, std::chrono::steady_clock::time_point> sampled_seen;
static std::set<std::tuple<u64, u64, u32, u32>> sampled_logged_keys;
static std::set<u64> mip0_empty; // guest addresses of sampled images whose top mip is empty
static u64 stale_total = 0;
static u64 checks_total = 0;

static std::string Describe(const ImageInfo& info) {
    return fmt::format("{:#x}+{:#x} {}x{} m{} l{} {} tile{}", info.guest_address, info.guest_size,
                       info.size.width, info.size.height, info.resources.levels,
                       info.resources.layers, vk::to_string(info.pixel_format),
                       static_cast<u32>(info.tile_mode));
}

// SHADPS4_DBG_TEX_DUMP=1 (with SHADPS4_DBG_TEX_AUDIT=1): writes the raw guest bytes of the first
// noisy (and a few clean) compressed textures to files so they can be looked at offline. The
// directory is SHADPS4_TEX_DUMP_DIR (default C:/aq/texdump).
static void DumpTexture(bool noisy, const ImageInfo& info) {
    static const bool enabled = std::getenv("SHADPS4_DBG_TEX_DUMP") != nullptr;
    if (!enabled || info.guest_size < 0x20000 || info.guest_size > (u64{24} << 20)) {
        return;
    }
    const std::string format = vk::to_string(info.pixel_format);
    const bool is_bc5 = format.find("Bc5") != std::string::npos;
    const bool is_bc13 = format.find("Bc1") != std::string::npos ||
                         format.find("Bc3") != std::string::npos;
    if (!is_bc5 && !is_bc13) {
        return;
    }
    static std::atomic<u32> counts[4]{};
    auto& counter = counts[(noisy ? 0 : 2) + (is_bc5 ? 1 : 0)];
    const u32 limit = noisy ? (is_bc5 ? 6 : 12) : (is_bc5 ? 1 : 3);
    const u32 index = counter.fetch_add(1);
    if (index >= limit) {
        return;
    }
    const char* env_dir = std::getenv("SHADPS4_TEX_DUMP_DIR");
    const std::filesystem::path dir = env_dir != nullptr ? env_dir : "C:/aq/texdump";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string name = fmt::format("{}_{}{}_{:x}_{}x{}_m{}_{}.bin", noisy ? "NOISE" : "CLEAN",
                                         is_bc5 ? "bc5_" : "bc13_", index, info.guest_address,
                                         info.size.width, info.size.height, info.resources.levels,
                                         format);
    std::ofstream out(dir / name, std::ios::binary);
    out.write(std::bit_cast<const char*>(info.guest_address),
              static_cast<std::streamsize>(info.guest_size));
    LOG_WARNING(Render_Vulkan, "TEXAUDIT: DUMPED {} ({:#x} bytes) to {}", name,
                static_cast<u64>(info.guest_size), (dir / name).string());
}

static u64 HashOf(const Image& image) {
    const u64 size = std::min<u64>(image.info.guest_size, u64{4} << 20);
    return XXH3_64bits(std::bit_cast<const u8*>(image.info.guest_address), size);
}

// True when the first (up to) 64 KiB at addr is all zero.
static bool RangeIsZero(VAddr addr, u64 size) {
    size = std::min<u64>(size, u64{64} << 10) & ~u64{7};
    const auto* words = std::bit_cast<const u64*>(addr);
    for (u64 i = 0; i < size / 8; ++i) {
        if (words[i] != 0) {
            return false;
        }
    }
    return size != 0;
}

// One letter per mip level: Z = empty (zero where it starts), D = has data.
static std::string MipState(const ImageInfo& info) {
    std::string state;
    const u32 levels = std::min<u32>(info.resources.levels, 16);
    for (u32 m = 0; m < levels; ++m) {
        const auto& layout = info.mips_layout[m];
        state += RangeIsZero(info.guest_address + layout.offset, layout.size) ? 'Z' : 'D';
    }
    return state;
}

// Is the data of a block compressed mip level spatially random? Consecutive blocks in memory are
// next to each other in the picture (thin 1D tiling), so for a real picture the first endpoint of
// neighbouring blocks is close, for noise it is not. Returns 'N' (noise), 'C' (clean), '?' (in
// between), '.' (not checked: too small, not BC1/3/4/5) or 'Z' (empty).
static char NoiseOfMip(const ImageInfo& info, u32 mip) {
    u32 block_bytes = 0, field_offset = 0, field_bits = 0;
    switch (info.pixel_format) {
    case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
        block_bytes = 8, field_offset = 0, field_bits = 16;
        break;
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc3SrgbBlock:
        block_bytes = 16, field_offset = 8, field_bits = 16;
        break;
    case vk::Format::eBc4UnormBlock:
    case vk::Format::eBc4SnormBlock:
        block_bytes = 8, field_offset = 0, field_bits = 8;
        break;
    case vk::Format::eBc5UnormBlock:
    case vk::Format::eBc5SnormBlock:
        block_bytes = 16, field_offset = 0, field_bits = 8;
        break;
    default:
        return '.';
    }
    const auto& layout = info.mips_layout[mip];
    const u64 bytes = std::min<u64>(layout.size, 8192) / block_bytes * block_bytes;
    if (bytes < block_bytes * 16) {
        return '.';
    }
    const auto* data = std::bit_cast<const u8*>(info.guest_address + layout.offset);
    const auto field = [&](u64 block) -> u32 {
        const u8* p = data + block * block_bytes + field_offset;
        if (field_bits == 16) {
            return (u32{p[0]} | (u32{p[1]} << 8)) >> 5; // top 11 bits of the 565 colour
        }
        return p[0];
    };
    const u64 blocks = bytes / block_bytes;
    u64 sum = 0, count = 0;
    bool any = false;
    for (u64 b = 0; b + 1 < blocks; b += 2) {
        const u32 x = field(b), y = field(b + 1);
        sum += x > y ? x - y : y - x;
        ++count;
    }
    for (u64 i = 0; i < bytes; i += 8) {
        u64 word;
        std::memcpy(&word, data + i, 8);
        any |= word != 0;
    }
    if (!any) {
        return 'Z';
    }
    const double range = field_bits == 16 ? 2048.0 : 256.0;
    const double score = (static_cast<double>(sum) / static_cast<double>(count)) / (range / 3.0);
    return score > 0.6 ? 'N' : score < 0.3 ? 'C' : '?';
}

static std::string NoiseState(const ImageInfo& info) {
    std::string state;
    const u32 levels = std::min<u32>(info.resources.levels, 16);
    for (u32 m = 0; m < levels; ++m) {
        state += NoiseOfMip(info, m);
    }
    return state;
}

struct NoiseRecord {
    std::chrono::steady_clock::time_point first;
    std::chrono::steady_clock::time_point checked;
    u64 uses = 0;
    bool reported = false;
    std::string mips;
};
static std::map<std::pair<u64, u64>, NoiseRecord> noise_state;
static u32 noise_logged = 0;
static u32 noise_sampled_logged = 0;
static const auto process_start = std::chrono::steady_clock::now();
static double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - process_start).count();
}
static std::chrono::steady_clock::time_point last_heartbeat{};
static std::set<std::tuple<u64, u64, u32, u32>> noise_sampled_keys;

// SHADPS4_MIP_FALLBACK=1: while the sharpest mips of a colour texture are still empty or hold
// random leftovers (the game streams the small mips first), sample from the first good mip.
// 1: colour textures (BC1/BC3). 2: also normal maps and other one/two channel textures (BC4/BC5),
// which the noise test is less sure about: they lose their fine detail while they are bad.
static int FallbackMode() {
    static const int mode = [] {
        const char* value = std::getenv("SHADPS4_MIP_FALLBACK");
        return value == nullptr ? 0 : std::clamp(std::atoi(value), 1, 2);
    }();
    return mode;
}
static bool FallbackEnabled() {
    return FallbackMode() > 0;
}
static std::map<std::pair<u64, u64>, u32> fallback_base;
static u32 fallback_logged = 0;

// Record of who wrote into guest memory recently (HLE copy kernel and host file reads), to say
// where the data of a noisy texture came from.
struct WriteEvent {
    double t;
    u64 dst;
    u64 bytes;
    u64 src;       // copy: source address; read: offset in the file
    bool is_copy;
    u64 tag;       // read: size of the file
    u32 file_id;   // read: index into file_names
    char dst_cls[8]; // what 8 places spread over the written range looked like after the write
    char src_cls[8]; // the same for the source range of a copy
};

// Looks at up to 2 KiB of memory as block compressed texture data and says what it is: Z empty,
// C a picture (neighbouring blocks have close colours/values in some block layout), N random
// (no layout fits), ? in between, . too short.
static char ClassOfBytes(const u8* p, u64 len) {
    len &= ~u64{15};
    if (len < 256) {
        return '.';
    }
    bool any = false;
    for (u64 i = 0; i < len; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        if (w != 0) {
            any = true;
            break;
        }
    }
    if (!any) {
        return 'Z';
    }
    const auto score = [&](u32 block, u32 off, u32 bits) {
        u64 sum = 0, count = 0;
        for (u64 b = 0; (b + 2) * block <= len; b += 2) {
            const u8* a = p + b * block + off;
            const u8* c = a + block;
            u32 x, y;
            if (bits == 16) {
                x = (u32{a[0]} | (u32{a[1]} << 8)) >> 5;
                y = (u32{c[0]} | (u32{c[1]} << 8)) >> 5;
            } else {
                x = a[0];
                y = c[0];
            }
            sum += x > y ? x - y : y - x;
            ++count;
        }
        const double range = bits == 16 ? 2048.0 : 256.0;
        return count ? (static_cast<double>(sum) / static_cast<double>(count)) / (range / 3.0)
                     : 1.0;
    };
    const double s1 = score(8, 0, 16), s3 = score(16, 8, 16), s5 = score(16, 0, 8);
    const double lo = std::min(s1, std::min(s3, s5));
    return lo < 0.3 ? 'C' : lo > 0.6 ? 'N' : '?';
}

static void ClassSamples(u64 addr, u64 bytes, char (&out)[8]) {
    for (u64 i = 0; i < 8; ++i) {
        const u64 off = ((bytes * i) / 8) & ~u64{15};
        const u64 len = std::min<u64>(2048, bytes > off ? bytes - off : 0);
        out[i] = ClassOfBytes(std::bit_cast<const u8*>(addr + off), len);
    }
}
static std::mutex write_mutex;
static std::vector<std::string> file_names;
static u32 InternFile(const std::string& path) {
    for (u32 i = 0; i < file_names.size(); ++i) {
        if (file_names[i] == path) {
            return i;
        }
    }
    file_names.push_back(path);
    return static_cast<u32>(file_names.size() - 1);
}
static std::vector<WriteEvent> write_ring;
static size_t write_head = 0;
constexpr size_t WriteRingSize = 40000;

// Staging hazards: file data lands in staging memory (host reads), then the HLE copy kernel moves
// it into the pool. If a read overwrites staging bytes no copy has consumed yet, a chunk is lost
// (the game thought the GPU was done); if a copy reads staging bytes that were already consumed,
// or that are filled only afterwards, the GPU got stale data. Both put the wrong bytes in the
// pool. Granularity 256 bytes.
struct StagePage {
    double t;
    u32 file_id;
    u64 file_off;
};
static std::unordered_map<u64, StagePage> stage_pending;  // read, not yet consumed
static std::unordered_set<u64> stage_ever_read;
static std::unordered_map<u64, double> stage_stale_at;     // copy took it while not pending
static u64 hz_reads = 0, hz_copies = 0, hz_lost_pages = 0, hz_lost_events = 0;
static u64 hz_stale_pages = 0, hz_stale_events = 0, hz_late_events = 0, hz_events = 0;
static u32 hz_logged_lost = 0, hz_logged_stale = 0, hz_logged_late = 0;

static void StageHazard(const WriteEvent& e) {
    // caller holds write_mutex
    if (stage_pending.size() > 6000000) {
        return;
    }
    const u64 first = e.dst >> 8;
    const u64 last = (e.dst + e.bytes - 1) >> 8;
    if (!e.is_copy) {
        // only the texture stream files matter here (other reads are consumed by the CPU)
        if (e.file_id >= file_names.size() ||
            file_names[e.file_id].find("streams") == std::string::npos) {
            return;
        }
        ++hz_reads;
        u64 lost = 0, late = 0;
        u64 old_file = 0, old_off = 0;
        double old_t = 0;
        u64 lost_at = 0;
        for (u64 p = first; p <= last; ++p) {
            const auto it = stage_pending.find(p);
            if (it != stage_pending.end()) {
                if (lost == 0) {
                    old_file = it->second.file_id;
                    old_off = it->second.file_off;
                    old_t = it->second.t;
                    lost_at = p << 8;
                }
                ++lost;
            }
            const auto st = stage_stale_at.find(p);
            if (st != stage_stale_at.end()) {
                if (e.t - st->second < 1.0) {
                    ++late;
                }
                stage_stale_at.erase(st);
            }
            stage_pending[p] = StagePage{e.t, e.file_id, e.src};
            stage_ever_read.insert(p);
        }
        if (lost) {
            hz_lost_pages += lost;
            ++hz_lost_events;
            if (hz_logged_lost++ < 60) {
                LOG_WARNING(Render_Vulkan,
                            "TEXAUDIT: STAGING HAZARD lost chunk: read t={:.2f}s {} @{:#x} "
                            "overwrote {} KiB at {:#x} that no copy had consumed (older read "
                            "t={:.2f}s {} @{:#x})",
                            e.t, e.file_id < file_names.size() ? file_names[e.file_id] : "?",
                            e.src, (lost * 256) >> 10, lost_at, old_t,
                            old_file < file_names.size() ? file_names[old_file] : "?", old_off);
            }
        }
        if (late) {
            ++hz_late_events;
            if (hz_logged_late++ < 30) {
                LOG_WARNING(Render_Vulkan,
                            "TEXAUDIT: STAGING HAZARD read after copy: read t={:.2f}s {} @{:#x} "
                            "{:#x}+{:#x} arrived after a copy had already used {} KiB of that "
                            "range",
                            e.t, e.file_id < file_names.size() ? file_names[e.file_id] : "?",
                            e.src, e.dst, e.bytes, (late * 256) >> 10);
            }
        }
    } else {
        ++hz_copies;
        const u64 sfirst = e.src >> 8;
        const u64 slast = (e.src + e.bytes - 1) >> 8;
        u64 stale = 0, stale_at = 0;
        for (u64 p = sfirst; p <= slast; ++p) {
            const auto it = stage_pending.find(p);
            if (it != stage_pending.end()) {
                stage_pending.erase(it);
            } else if (stage_ever_read.count(p)) {
                if (stale == 0) {
                    stale_at = p << 8;
                }
                ++stale;
                stage_stale_at[p] = e.t;
            }
        }
        for (u64 p = first; p <= last; ++p) {
            // a copy wrote here: no longer file data waiting
            stage_pending.erase(p);
            stage_ever_read.erase(p);
        }
        if (stale) {
            hz_stale_pages += stale;
            ++hz_stale_events;
            if (hz_logged_stale++ < 30) {
                LOG_WARNING(Render_Vulkan,
                            "TEXAUDIT: STAGING HAZARD stale copy: t={:.2f}s copy {:#x}+{:#x} <- "
                            "{:#x} used {} KiB (first at {:#x}) of staging that was already "
                            "consumed or not yet filled",
                            e.t, e.dst, e.bytes, e.src, (stale * 256) >> 10, stale_at);
            }
        }
    }
    if ((++hz_events % 500) == 0) {
        LOG_WARNING(Render_Vulkan,
                    "TEXAUDIT: STAGING summary after {} events: {} reads, {} copies; lost chunks "
                    "{} (events) / {} MiB; stale copies {} / {} MiB; reads after copy {}",
                    hz_events, hz_reads, hz_copies, hz_lost_events, (hz_lost_pages * 256) >> 20,
                    hz_stale_events, (hz_stale_pages * 256) >> 20, hz_late_events);
    }
}

static std::string WritersOf(u64 begin, u64 end) {
    std::scoped_lock lock{write_mutex};
    std::string out;
    int found = 0;
    const size_t n = write_ring.size();
    for (size_t i = 0; i < n && found < 3; ++i) {
        const auto& e = write_ring[(write_head + n - 1 - i) % n];
        if (e.dst < end && e.dst + e.bytes > begin) {
            out += fmt::format(" [{} t={:.1f}s dst={:#x}+{:#x} {}={:#x}]",
                               e.is_copy ? "copy" : "read", e.t, e.dst, e.bytes,
                               e.is_copy ? "src" : "file_off", e.src);
            ++found;
        }
    }
    return found ? out : std::string{" [no recorded writer]"};
}

// Where the first bytes at addr came from: the newest write that covers them, then the newest
// older write that covers the source of that one, and so on, with what the data looked like at
// each step.
struct ChainResult {
    std::string text;
    bool from_file = false;
    std::string path;
    u64 file_off = 0;  // offset in the file of the bytes that were at the start address
    u64 file_run = 0;  // how many bytes from there are covered by the same read
    u64 start = 0;
};
static ChainResult ChainOf(u64 addr) {
    ChainResult res;
    res.start = addr;
    std::scoped_lock lock{write_mutex};
    std::string& out = res.text;
    const size_t n = write_ring.size();
    size_t i = 0;
    for (int hop = 0; hop < 6; ++hop) {
        bool found = false;
        for (; i < n; ++i) {
            const auto& e = write_ring[(write_head + n - 1 - i) % n];
            if (addr < e.dst || addr >= e.dst + e.bytes) {
                continue;
            }
            found = true;
            const u64 k = std::min<u64>(7, (addr - e.dst) * 8 / e.bytes);
            if (e.is_copy) {
                out += fmt::format(" <-copy t={:.1f} {:#x}+{:#x} from {:#x} [dst {} src {}]", e.t,
                                   e.dst, e.bytes, e.src, e.dst_cls[k], e.src_cls[k]);
                addr = e.src + (addr - e.dst);
            } else {
                out += fmt::format(" <-read t={:.1f} {:#x}+{:#x} file_off={:#x} of a {:#x}-byte "
                                   "file [data {}]",
                                   e.t, e.dst, e.bytes, e.src, e.tag, e.dst_cls[k]);
            }
            ++i;
            if (!e.is_copy) {
                res.from_file = true;
                res.path = e.file_id < file_names.size() ? file_names[e.file_id] : std::string{};
                res.file_off = e.src + (addr - e.dst);
                res.file_run = e.dst + e.bytes - addr;
                return res;
            }
            break;
        }
        if (!found) {
            out += " <-[no recorded writer: the game's own code or a shader]";
            return res;
        }
    }
    return res;
}

// Compares the first bytes of the memory at the start of a chain that ends in a file read with
// what the file holds there now.
static std::string FileCheck(const ChainResult& chain, u64 max_bytes) {
    if (!chain.from_file || chain.path.empty()) {
        return {};
    }
    const u64 len = std::min<u64>({u64{4096}, chain.file_run, max_bytes});
    std::vector<char> disk(len);
    std::ifstream in(std::filesystem::path(chain.path), std::ios::binary);
    if (!in) {
        return " FILECHECK: cannot open the file";
    }
    in.seekg(static_cast<std::streamoff>(chain.file_off));
    in.read(disk.data(), static_cast<std::streamsize>(len));
    const u64 got = static_cast<u64>(in.gcount());
    const auto* mem = std::bit_cast<const char*>(chain.start);
    u64 first = ~u64{0}, diff = 0;
    for (u64 i = 0; i < got; ++i) {
        if (mem[i] != disk[i]) {
            if (first == ~u64{0}) {
                first = i;
            }
            ++diff;
        }
    }
    const auto name = std::filesystem::path(chain.path).filename().string();
    if (got == 0) {
        return " FILECHECK: nothing read from the file";
    }
    if (diff == 0) {
        return fmt::format(" FILECHECK: the first {} bytes EQUAL the file {} at {:#x}", got, name,
                           chain.file_off);
    }
    return fmt::format(" FILECHECK: DIFFERS from the file {} at {:#x}: {} of {} bytes differ, "
                       "first at +{}",
                       name, chain.file_off, diff, got, first);
}
static u32 clean_chain_logged = 0;

// Follows 8 places spread over [begin, begin+size) back to the files they came from. Data that
// was loaded as a whole comes from one file at consecutive offsets; data that was put together
// from unrelated pieces (the wrong thing in the place) does not.
static std::string LayoutOf(u64 begin, u64 size) {
    constexpr int N = 8;
    struct Sample {
        bool from_file = false;
        std::string name;
        u64 off = 0;
        u64 delta = 0;
    };
    Sample s[N];
    for (int k = 0; k < N; ++k) {
        s[k].delta = (size * k / N) & ~u64{255};
        const auto c = ChainOf(begin + s[k].delta);
        s[k].from_file = c.from_file;
        s[k].name = std::filesystem::path(c.path).filename().string();
        s[k].off = c.file_off;
    }
    bool consistent = true;
    for (int k = 0; k < N; ++k) {
        if (!s[k].from_file || s[k].name != s[0].name ||
            s[k].off - s[0].off != s[k].delta - s[0].delta) {
            consistent = false;
        }
    }
    const auto short_name = [](const std::string& n) {
        return n.size() > 40 ? n.substr(n.size() - 40) : n;
    };
    if (consistent) {
        return fmt::format(" LAYOUT consistent: one file {} from {:#x}", short_name(s[0].name),
                           s[0].off);
    }
    std::string out = " LAYOUT MIXED:";
    for (int k = 0; k < N; ++k) {
        if (s[k].from_file) {
            out += fmt::format(" [{}:{}@{:#x}]", k, short_name(s[k].name), s[k].off);
        } else {
            out += fmt::format(" [{}:no file]", k);
        }
    }
    return out;
}

// How much of [begin, end) was written by anything the ring remembers (HLE copies and file
// reads), and by how many writes: a texture that never got its data written stays low.
static std::string CoverageOf(u64 begin, u64 end) {
    std::vector<std::pair<u64, u64>> parts;
    {
        std::scoped_lock lock{write_mutex};
        for (const auto& e : write_ring) {
            if (e.dst < end && e.dst + e.bytes > begin) {
                parts.emplace_back(std::max(e.dst, begin), std::min(e.dst + e.bytes, end));
            }
        }
    }
    const size_t writes = parts.size();
    std::sort(parts.begin(), parts.end());
    u64 covered = 0, cursor = begin;
    for (const auto& [a, z] : parts) {
        const u64 from = std::max(a, cursor);
        if (z > from) {
            covered += z - from;
            cursor = z;
        }
    }
    const u64 total = end > begin ? end - begin : 1;
    return fmt::format(" mip0 written {}% by {} writes", covered * 100 / total, writes);
}

static bool VerifyEnabled() {
    static const bool enabled = std::getenv("SHADPS4_DBG_TEX_VERIFY") != nullptr;
    return enabled;
}
static u32 verify_done = 0;
static u32 verify_bad = 0;
static u32 verify_logged = 0;

static bool IsTarget(const Image& image) {
    return image.usage.render_target || image.usage.depth_target;
}

static bool StartsZero(const Image& image) {
    const u64 size = std::min<u64>(image.info.guest_size, u64{64} << 10) & ~u64{7};
    const auto* words = std::bit_cast<const u64*>(image.info.guest_address);
    for (u64 i = 0; i < size / 8; ++i) {
        if (words[i] != 0) {
            return false;
        }
    }
    return size != 0;
}

// Shadow page hashes (SHADPS4_DBG_TEX_AUDIT): every recorded write (HLE copy, host file read)
// stores an XXH3 hash of each whole 4 KiB page it wrote. Before a copy reads its source, and when a
// texture is uploaded, the pages are hashed again: a page whose hash no longer matches was
// changed by something that is not recorded (the game's CPU, a remap of the address range, a
// shader), after the last write we know about.
struct ShadowPage {
    u64 hash;
    float t;
    u32 file_id;
    u8 kind; // 1 copy, 2 file read
};
static std::mutex shadow_mutex;
static std::unordered_map<u64, ShadowPage> shadow;
static u64 sh_copies = 0, sh_copies_changed = 0, sh_pages_checked = 0, sh_pages_changed = 0,
           sh_pages_unknown = 0;
static u32 sh_logged = 0;

static void ShadowRecord(u64 dst, u64 bytes, u8 kind, u32 file_id) {
    const u64 end = dst + bytes;
    const u64 first = (dst + 0xfff) >> 12;
    const u64 last = end >> 12;
    std::vector<std::pair<u64, u64>> hashes;
    for (u64 p = first; p < last; ++p) {
        hashes.emplace_back(p, XXH3_64bits(std::bit_cast<const void*>(p << 12), 4096));
    }
    const float t = static_cast<float>(Seconds());
    std::scoped_lock lock{shadow_mutex};
    if ((dst & 0xfff) != 0) {
        shadow.erase(dst >> 12);
    }
    if ((end & 0xfff) != 0) {
        shadow.erase(end >> 12);
    }
    for (const auto& [p, h] : hashes) {
        shadow[p] = {h, t, file_id, kind};
    }
}

struct ShadowResult {
    u64 checked = 0, changed = 0, unknown = 0;
    u64 first_changed = 0;
    ShadowPage first{};
};
static ShadowResult ShadowCheck(u64 begin, u64 bytes) {
    ShadowResult r;
    const u64 first = (begin + 0xfff) >> 12;
    const u64 last = (begin + bytes) >> 12;
    std::vector<std::pair<u64, ShadowPage>> known;
    {
        std::scoped_lock lock{shadow_mutex};
        for (u64 p = first; p < last; ++p) {
            const auto it = shadow.find(p);
            if (it == shadow.end()) {
                ++r.unknown;
            } else {
                known.emplace_back(p, it->second);
            }
        }
    }
    for (const auto& [p, sp] : known) {
        ++r.checked;
        if (XXH3_64bits(std::bit_cast<const void*>(p << 12), 4096) != sp.hash) {
            if (r.changed++ == 0) {
                r.first_changed = p << 12;
                r.first = sp;
            }
        }
    }
    return r;
}


// What the write ring knows about a page that changed after it was recorded: later recorded
// writes that overlap it (there should be none or the shadow would have been updated), and what
// the page looks like now.
static std::string ChangedPageInfo(u64 page, float since) {
    std::string out;
    u32 later = 0;
    {
        std::scoped_lock lock{write_mutex};
        for (const auto& e : write_ring) {
            if (e.dst < page + 4096 && e.dst + e.bytes > page && e.t > static_cast<double>(since) + 1e-6) {
                if (later++ < 3) {
                    out += fmt::format(" [later {} t={:.2f}s dst={:#x}+{:#x}]",
                                       e.is_copy ? "copy" : "read", e.t, e.dst, e.bytes);
                }
            }
        }
    }
    const u8* p = std::bit_cast<const u8*>(page);
    bool zero = true;
    for (u64 i = 0; i < 4096; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        if (w != 0) {
            zero = false;
            break;
        }
    }
    return fmt::format(" now={} later recorded writes overlapping: {}{}",
                       zero ? "ZERO" : std::string(1, ClassOfBytes(p, 2048)), later, out);
}

static std::string ShadowText(u64 begin, u64 bytes) {
    const auto r = ShadowCheck(begin, bytes);
    return fmt::format(" SHADOW pages: {} recorded, {} changed since, {} never recorded{}",
                       r.checked, r.changed, r.unknown,
                       r.changed != 0
                           ? fmt::format(" (first changed {:#x}, written t={:.2f}s by {})",
                                         r.first_changed, r.first.t,
                                         r.first.kind == 1 ? "a copy" : "a file read")
                           : std::string{});
}
} // namespace TexAudit

static void TexAuditAddEvent(u64 dst, u64 bytes, u64 src, bool is_copy, u64 tag,
                             const std::string& path = {}) {
    if (!TexAudit::Enabled() || bytes == 0) {
        return;
    }
    TexAudit::WriteEvent e{TexAudit::Seconds(), dst, bytes, src, is_copy, tag, 0, {}, {}};
    if (!path.empty()) {
        std::scoped_lock lock{TexAudit::write_mutex};
        e.file_id = TexAudit::InternFile(path);
    }
    TexAudit::ClassSamples(dst, bytes, e.dst_cls);
    if (is_copy) {
        TexAudit::ClassSamples(src, bytes, e.src_cls);
    } else {
        std::memset(e.src_cls, '.', sizeof(e.src_cls));
    }
    {
        std::scoped_lock lock{TexAudit::write_mutex};
        TexAudit::StageHazard(e);
        if (TexAudit::write_ring.size() < TexAudit::WriteRingSize) {
            TexAudit::write_ring.push_back(e);
            TexAudit::write_head = TexAudit::write_ring.size() % TexAudit::WriteRingSize;
        } else {
            TexAudit::write_ring[TexAudit::write_head] = e;
            TexAudit::write_head = (TexAudit::write_head + 1) % TexAudit::WriteRingSize;
        }
    }
    TexAudit::ShadowRecord(dst, bytes, is_copy ? 1 : 2, e.file_id);
}


// Content based "first good mip" for streamed block compressed textures. The game only makes the
// mips it needs for the console's picture size resident; the top mip(s) of the rest hold other
// assets' data. A real mip chain is a downsampling pyramid, so mip m+1 must look like the average
// of 2x2 blocks of mip m; for garbage it does not. The first mip from which on every level
// agrees with the next one is the first good one; the rasterizer raises the sampler's min LOD to
// it. SHADPS4_NO_CONTENT_MIN_LOD=1 turns this off.
namespace ContentLod {
static std::mutex mutex;
struct Entry {
    u32 fg = 0;
    ImageInfo info;
    std::chrono::steady_clock::time_point checked;
};
static std::unordered_map<u64, Entry> first_good;
static std::unordered_map<u64, std::string> skipped;
static std::set<u64> unknown_logged;
static void Update(const ImageInfo& info, bool quick = false);
static u32 logged = 0;
static u64 evaluated = 0, clamped = 0, evaluated_bad = 0;

static bool Enabled() {
    static const bool enabled = std::getenv("SHADPS4_NO_CONTENT_MIN_LOD") == nullptr;
    return enabled;
}

static u32 Morton(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | (((x >> 1) & 1) << 2) | (((y >> 1) & 1) << 3) |
           (((x >> 2) & 1) << 4) | (((y >> 2) & 1) << 5);
}

// kind: 1 = BC1 (8 bytes), 3 = BC3 (16, colour at +8), 5 = BC5 unorm, 6 = BC5 snorm.
static float BlockMean(const u8* p, u32 kind) {
    const auto colour = [](const u8* c) {
        const u32 c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8);
        const auto lum = [](u32 v) {
            const u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            return (r * 255.0f / 31.0f + g * 255.0f / 63.0f + b * 255.0f / 31.0f) / 3.0f;
        };
        return 0.5f * (lum(c0) + lum(c1));
    };
    switch (kind) {
    case 1:
        return colour(p);
    case 3:
        return colour(p + 8);
    case 5:
        return 0.5f * (p[0] + p[1]);
    default:
        return 0.5f * (static_cast<s8>(p[0]) + static_cast<s8>(p[1]));
    }
}

// Bumped whenever a verdict is added or changes, so that the lookups' per-thread caches know.
static std::atomic<u64> version{1};
static void UpdateImpl(const ImageInfo& info, bool quick);
static void Update(const ImageInfo& info, bool quick) {
    UpdateImpl(info, quick);
}
static void UpdateImpl(const ImageInfo& info, bool quick) {
    u32 kind = 0, bb = 0, blk = 4;
    switch (info.pixel_format) {
    case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
        kind = 1, bb = 8;
        break;
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc3SrgbBlock:
        kind = 3, bb = 16;
        break;
    case vk::Format::eBc5UnormBlock:
        kind = 5, bb = 16;
        break;
    case vk::Format::eBc5SnormBlock:
        kind = 6, bb = 16;
        break;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        kind = 7, bb = 4, blk = 1;
        break;
    case vk::Format::eR8Unorm:
        kind = 8, bb = 1, blk = 1;
        break;
    default: {
        std::lock_guard lock{mutex};
        skipped[info.guest_address] = fmt::format("format {}", vk::to_string(info.pixel_format));
        return;
    }
    }
    const u32 levels = std::min<u32>(info.resources.levels, 16);
    if (levels < 3 || static_cast<u32>(info.tile_mode) != 13 || info.resources.layers != 1) {
        std::lock_guard lock{mutex};
        skipped[info.guest_address] = fmt::format("levels {} tile {} layers {}", levels,
                                                  static_cast<u32>(info.tile_mode),
                                                  info.resources.layers);
        return;
    }
    struct Dim {
        u32 bw, bh, pw, ph;
        const u8* data;
        bool ok;
    };
    std::array<Dim, 16> dims{};
    for (u32 m = 0; m < levels; ++m) {
        const u32 w = std::max(1u, info.size.width >> m), h = std::max(1u, info.size.height >> m);
        Dim& d = dims[m];
        d.bw = (w + blk - 1) / blk, d.bh = (h + blk - 1) / blk;
        d.pw = (d.bw + 7) & ~7u, d.ph = (d.bh + 7) & ~7u;
        d.data = std::bit_cast<const u8*>(info.guest_address + info.mips_layout[m].offset);
        d.ok = info.mips_layout[m].size >= u64{d.pw} * d.ph * bb;
        if (!d.ok && m < 2) {
            std::lock_guard lock{mutex};
            skipped[info.guest_address] = fmt::format("mip {} layout size {:#x} < {:#x}", m,
                                                      info.mips_layout[m].size,
                                                      u64{d.pw} * d.ph * bb);
        }
    }
    const float thr = kind == 6 ? 4.0f : kind == 5 ? 10.0f : kind >= 7 ? 5.0f : 30.0f;
    const auto at = [&](u32 m, u32 X, u32 Y) {
        const Dim& d = dims[m];
        const u64 idx = (u64{(Y >> 3) * (d.pw >> 3) + (X >> 3)} << 6) + Morton(X & 7, Y & 7);
        return d.data + idx * bb;
    };
    // Uncompressed textures: the mean over a whole 8x8 micro tile (64 consecutive elements)
    // does not depend on the order of the elements inside the tile.
    const auto tile_mean = [&](u32 m, u32 tx, u32 ty) {
        const Dim& d = dims[m];
        const u8* p = d.data + (u64{ty * (d.pw >> 3) + tx} << 6) * bb;
        u32 sum = 0;
        if (kind == 8) {
            for (u32 i = 0; i < 64; ++i) {
                sum += p[i];
            }
            return static_cast<float>(sum) / 64.0f;
        }
        for (u32 i = 0; i < 64; ++i) {
            sum += p[i * 4] + p[i * 4 + 1] + p[i * 4 + 2];
        }
        return static_cast<float>(sum) / 192.0f;
    };
    u32 last_bad = 0;
    bool any_bad = false;
    std::string medians;
    const u32 pairs = quick ? std::min<u32>(levels - 1, 2) : levels - 1;
    for (u32 m = 0; m < pairs; ++m) {
        const Dim& parent = dims[m + 1];
        const Dim& child = dims[m];
        if (!parent.ok || !child.ok || parent.bw < 8 || parent.bh < 8) {
            medians += " -";
            continue;
        }
        std::array<float, 512> errs;
        const u32 nsamples = quick ? 160 : 512;
        for (u32 k = 0; k < errs.size(); ++k) {
            if (k >= nsamples) {
                errs[k] = 0.0f;
                continue;
            }
            if (kind >= 7) {
                const u32 txp = parent.bw / 8, typ = parent.bh / 8;
                const u32 TX = (k * 37 + 5) % txp, TY = (k * 53 + 11 + k / txp) % typ;
                float csum = 0.0f;
                u32 cn = 0;
                for (u32 dy = 0; dy < 2; ++dy) {
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u32 cx = 2 * TX + dx, cy = 2 * TY + dy;
                        if (cx < child.bw / 8 && cy < child.bh / 8) {
                            csum += tile_mean(m, cx, cy);
                            ++cn;
                        }
                    }
                }
                errs[k] = cn != 0 ? std::abs(csum / static_cast<float>(cn) - tile_mean(m + 1, TX, TY))
                                  : 0.0f;
                continue;
            }
            const u32 X = (k * 37 + 5) % parent.bw, Y = (k * 53 + 11 + k / parent.bw) % parent.bh;
            const float p = BlockMean(at(m + 1, X, Y), kind);
            float sum = 0.0f;
            u32 n = 0;
            for (u32 dy = 0; dy < 2; ++dy) {
                for (u32 dx = 0; dx < 2; ++dx) {
                    const u32 cx = 2 * X + dx, cy = 2 * Y + dy;
                    if (cx < child.bw && cy < child.bh) {
                        sum += BlockMean(at(m, cx, cy), kind);
                        ++n;
                    }
                }
            }
            errs[k] = n != 0 ? std::abs(sum / static_cast<float>(n) - p) : 0.0f;
        }
        std::sort(errs.begin(), errs.begin() + nsamples);
        const float median = errs[nsamples / 2];
        const float p90 = errs[nsamples * 9 / 10];
        if (logged < 120) {
            medians += fmt::format(" {:.0f}/{:.0f}", median, p90);
        }
        // The top mip is the one that is usually missing, so it also gets the stricter 90th
        // percentile test that catches partly filled or patchwork data.
        const float p90_thr = kind == 6 ? 10.0f : kind == 5 ? 25.0f : kind >= 7 ? 20.0f : 45.0f;
        if (median > thr || (m == 0 && p90 > p90_thr)) {
            any_bad = true;
            last_bad = m;
        }
    }
    u32 fg = any_bad ? std::min(last_bad + 1, levels - 1) : 0;
    std::lock_guard lock{mutex};
    if (quick) {
        // Only the two top pairs were looked at: keep an earlier verdict about deeper mips.
        const auto old = first_good.find(info.guest_address);
        if (old != first_good.end() && old->second.fg > 2) {
            fg = old->second.fg;
        }
    }
    ++evaluated;
    evaluated_bad += fg != 0 ? 1 : 0;
    const auto it = first_good.find(info.guest_address);
    const u32 before = it != first_good.end() ? it->second.fg : 0;
    const bool is_new = it == first_good.end();
    first_good[info.guest_address] = Entry{fg, info, std::chrono::steady_clock::now()};
    if (fg != before || is_new) {
        version.fetch_add(1, std::memory_order_release);
    }
    if (fg != before || (fg != 0 && is_new)) {
        if (logged < 120) {
            ++logged;
            LOG_WARNING(Render_Vulkan,
                        "CONTENT_MIN_LOD: {:#x} {}x{} m{} {}: median pyramid error per mip pair "
                        "[{} ] -> first good mip {} (was {})",
                        static_cast<u64>(info.guest_address), info.size.width, info.size.height,
                        levels, vk::to_string(info.pixel_format), medians, fg, before);
        }
    }
}
} // namespace ContentLod

u32 TexFirstGoodMipImpl(u64 address, u32 width, u32 height, u32 levels, u32 data_format);
u32 TexFirstGoodMip(u64 address, u32 width, u32 height, u32 levels, u32 data_format) {
    if (!ContentLod::Enabled()) {
        return 0;
    }
    return TexFirstGoodMipImpl(address, width, height, levels, data_format);
}
u32 TexFirstGoodMipImpl(u64 address, u32 width, u32 height, u32 levels, u32 data_format) {
    // Most binds ask about a texture that was asked about a moment ago: a small cache per thread
    // answers those without the lock, until a verdict changes or about a second has passed (the
    // time after which the data is looked at again).
    struct CacheEntry {
        u64 address = 0;
        u64 version = 0;
        std::chrono::steady_clock::time_point until{};
        u32 result = 0;
    };
    thread_local std::array<CacheEntry, 2048> cache{};
    const auto now = std::chrono::steady_clock::now();
    CacheEntry& slot = cache[(address >> 4) * 0x9E3779B1u >> 7 & 2047];
    const u64 current_version = ContentLod::version.load(std::memory_order_acquire);
    if (slot.address == address && slot.version == current_version && now < slot.until) {
        return slot.result;
    }
    ImageInfo recheck;
    bool do_recheck = false;
    u32 result = 0;
    {
        std::lock_guard lock{ContentLod::mutex};
        const auto it = ContentLod::first_good.find(address);
        if (it == ContentLod::first_good.end()) {
            if (ContentLod::unknown_logged.size() < 60 &&
                ContentLod::unknown_logged.insert(address).second) {
                const auto why = ContentLod::skipped.find(address);
                LOG_WARNING(Render_Vulkan,
                            "CONTENT_MIN_LOD: sampled texture {:#x} {}x{} levels {} data format {} "
                            "has no verdict: {}",
                            address, width, height, levels, data_format,
                            why != ContentLod::skipped.end() ? why->second
                                                             : std::string{"never refreshed"});
            }
            slot = CacheEntry{address, current_version, now + std::chrono::milliseconds(200), 0};
            return 0;
        }
        result = it->second.fg;
        // The top mips of a streamed texture can be overwritten without the texture being
        // uploaded again: look at the data again about every half second.
        if (std::chrono::steady_clock::now() - it->second.checked > std::chrono::milliseconds(1500)) {
            recheck = it->second.info;
            do_recheck = true;
            it->second.checked = std::chrono::steady_clock::now();
        }
    }
    if (do_recheck) {
        ContentLod::Update(recheck, true);
        std::lock_guard lock{ContentLod::mutex};
        const auto it = ContentLod::first_good.find(address);
        if (it != ContentLod::first_good.end()) {
            result = it->second.fg;
        }
    }
    slot = CacheEntry{address, ContentLod::version.load(std::memory_order_acquire),
                      now + std::chrono::milliseconds(1000), result};
    return result;
}

// Called by the rasterizer for every sampler bind (SHADPS4_DBG_TEX_AUDIT): does the range of mips
// this draw can actually reach (T# base..last level, clamped by the T#/S# min_lod) contain data
// the noise detector flagged? Counts and logs the first cases.
void TexAuditSampleCheck(u64 tex_addr, u32 base_level, u32 last_level, u32 eff_min_lod) {
    if (!TexAudit::Enabled()) {
        return;
    }
    static u64 binds = 0, flagged_tex = 0, reaches_noise = 0, clamped_clean = 0, logged = 0;
    ++binds;
    auto it = TexAudit::noise_state.lower_bound({tex_addr, 0});
    if (it == TexAudit::noise_state.end() || it->first.first != tex_addr) {
        return;
    }
    ++flagged_tex;
    const std::string& m = it->second.mips;
    const u32 first = std::max(base_level, eff_min_lod);
    bool hit = false;
    for (u32 i = first; i <= last_level && i < m.size(); ++i) {
        hit |= m[i] == 'N';
    }
    if (hit) {
        ++reaches_noise;
    } else {
        ++clamped_clean;
    }
    if (logged < 80 && (hit || (flagged_tex & 31) == 1)) {
        ++logged;
        LOG_WARNING(Render_Vulkan,
                    "TEXAUDIT: t={:.1f}s SAMPLE-CHECK {:#x} mips {} levels {}..{} eff_min_lod {} -> "
                    "{} (of {} sampler binds: {} on flagged textures, {} reach noisy mips, {} "
                    "avoid them)",
                    TexAudit::Seconds(), tex_addr, m, base_level, last_level, eff_min_lod,
                    hit ? "REACHES NOISE" : "only clean mips", binds, flagged_tex, reaches_noise,
                    clamped_clean);
    }
}

// Called by the HLE copy kernel before it reads its source: has the source changed since the
// last write we recorded there?
void TexAuditCopyPre(u64 src, u64 bytes, u64 dst) {
    if (!TexAudit::Enabled() || bytes == 0) {
        return;
    }
    const auto r = TexAudit::ShadowCheck(src, bytes);
    ++TexAudit::sh_copies;
    TexAudit::sh_pages_checked += r.checked;
    TexAudit::sh_pages_changed += r.changed;
    TexAudit::sh_pages_unknown += r.unknown;
    if (r.changed != 0) {
        ++TexAudit::sh_copies_changed;
        if (TexAudit::sh_logged < 80) {
            ++TexAudit::sh_logged;
            LOG_WARNING(Render_Vulkan,
                        "TEXAUDIT: SRC-CHANGED t={:.2f}s copy src={:#x}+{:#x} -> dst={:#x}: {} of "
                        "{} recorded source pages changed since they were written, {} never "
                        "recorded; first changed page {:#x} (written t={:.2f}s by {})",
                        TexAudit::Seconds(), src, bytes, dst, r.changed, r.checked, r.unknown,
                        r.first_changed, r.first.t, r.first.kind == 1 ? "a copy" : "a file read");
            LOG_WARNING(Render_Vulkan, "TEXAUDIT: SRC-CHANGED detail for page {:#x}:{}",
                        r.first_changed, TexAudit::ChangedPageInfo(r.first_changed, r.first.t));
        }
    }
    if (TexAudit::sh_copies % 500 == 0) {
        LOG_WARNING(Render_Vulkan,
                    "TEXAUDIT: SHADOW summary t={:.1f}s: {} copies, {} with changed source; source "
                    "pages: {} recorded checked, {} changed, {} never recorded",
                    TexAudit::Seconds(), TexAudit::sh_copies, TexAudit::sh_copies_changed,
                    TexAudit::sh_pages_checked, TexAudit::sh_pages_changed,
                    TexAudit::sh_pages_unknown);
    }
}

void TexAuditNoteWrite(u64 dst, u64 bytes, u64 src, bool is_copy) {
    TexAuditAddEvent(dst, bytes, src, is_copy, 0);
}

void TexAuditNoteRead(u64 dst, u64 bytes, u64 file_offset, u64 file_size,
                      const std::string& path) {
    TexAuditAddEvent(dst, bytes, file_offset, false, file_size, path);
}

// Staging guard (Rush of Blood): the game reads a stream file chunk into a staging slot and queues
// a GPU copy kernel that moves it into the texture pool. It reuses the slot as soon as it believes
// the GPU is done. Our GPU thread can be a hundred milliseconds behind the game (shader
// compiles, VR rendering), so a new read can overwrite a chunk the copy has not run for yet and
// that part of the texture is lost for good. The real GPU would have copied it long before. So a
// stream read waits (up to SHADPS4_STAGE_GUARD_MS, default 300) until the copies for the slot's
// previous chunk have run. SHADPS4_STAGE_GUARD=0 turns it off.
namespace StageGuard {
static std::mutex mutex;
static std::unordered_set<u64> pending; // 256 byte pages of unconsumed stream data
static bool Enabled() {
    static const bool value = [] {
        const char* v = std::getenv("SHADPS4_STAGE_GUARD");
        return v != nullptr && v[0] == '1';
    }();
    return value;
}
static s64 TimeoutMs() {
    static const s64 value = [] {
        const char* v = std::getenv("SHADPS4_STAGE_GUARD_MS");
        return v != nullptr ? std::max<s64>(1, std::atoll(v)) : s64{300};
    }();
    return value;
}
static std::atomic<u32> waits{0}, timeouts{0};
static std::atomic<u64> waited_ms_total{0};
} // namespace StageGuard

void StageGuardBeforeRead(u64 dst, u64 bytes, const std::string& path) {
    if (!StageGuard::Enabled() || bytes == 0 || path.find("streams") == std::string::npos) {
        return;
    }
    const u64 first = dst >> 8;
    const u64 last = (dst + bytes - 1) >> 8;
    const auto start = std::chrono::steady_clock::now();
    bool waited = false, timed_out = false;
    u64 busy_pages = 0;
    for (;;) {
        busy_pages = 0;
        {
            std::scoped_lock lock{StageGuard::mutex};
            for (u64 p = first; p <= last; ++p) {
                if (StageGuard::pending.count(p)) {
                    ++busy_pages;
                }
            }
        }
        if (busy_pages == 0) {
            break;
        }
        waited = true;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed >= StageGuard::TimeoutMs()) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (waited) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        const u32 n = StageGuard::waits.fetch_add(1) + 1;
        const u32 t = timed_out ? StageGuard::timeouts.fetch_add(1) + 1 : StageGuard::timeouts.load();
        StageGuard::waited_ms_total.fetch_add(static_cast<u64>(ms));
        if (n <= 60 || (n % 100) == 0) {
            LOG_WARNING(Render_Vulkan,
                        "STAGE GUARD: read into {:#x}+{:#x} waited {} ms for the copy of the "
                        "previous chunk ({}); {} waits, {} timeouts, {} ms in total [{}]",
                        dst, bytes, ms, timed_out ? "TIMED OUT, " + std::to_string(busy_pages) +
                                                       " pages still unconsumed"
                                                  : std::string{"copy ran"},
                        n, t, StageGuard::waited_ms_total.load(), path);
        }
    }
}

void StageGuardAfterRead(u64 dst, u64 bytes, const std::string& path) {
    if (!StageGuard::Enabled() || bytes == 0 || path.find("streams") == std::string::npos) {
        return;
    }
    std::scoped_lock lock{StageGuard::mutex};
    if (StageGuard::pending.size() > 4000000) {
        StageGuard::pending.clear();
    }
    const u64 first = dst >> 8;
    const u64 last = (dst + bytes - 1) >> 8;
    for (u64 p = first; p <= last; ++p) {
        StageGuard::pending.insert(p);
    }
}

void StageGuardOnCopy(u64 src, u64 bytes, u64 dst) {
    if (!StageGuard::Enabled() || bytes == 0) {
        return;
    }
    std::scoped_lock lock{StageGuard::mutex};
    if (StageGuard::pending.empty()) {
        return;
    }
    const u64 first = src >> 8;
    const u64 last = (src + bytes - 1) >> 8;
    for (u64 p = first; p <= last; ++p) {
        StageGuard::pending.erase(p);
    }
    // a copy that writes into staging pages makes whatever was waiting there obsolete
    const u64 dfirst = dst >> 8;
    const u64 dlast = (dst + bytes - 1) >> 8;
    for (u64 p = dfirst; p <= dlast; ++p) {
        StageGuard::pending.erase(p);
    }
}

bool TexAuditMip0Empty(VAddr addr) {
    return TexAudit::mip0_empty.count(addr) != 0;
}

static void LogViewDimensionMismatch(const TextureCache::ImageDesc& desc, const Image& image) {
    if (IsViewTypeCompatible(desc.view_info.type, image.info.type)) {
        return;
    }

    const auto& requested = desc.info;
    const auto& backing = image.info;
    const auto requested_micro = AmdGpu::GetMicroTileMode(requested.tile_mode);
    const auto backing_micro = AmdGpu::GetMicroTileMode(backing.tile_mode);
    const u32 active_backing_samples = image.backing ? image.backing->num_samples : 0;
    const std::string actual_vk_type =
        image.backing ? vk::to_string(image.backing->image.image_ci.imageType) : "None";
    const std::string actual_vk_format =
        image.backing ? vk::to_string(image.backing->image.image_ci.format) : "None";
    const std::string actual_vk_samples =
        image.backing ? vk::to_string(image.backing->image.image_ci.samples) : "None";
    const std::string record = fmt::format(
        "VIEW_DIMENSION_MISMATCH binding={} view={} requested_type={} backing_type={} "
        "request_addr={:#x} request_size={:#x} backing_addr={:#x} backing_size={:#x} "
        "request_extent={}x{}x{} request_pitch={} request_bpp={} request_samples={} "
        "request_levels={} request_layers={} request_tile={} request_array={} "
        "request_micro={} request_thickness={} request_format={} "
        "backing_extent={}x{}x{} backing_pitch={} backing_bpp={} backing_samples={} "
        "active_backing_samples={} actual_vk_type={} actual_vk_format={} actual_vk_samples={} "
        "backing_levels={} backing_layers={} backing_tile={} "
        "backing_array={} backing_micro={} backing_thickness={} backing_format={} "
        "view_format={} mip_base={} mip_count={} layer_base={} layer_count={} storage={} "
        "history_texture={} history_storage={} history_rt={} history_depth={} history_vo={} "
        "bound={} target={} usage_flags={}",
        magic_enum::enum_name(desc.type), magic_enum::enum_name(desc.view_info.type),
        magic_enum::enum_name(requested.type), magic_enum::enum_name(backing.type),
        requested.guest_address, requested.guest_size, backing.guest_address, backing.guest_size,
        requested.size.width, requested.size.height, requested.size.depth, requested.pitch,
        requested.num_bits, requested.num_samples, requested.resources.levels,
        requested.resources.layers, magic_enum::enum_name(requested.tile_mode),
        magic_enum::enum_name(requested.array_mode), magic_enum::enum_name(requested_micro),
        AmdGpu::GetMicroTileThickness(requested.array_mode),
        vk::to_string(requested.pixel_format), backing.size.width, backing.size.height,
        backing.size.depth, backing.pitch, backing.num_bits, backing.num_samples,
        active_backing_samples, actual_vk_type, actual_vk_format, actual_vk_samples,
        backing.resources.levels, backing.resources.layers,
        magic_enum::enum_name(backing.tile_mode), magic_enum::enum_name(backing.array_mode),
        magic_enum::enum_name(backing_micro), AmdGpu::GetMicroTileThickness(backing.array_mode),
        vk::to_string(backing.pixel_format), vk::to_string(desc.view_info.format),
        desc.view_info.range.base.level, desc.view_info.range.extent.levels,
        desc.view_info.range.base.layer, desc.view_info.range.extent.layers,
        desc.view_info.is_storage, image.usage.texture, image.usage.storage,
        image.usage.render_target, image.usage.depth_target, image.usage.vo_surface,
        image.binding.is_bound, image.binding.is_target, vk::to_string(image.usage_flags));

    static std::mutex records_mutex;
    static std::unordered_set<std::string> records;
    {
        std::scoped_lock lock{records_mutex};
        if (records.size() >= MaxViewDimensionMismatchRecords || !records.emplace(record).second) {
            return;
        }
    }
    LOG_ERROR(Render_Vulkan, "{}", record);
}

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           AmdGpu::Liverpool* liverpool_, BufferCache& buffer_cache_,
                           PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {
    // Create basic null image at fixed image ID.
    const auto null_id = GetNullImage(vk::Format::eR8G8B8A8Unorm);
    ASSERT(null_id.index == NULL_IMAGE_ID.index);

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
#ifdef ENABLE_BACHATA_RUNTIME
    // GPU memory is system memory here and the budget is small: start dropping images that have
    // not been used for a while long before the desktop thresholds, which exceed the budget.
    pressure_gc_memory = static_cast<u64>(device_local_memory * 4 / 10);
    critical_gc_memory = static_cast<u64>(device_local_memory * 6 / 10);
    trigger_gc_memory = 0;
    return;
#endif
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

ImageId TextureCache::GetNullImage(const vk::Format format) {
    const auto existing_image = null_images.find(format);
    if (existing_image != null_images.end()) {
        return existing_image->second;
    }

    ImageInfo info{};
    info.pixel_format = format;
    info.type = AmdGpu::ImageType::Color2D;
    info.tile_mode = AmdGpu::TileMode::Thin1DThin;
    info.num_bits = 32;
    info.UpdateSize();

    const ImageId null_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    auto& image = slot_images[null_id];
    Vulkan::SetObjectName(instance.GetDevice(), image.GetImage(),
                          fmt::format("Null Image ({})", vk::to_string(format)));

    image.flags = ImageFlagBits::Empty;
    image.track_addr = image.info.guest_address;
    image.track_addr_end = image.info.guest_address + image.info.guest_size;

    null_images.emplace(format, null_id);
    return null_id;
}

void TextureCache::ProcessDownloadImages() {
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    auto& download_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
    const u32 download_size = image.info.pitch * image.info.size.height *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto [download, offset] = download_buffer.Map(download_size);
    download_buffer.Commit();
    const vk::BufferImageCopy image_download = {
        .bufferOffset = offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, 1},
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                             download_buffer.Handle(), image_download);

    scheduler.DeferPriorityOperation(
        [this, device_addr = image.info.guest_address, download, download_size] {
            Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr), download,
                                                      download_size);
        });
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        const auto new_image_id =
            slot_images.insert(instance, scheduler, blit_helper, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Inserting may have moved the slot storage: look the old image up again.
        auto& old_image = slot_images[cache_image_id];

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = old_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;

        if (old_image.info.num_samples == 1 && new_info.num_samples == 1) {
            // Perform depth<->color copy using the intermediate copy buffer.
            if (instance.IsMaintenance8Supported()) {
                new_image.CopyImage(old_image);
            } else {
                const auto& copy_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
                new_image.CopyImageWithBuffer(old_image, copy_buffer.Handle(), 0);
            }
        } else if (old_image.info.num_samples == 1 && new_info.props.is_depth &&
                   new_info.num_samples > 1) {
            // Perform a rendering pass to transfer the channels of source as samples in dest.
            old_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::AccessFlagBits2::eShaderRead, {});
            new_image.Transit(vk::ImageLayout::eDepthAttachmentOptimal,
                              vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {});
            blit_helper.ReinterpretColorAsMsDepth(
                new_info.size.width, new_info.size.height,
                new_image.backing->image.image_ci.samples, old_image.info.pixel_format,
                new_info.pixel_format, old_image.GetImage(), new_image.GetImage());
        } else {
            LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy");
        }

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

ImageId TextureCache::ResolveDimensionOverlap(const ImageInfo& requested_info,
                                              ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    if (!NeedsViewTypeRecreation(requested_info.type, cache_image.info.type)) {
        return {};
    }

    const auto& cached_info = cache_image.info;
    const bool is_supported_single_row =
        requested_info.size.width == cached_info.size.width && requested_info.size.height == 1 &&
        cached_info.size.height == 1 && requested_info.size.depth == 1 &&
        cached_info.size.depth == 1 && requested_info.resources.levels == 1 &&
        cached_info.resources.levels == 1 && requested_info.resources.layers == 1 &&
        cached_info.resources.layers == 1 && requested_info.num_samples == 1 &&
        cached_info.num_samples == 1 && requested_info.pixel_format == cached_info.pixel_format &&
        requested_info.tile_mode == cached_info.tile_mode;
    if (!is_supported_single_row) {
        LOG_ERROR(Render_Vulkan,
                  "Unsupported image dimension recreation: requested={} backing={} "
                  "requested_extent={}x{}x{} backing_extent={}x{}x{}",
                  magic_enum::enum_name(requested_info.type),
                  magic_enum::enum_name(cached_info.type), requested_info.size.width,
                  requested_info.size.height, requested_info.size.depth, cached_info.size.width,
                  cached_info.size.height, cached_info.size.depth);
        return {};
    }

    const auto& copy_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
    const u64 copy_bytes = static_cast<u64>(cached_info.size.width) * cached_info.size.height *
                           cached_info.size.depth * cached_info.num_bits / 8;
    if (copy_bytes > copy_buffer.SizeBytes()) {
        LOG_ERROR(Render_Vulkan, "Image dimension recreation needs {} bytes, buffer has {}",
                  copy_bytes, copy_buffer.SizeBytes());
        return {};
    }

    const auto& new_info = requested_info;
    const auto new_image_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, new_info);
    RegisterImage(new_image_id);

    auto& new_image = slot_images[new_image_id];
    auto& source_image = slot_images[cache_image_id];
    new_image.usage = source_image.usage;

    // Fold any pending CPU or buffer-cache changes into the old backing before moving its
    // authoritative contents through a dimension-neutral buffer copy.
    RefreshImage(source_image);
    new_image.CopyImageWithBuffer(source_image, copy_buffer.Handle(), 0);
    new_image.flags &= ~ImageFlagBits::Dirty;
    new_image.flags |= source_image.flags & ImageFlagBits::GpuModified;

    if (source_image.binding.is_bound || source_image.binding.is_target) {
        source_image.binding.needs_rebind = 1u;
    }
    FreeImage(cache_image_id);
    return new_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        if (const auto dimension_image_id =
                ResolveDimensionOverlap(image_info, cache_image_id)) {
            return {dimension_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_format_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            const bool is_view_compatible =
                IsViewTypeCompatible(image_info.type, result_image.info.type);
            return {is_format_compatible && is_view_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    merged_image.CopyMip(cache_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    new_image.CopyImage(src_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);

    TrackImage(new_image_id);
    new_image.flags &= ~ImageFlagBits::Dirty;
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;

    if (info.guest_address == 0) [[unlikely]] {
        return GetNullImage(info.pixel_format);
    }

    std::scoped_lock lock{mutex};
    Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageLookups);

    auto& found = found_images[((info.guest_address >> 6) ^ (info.guest_address >> 17) ^
                                static_cast<u64>(info.pixel_format)) %
                               found_images.size()];
    if (found.registrations == registrations && found.guest_address == info.guest_address &&
        found.guest_size == info.guest_size && found.size == info.size &&
        found.resources == info.resources && found.pixel_format == info.pixel_format &&
        found.type == info.type && found.exact_fmt == exact_fmt) {
        // The image may have changed since; what made it the answer is checked again.
        Image& image = slot_images[found.image_id];
        if (image.info.guest_address == info.guest_address &&
            image.info.guest_size == info.guest_size && image.info.size == info.size &&
            IsVulkanFormatCompatible(image.info.pixel_format, info.pixel_format) &&
            IsViewTypeCompatible(info.type, image.info.type) &&
            (!exact_fmt || info.pixel_format == image.info.pixel_format) &&
            !(image.info.resources < info.resources)) {
            image.tick_accessed_last = scheduler.CurrentTick();
            TouchImage(image);
            Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageLookupsShort);
            return found.image_id;
        }
    }

    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            !IsViewTypeCompatible(info.type, cache_image.info.type)) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    const ImageId perfect_match = image_id;

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    if (TexAudit::Enabled() && image_id && image_id != perfect_match &&
        TexAudit::alias_logged < 400) {
        const Image& resolved = slot_images[image_id];
        const auto key = std::make_tuple(u64{info.guest_address}, u64{info.guest_size},
                                         u64{resolved.info.guest_address},
                                         u64{resolved.info.guest_size});
        if (TexAudit::aliases.insert(key).second) {
            ++TexAudit::alias_logged;
            LOG_WARNING(Render_Vulkan,
                        "TEXAUDIT: ALIAS asked for {} and got {} (view mip {} slice {}) flags {:#x}",
                        TexAudit::Describe(info), TexAudit::Describe(resolved.info), view_mip,
                        view_slice, static_cast<u32>(resolved.flags));
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    if (perfect_match && image_id == perfect_match) {
        found = {
            .guest_address = info.guest_address,
            .guest_size = info.guest_size,
            .size = info.size,
            .resources = info.resources,
            .pixel_format = info.pixel_format,
            .type = info.type,
            .exact_fmt = exact_fmt,
            .image_id = image_id,
            .registrations = registrations,
        };
    }

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    LogViewDimensionMismatch(desc, image);
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && !image.info.props.is_tiled && image.info.guest_address != 0) {
            download_images.emplace(image_id);
        }
    }
    UpdateImage(image_id);
    if (TexAudit::Enabled() && desc.type != BindingType::Storage) {
        const auto beat_now = std::chrono::steady_clock::now();
        if (beat_now - TexAudit::last_heartbeat >= std::chrono::seconds{2}) {
            TexAudit::last_heartbeat = beat_now;
            const u64 noisy = TexAudit::noise_state.size();
            const u64 empty = TexAudit::zero_state.size();
            LOG_WARNING(Render_Vulkan,
                        "TEXAUDIT: t={:.0f}s heartbeat: {} images currently flagged noisy, {} "
                        "flagged empty",
                        TexAudit::Seconds(), noisy, empty);
            // What is every game thread doing (blocked in which wait, or running where)?
            static u32 thread_dumps = 0;
            static double next_dump = 15.0;
            if (thread_dumps < 8 && TexAudit::Seconds() >= next_dump) {
                ++thread_dumps;
                next_dump = TexAudit::Seconds() + 8.0;
                LOG_WARNING(Render_Vulkan, "TEXAUDIT: GAME THREADS t={:.0f}s #{}:\n{}",
                            TexAudit::Seconds(), thread_dumps,
                            Core::GuestCpu::GuestWaitTable::Instance().Dump());
            }
        }
        if (!TexAudit::IsTarget(image) && image.info.num_samples == 1) {
            const auto nkey = std::make_pair(u64{image.info.guest_address},
                                             u64{image.info.guest_size});
            if (TexAudit::noise_state.count(nkey) != 0 &&
                TexAudit::noise_sampled_logged < 200) {
                const u32 nbase = std::min<u32>(desc.view_info.range.base.level, 15);
                const u32 ncount = std::min<u32>(desc.view_info.range.extent.levels, 16);
                const auto skey = std::make_tuple(nkey.first, nkey.second, nbase, ncount);
                if (TexAudit::noise_sampled_keys.insert(skey).second) {
                    ++TexAudit::noise_sampled_logged;
                    LOG_WARNING(Render_Vulkan,
                                "TEXAUDIT: t={:.1f}s SAMPLED-NOISE a draw samples mips {}..{} of a "
                                "noisy texture, mips {}: {}",
                                TexAudit::Seconds(), nbase, nbase + ncount - 1,
                                TexAudit::NoiseState(image.info),
                                TexAudit::Describe(image.info));
                }
            }
        }
        // Which mip levels does this draw sample, and are they empty?
        const auto& info = image.info;
        if (info.num_samples == 1 && !TexAudit::IsTarget(image) &&
            False(image.flags & ImageFlagBits::GpuModified) && info.guest_size >= 256 &&
            info.resources.levels <= 16) {
            const u32 base = std::min<u32>(desc.view_info.range.base.level, 15);
            const u32 count = std::min<u32>(desc.view_info.range.extent.levels,
                                            info.resources.levels > base ? info.resources.levels - base : 1);
            const auto key = std::make_tuple(u64{info.guest_address}, u64{info.guest_size}, base, count);
            const auto now = std::chrono::steady_clock::now();
            auto& seen = TexAudit::sampled_seen[key];
            if (now - seen >= std::chrono::seconds{2}) {
                seen = now;
                u32 zero_mips = 0;
                for (u32 m = base; m < base + count && m < 16; ++m) {
                    const auto& layout = info.mips_layout[m];
                    zero_mips += TexAudit::RangeIsZero(info.guest_address + layout.offset, layout.size);
                }
                if (TexAudit::RangeIsZero(info.guest_address + info.mips_layout[base].offset,
                                          info.mips_layout[base].size)) {
                    TexAudit::mip0_empty.insert(info.guest_address);
                } else {
                    TexAudit::mip0_empty.erase(info.guest_address);
                }
                if (zero_mips != 0 && TexAudit::sampled_logged_keys.insert(key).second) {
                    const bool all = zero_mips == count;
                    u32& counter = all ? TexAudit::sampled_logged : TexAudit::partial_logged;
                    if (counter < 300) {
                        ++counter;
                        LOG_WARNING(Render_Vulkan,
                                    "TEXAUDIT: {} view samples mips {}..{} of {}, mips {}: {}",
                                    all ? "SAMPLED-EMPTY" : "SAMPLED-PARTIAL", base,
                                    base + count - 1, info.resources.levels,
                                    TexAudit::MipState(info), TexAudit::Describe(info));
                    }
                }
            }
        }
    }
    if (TexAudit::FallbackEnabled() && desc.type != BindingType::Storage &&
        !TexAudit::fallback_base.empty()) {
        const auto it = TexAudit::fallback_base.find(
            std::make_pair(u64{image.info.guest_address}, u64{image.info.guest_size}));
        if (it != TexAudit::fallback_base.end()) {
            auto view_info = desc.view_info;
            const u32 first = view_info.range.base.level;
            const u32 count = view_info.range.extent.levels;
            const u32 end = first + count; // one past the last level the draw wants
            const u32 base = std::max<u32>(first, it->second);
            if (base < end && base < image.info.resources.levels) {
                view_info.range.base.level = base;
                view_info.range.extent.levels = end - base;
                return image.FindView(view_info);
            }
        }
    }
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && !image.info.props.is_tiled) {
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id =
                slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        // `image` may have moved if the stencil image was just inserted.
        stencil_image.AssociateDepth(image_id, slot_images[image_id].image_uid);
    }

    // The description says whether depth is written, which makes no difference to the view (a
    // depth buffer is never a storage image). Draws with and without depth writes get the same
    // one, and with it the same render pass.
    auto view_info = desc.view_info;
    view_info.is_storage = true;
    return slot_images[image_id].FindView(view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (ContentLod::Enabled() && image.info.num_samples == 1 &&
        False(image.flags & ImageFlagBits::GpuModified) && True(image.flags & ImageFlagBits::Dirty) &&
        image.info.guest_size >= 0x1000 && image.info.resources.levels >= 3) {
        ContentLod::Update(image.info);
    }
    if (TexAudit::Enabled() && image.info.num_samples == 1 &&
        False(image.flags & ImageFlagBits::GpuModified) && image.info.guest_size >= 256 &&
        image.info.guest_size <= (u64{64} << 20)) {
        const auto key = std::make_pair(u64{image.info.guest_address}, u64{image.info.guest_size});
        const auto now = std::chrono::steady_clock::now();
        if (True(image.flags & ImageFlagBits::Dirty)) {
            // About to be uploaded: a new baseline is taken when it is clean again.
            TexAudit::records.erase(key);
            if (!TexAudit::IsTarget(image)) {
                const bool empty = TexAudit::StartsZero(image);
                const auto state_it = TexAudit::zero_state.find(key);
                if (empty) {
                    if (state_it == TexAudit::zero_state.end()) {
                        TexAudit::zero_state[key] = {now, now, 0, false};
                        if (TexAudit::zero_logged < 300) {
                            ++TexAudit::zero_logged;
                            LOG_WARNING(Render_Vulkan,
                                        "TEXAUDIT: ZERO uploaded from memory that is zero at the "
                                        "start: {} mips {}",
                                        TexAudit::Describe(image.info),
                                        TexAudit::MipState(image.info));
                        }
                    }
                } else if (state_it != TexAudit::zero_state.end()) {
                    if (TexAudit::heal_logged < 300) {
                        ++TexAudit::heal_logged;
                        LOG_WARNING(Render_Vulkan,
                                    "TEXAUDIT: HEALED data arrived {:.1f} s after an empty upload "
                                    "({} uses while empty), cache was told: {} mips {}",
                                    std::chrono::duration<double>(now - state_it->second.first)
                                        .count(),
                                    state_it->second.uses, TexAudit::Describe(image.info),
                                    TexAudit::MipState(image.info));
                    }
                    TexAudit::zero_state.erase(state_it);
                }
                // Noise check of the compressed data that is about to be uploaded.
                const std::string noise = TexAudit::NoiseState(image.info);
                const bool noisy = noise.find('N') != std::string::npos;
                const auto noise_it = TexAudit::noise_state.find(key);
                if (noisy) {
                    if (noise_it == TexAudit::noise_state.end()) {
                        TexAudit::noise_state[key] = {now, now, 0, false};
                    }
                    TexAudit::noise_state[key].mips = noise;
                    if (TexAudit::noise_logged < 150) {
                        ++TexAudit::noise_logged;
                        TexAudit::DumpTexture(true, image.info);
                        // Trace the first NOISY mip (mip 0 is often just unknown '?').
                        const size_t nmip = std::min<size_t>(noise.find('N'),
                                                             image.info.mips_layout.size() - 1);
                        const u64 naddr = image.info.guest_address + image.info.mips_layout[nmip].offset;
                        const u64 nsize = image.info.mips_layout[nmip].size;
                        const auto chain = TexAudit::ChainOf(naddr);
                        const std::string chain_text =
                            chain.text + TexAudit::FileCheck(chain, nsize) +
                            TexAudit::CoverageOf(naddr, naddr + nsize) +
                            TexAudit::LayoutOf(naddr, nsize) +
                            TexAudit::ShadowText(image.info.guest_address,
                                                 image.info.guest_size);
                        LOG_WARNING(Render_Vulkan,
                                    "TEXAUDIT: t={:.1f}s NOISE uploaded{}: mips {}: {} first noisy mip {} at "
                                    "{:#x}+{:#x}, last writers:{} CHAIN of its first bytes:{}",
                                    TexAudit::Seconds(),
                                    noise_it != TexAudit::noise_state.end() ? " AGAIN" : "", noise,
                                    TexAudit::Describe(image.info), nmip, naddr, nsize,
                                    TexAudit::WritersOf(naddr, naddr + nsize), chain_text);
                    }
                } else if (noise_it != TexAudit::noise_state.end()) {
                    LOG_WARNING(Render_Vulkan,
                                "TEXAUDIT: t={:.1f}s NOISE-HEALED {:.1f} s after the first noisy "
                                "upload ({} uses), re-uploaded clean: mips {}: {}",
                                TexAudit::Seconds(),
                                std::chrono::duration<double>(now - noise_it->second.first).count(),
                                noise_it->second.uses, noise, TexAudit::Describe(image.info));
                    TexAudit::noise_state.erase(noise_it);
                } else if (TexAudit::clean_chain_logged < 40 && !noise.empty() && noise[0] == 'C' &&
                           image.info.pixel_format != vk::Format::eUndefined) {
                    // Control: where does the data of a picture that is fine come from?
                    ++TexAudit::clean_chain_logged;
                    TexAudit::DumpTexture(false, image.info);
                    const auto chain = TexAudit::ChainOf(image.info.guest_address +
                                                         image.info.mips_layout[0].offset);
                    LOG_WARNING(Render_Vulkan,
                                "TEXAUDIT: t={:.1f}s CLEAN uploaded: mips {}: {} CHAIN of mip 0's "
                                "first bytes:{}{}",
                                TexAudit::Seconds(), noise, TexAudit::Describe(image.info),
                                chain.text,
                                TexAudit::FileCheck(chain, image.info.mips_layout[0].size) +
                                    TexAudit::CoverageOf(image.info.guest_address +
                                                             image.info.mips_layout[0].offset,
                                                         image.info.guest_address +
                                                             image.info.mips_layout[0].offset +
                                                             image.info.mips_layout[0].size) +
                                    TexAudit::LayoutOf(image.info.guest_address +
                                                           image.info.mips_layout[0].offset,
                                                       image.info.mips_layout[0].size) +
                                    TexAudit::ShadowText(image.info.guest_address,
                                                         image.info.guest_size));
                }
            }
        } else {
            auto it = TexAudit::records.find(key);
            if (it == TexAudit::records.end()) {
                TexAudit::records[key] = {TexAudit::HashOf(image), now, now};
            } else if (now - it->second.checked >= std::chrono::seconds{1}) {
                it->second.checked = now;
                ++TexAudit::checks_total;
                const u64 hash = TexAudit::HashOf(image);
                if (hash != it->second.hash) {
                    ++TexAudit::stale_total;
                    if (TexAudit::stale_logged < 400) {
                        ++TexAudit::stale_logged;
                        LOG_WARNING(Render_Vulkan,
                                    "TEXAUDIT: STALE memory changed {:.1f} s after the upload and "
                                    "the cache was not told ({} stale in {} checks): {} flags {:#x}",
                                    std::chrono::duration<double>(now - it->second.uploaded).count(),
                                    TexAudit::stale_total, TexAudit::checks_total,
                                    TexAudit::Describe(image.info),
                                    static_cast<u32>(image.flags));
                    }
                    it->second.hash = hash;
                    it->second.uploaded = now;
                }
            }
            const auto noise_it = TexAudit::noise_state.find(key);
            if (noise_it != TexAudit::noise_state.end()) {
                auto& ns = noise_it->second;
                ++ns.uses;
                if (now - ns.checked >= std::chrono::seconds{1}) {
                    ns.checked = now;
                    const double age = std::chrono::duration<double>(now - ns.first).count();
                    const std::string noise = TexAudit::NoiseState(image.info);
                    if (noise.find('N') == std::string::npos) {
                        LOG_WARNING(Render_Vulkan,
                                    "TEXAUDIT: NOISE-MISSED data became clean {:.1f} s after a noisy "
                                    "upload and the cache was NOT told ({} uses): mips {}: {}",
                                    age, ns.uses, noise, TexAudit::Describe(image.info));
                        TexAudit::noise_state.erase(noise_it);
                    } else if (age >= 3.0 && !ns.reported) {
                        ns.reported = true;
                        LOG_WARNING(Render_Vulkan,
                                    "TEXAUDIT: t={:.1f}s NOISE STILL there {:.1f} s after the "
                                    "upload, used {} times: mips {}: {}",
                                    TexAudit::Seconds(), age, ns.uses, noise,
                                    TexAudit::Describe(image.info));
                    }
                }
            }
            const auto zero_it = TexAudit::zero_state.find(key);
            if (zero_it != TexAudit::zero_state.end()) {
                auto& zs = zero_it->second;
                ++zs.uses;
                if (now - zs.checked >= std::chrono::seconds{1}) {
                    zs.checked = now;
                    const double age = std::chrono::duration<double>(now - zs.first).count();
                    if (!TexAudit::StartsZero(image)) {
                        if (TexAudit::missed_logged < 300) {
                            ++TexAudit::missed_logged;
                            LOG_WARNING(Render_Vulkan,
                                        "TEXAUDIT: MISSED data appeared {:.1f} s after an empty "
                                        "upload and the cache was NOT told ({} uses): {} mips {}",
                                        age, zs.uses, TexAudit::Describe(image.info),
                                        TexAudit::MipState(image.info));
                        }
                        TexAudit::zero_state.erase(zero_it);
                    } else if (age >= 3.0 && !zs.reported) {
                        zs.reported = true;
                        if (TexAudit::still_logged < 300) {
                            ++TexAudit::still_logged;
                            LOG_WARNING(Render_Vulkan,
                                        "TEXAUDIT: STILL EMPTY {:.1f} s after the upload, used {} "
                                        "times since: {}",
                                        age, zs.uses, TexAudit::Describe(image.info));
                        }
                    }
                }
            }
        }
    }
    if (TexAudit::FallbackEnabled() && True(image.flags & ImageFlagBits::Dirty) &&
        image.info.num_samples == 1 && !TexAudit::IsTarget(image) && image.info.props.is_block &&
        False(image.flags & ImageFlagBits::GpuModified)) {
        // Leading run of empty (Z) or random (N) mips, followed by a good one.
        const std::string state = TexAudit::NoiseState(image.info);
        const bool colour = image.info.pixel_format == vk::Format::eBc1RgbUnormBlock ||
                            image.info.pixel_format == vk::Format::eBc1RgbSrgbBlock ||
                            image.info.pixel_format == vk::Format::eBc1RgbaUnormBlock ||
                            image.info.pixel_format == vk::Format::eBc1RgbaSrgbBlock ||
                            image.info.pixel_format == vk::Format::eBc3UnormBlock ||
                            image.info.pixel_format == vk::Format::eBc3SrgbBlock;
        const bool normal_map = image.info.pixel_format == vk::Format::eBc4UnormBlock ||
                                image.info.pixel_format == vk::Format::eBc4SnormBlock ||
                                image.info.pixel_format == vk::Format::eBc5UnormBlock ||
                                image.info.pixel_format == vk::Format::eBc5SnormBlock;
        u32 run = 0;
        if (normal_map && TexAudit::FallbackMode() >= 2) {
            // Everything up to the last bad mip.
            for (u32 i = 0; i < state.size(); ++i) {
                if (state[i] == 'N' || state[i] == 'Z') {
                    run = i + 1;
                }
            }
        } else {
            while (run < state.size() && (state[run] == 'Z' || (colour && state[run] == 'N'))) {
                ++run;
            }
        }
        // Only when a good mip follows, and keep at least the last two levels.
        if (run >= state.size() || run + 2 > state.size() || state[run] == '.' ||
            state[run] == 'Z' || state[run] == 'N') {
            run = 0;
        }
        const auto key = std::make_pair(u64{image.info.guest_address}, u64{image.info.guest_size});
        const u32 previous = TexAudit::fallback_base.count(key) ? TexAudit::fallback_base[key] : 0;
        if (run != 0) {
            TexAudit::fallback_base[key] = run;
        } else {
            TexAudit::fallback_base.erase(key);
        }
        if (run != previous && TexAudit::fallback_logged < 300) {
            ++TexAudit::fallback_logged;
            LOG_WARNING(Render_Vulkan,
                        "TEXAUDIT: t={:.1f}s MIP-FALLBACK mips below {} are not used for now "
                        "(mips {}; was {}): {}",
                        TexAudit::Seconds(), run, state, previous, TexAudit::Describe(image.info));
        }
    }
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    if (auto barrier = in_buffer->GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                             vk::PipelineStageFlagBits2::eTransfer)) {
        scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier.value(),
        });
    }

    if (TexAudit::VerifyEnabled() && !TexAudit::IsTarget(image) && image.info.props.is_block &&
        image.info.guest_size >= 0x1000 && image.info.guest_size <= (u32{4} << 20) &&
        TexAudit::verify_done < 800) {
        // SHADPS4_DBG_TEX_VERIFY: read back what is about to be uploaded (the buffer the GPU
        // copies from) and compare it with the guest memory. A difference means a stale or
        // GPU-made copy in the buffer cache. Slow: waits for the GPU on every check.
        ++TexAudit::verify_done;
        auto& download = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
        const u64 size = image.info.guest_size;
        const auto [dl_ptr, dl_offset] = download.Map(size);
        download.Commit();
        const vk::BufferCopy readback{.srcOffset = in_offset, .dstOffset = dl_offset, .size = size};
        const auto cmd = scheduler.CommandBuffer();
        cmd.copyBuffer(in_buffer->Handle(), download.Handle(), readback);
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &to_host,
        });
        scheduler.Finish();
        const auto* guest = std::bit_cast<const u8*>(image.info.guest_address);
        u64 pages = 0, differ = 0, src_zero = 0, guest_zero = 0, first_diff = ~u64{0};
        for (u64 page = 0; page * 4096 < size; ++page) {
            const u64 len = std::min<u64>(4096, size - page * 4096);
            ++pages;
            const bool same = std::memcmp(dl_ptr + page * 4096, guest + page * 4096, len) == 0;
            if (!same) {
                ++differ;
                first_diff = std::min(first_diff, page * 4096);
            }
            const auto all_zero = [len](const u8* p) {
                for (u64 i = 0; i < len; ++i) {
                    if (p[i] != 0) {
                        return false;
                    }
                }
                return true;
            };
            src_zero += all_zero(dl_ptr + page * 4096);
            guest_zero += all_zero(guest + page * 4096);
        }
        if (differ != 0) {
            ++TexAudit::verify_bad;
        }
        if ((differ != 0 && TexAudit::verify_logged < 300) ||
            (TexAudit::verify_done % 50 == 0 && TexAudit::verify_logged < 300)) {
            ++TexAudit::verify_logged;
            LOG_WARNING(Render_Vulkan,
                        "TEXVERIFY: {} upload source vs guest memory: {} of {} pages differ (first "
                        "at +{:#x}), all-zero pages source {} guest {}, buffer gpu_modified {} "
                        "cpu_modified {} ({} bad of {} checked): {}",
                        differ != 0 ? "MISMATCH" : "ok", differ, pages,
                        differ != 0 ? first_diff : 0, src_zero, guest_zero,
                        buffer_cache.IsRegionGpuModified(image.info.guest_address,
                                                         image.info.guest_size),
                        buffer_cache.IsRegionCpuModified(image.info.guest_address,
                                                         image.info.guest_size),
                        TexAudit::verify_bad, TexAudit::verify_done,
                        TexAudit::Describe(image.info));
        }
    }

    const auto [buffer, offset] =
        tile_manager.DetileImage(in_buffer->Handle(), in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    if (image.info.guest_size >= 0x700000) {
        // Close the Mode A blind spot: stamp buffer_cache (or stream) source as detile-read.
        // Host dispatch log often has srcResource=0; guest track is authoritative for FHD src.
        buffer_cache.NoteDetileSourceUse(in_buffer->Handle(), in_offset, image.info.guest_size,
                                         "detile_src");
        if (StagingVerbose()) {
            LOG_WARNING(Render_Vulkan,
                        "IMAGE_UPLOAD_CHAIN size={:#x} srcBuffer={:#x} srcOffset={:#x} "
                        "detileDst={:#x} detileOffset={:#x} tick={} w={} h={} tiled={}",
                        image.info.guest_size, u64(VkBuffer(in_buffer->Handle())), in_offset,
                        u64(VkBuffer(buffer)), offset, scheduler.CurrentTick(),
                        image.info.size.width, image.info.size.height,
                        image.info.props.is_tiled ? 1 : 0);
        }
    }

    image.Upload(image_copies, buffer, offset);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base) {
    const u64 hash = XXH3_64bits(&sampler, sizeof(sampler));
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base);
    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registrations;
    Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageRegistrations);
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registrations;
    Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageRegistrations);
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            return false;
        }
        if (download && !pressured) {
            return false;
        }
        if (download) {
            DownloadImageMemory(image_id);
        }
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::TouchImage(const Image& image) {
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

} // namespace VideoCore
