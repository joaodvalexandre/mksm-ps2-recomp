// (The file name starts with "trace_" so that it sorts after the generated sub_*.cpp in the runner directory: a name
// that sorts earlier would shift every unity batch.)
//
// MKSM_GS_DUMP (diagnostic): records every GIF packet the GS receives during a range of vertical blanks, to look
// at what the game draws (tools/gs_dump.py lists the draw batches with their GS state).
//
//   MKSM_GS_DUMP=<file>            the file to write (it holds the game's textures and geometry: keep it outside
//                                  the repository)
//   MKSM_GS_DUMP_TICKS=<a>:<b>     the vertical blanks to record, counted from the start (60 a second); default
//                                  3600:3605
//
// The packets are copied on their way from the GIF arbiter to the GS, in the order the GS gets them, with their path
// (1 XGKICK, 2 VIF1 DIRECT, 3 the GIF channel). The file: "MKGD", version 1, then records of tick (u32), path (u32),
// size in bytes (u32) and the bytes. This replaces the arbiter's function, as MKSM_VU1=verify does: the two cannot be
// used together (the dump wins).
#include "ps2_runtime.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    struct GsDump
    {
        std::FILE *file = nullptr;
        uint64_t first = 3600;
        uint64_t last = 3605;
        uint64_t packets = 0;
        uint64_t bytes = 0;
    };
    GsDump g_gsDump;

    void put32(std::FILE *file, uint32_t value)
    {
        const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                                  static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
        std::fwrite(bytes, 1u, sizeof(bytes), file);
    }
}

// Called by applyMksmOverrides (mksm_overrides.cpp), after the VU1 glue.
void mksmInstallGsDump(PS2Runtime &runtime)
{
    const char *path = std::getenv("MKSM_GS_DUMP");
    if (!path || !*path)
        return;
    if (const char *ticks = std::getenv("MKSM_GS_DUMP_TICKS"))
    {
        unsigned long long first = 0, last = 0;
        if (std::sscanf(ticks, "%llu:%llu", &first, &last) == 2 && last >= first)
        {
            g_gsDump.first = first;
            g_gsDump.last = last;
        }
    }
    g_gsDump.file = std::fopen(path, "wb");
    if (!g_gsDump.file)
    {
        std::fprintf(stderr, "[mksm-gsdump] cannot write %s\n", path);
        return;
    }
    std::fwrite("MKGD", 1u, 4u, g_gsDump.file);
    put32(g_gsDump.file, 1u);
    runtime.gifArbiter().setProcessPacketFn([&runtime](GifPathId gifPath, const uint8_t *data, uint32_t size) {
        if (g_gsDump.file)
        {
            const uint64_t tick = runtime.memory().gs().vsyncTick.load(std::memory_order_relaxed);
            if (tick >= g_gsDump.first && tick <= g_gsDump.last)
            {
                put32(g_gsDump.file, static_cast<uint32_t>(tick));
                put32(g_gsDump.file, static_cast<uint32_t>(gifPath));
                put32(g_gsDump.file, size);
                std::fwrite(data, 1u, size, g_gsDump.file);
                ++g_gsDump.packets;
                g_gsDump.bytes += size;
            }
            else if (tick > g_gsDump.last)
            {
                std::fclose(g_gsDump.file);
                g_gsDump.file = nullptr;
                std::fprintf(stderr, "[mksm-gsdump] %llu packets, %llu bytes recorded\n",
                             static_cast<unsigned long long>(g_gsDump.packets), static_cast<unsigned long long>(g_gsDump.bytes));
            }
        }
        // Upstream's arbiter function (PS2Runtime::syncCoreSubsystems).
        runtime.gs().processGIFPacket(data, size, static_cast<uint32_t>(gifPath));
    });
    std::fprintf(stderr, "[mksm-gsdump] recording the GIF packets of vertical blanks %llu to %llu into %s\n",
                 static_cast<unsigned long long>(g_gsDump.first), static_cast<unsigned long long>(g_gsDump.last), path);
}
