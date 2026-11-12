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
//   MKSM_GS_DUMP_VRAM=<n>          also records the GS memory as the backend holds it (4 MB): before the first packet
//                                  of the range and then after every n packets (0, the default: only before the
//                                  first packet). Each one makes the backend finish its drawing and read the memory
//                                  back, so the game crawls while the range lasts.
//
// The packets are copied on their way from the GIF arbiter to the GS, in the order the GS gets them, with their path
// (1 XGKICK, 2 VIF1 DIRECT, 3 the GIF channel). The file: "MKGD", version 1, then records of tick (u32), path (u32),
// size in bytes (u32) and the bytes; a record with path 0 is the GS memory at that point. This replaces the arbiter's
// function, as MKSM_VU1=verify does: the two cannot be used together (the dump wins).
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
        uint64_t vramEvery = 0;
        uint64_t vramRecords = 0;
        bool started = false;
    };
    GsDump g_gsDump;

    void put32(std::FILE *file, uint32_t value)
    {
        const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                                  static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
        std::fwrite(bytes, 1u, sizeof(bytes), file);
    }

    // The GS memory as the backend holds it now (the backend finishes what it was given first).
    void putVram(PS2Runtime &runtime, uint64_t tick)
    {
        runtime.gs().refreshDisplaySnapshot();
        uint32_t size = 0;
        const uint8_t *vram = runtime.gs().lockDisplaySnapshot(size);
        if (vram && size)
        {
            put32(g_gsDump.file, static_cast<uint32_t>(tick));
            put32(g_gsDump.file, 0u);
            put32(g_gsDump.file, size);
            std::fwrite(vram, 1u, size, g_gsDump.file);
            ++g_gsDump.vramRecords;
        }
        runtime.gs().unlockDisplaySnapshot();
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
    if (const char *every = std::getenv("MKSM_GS_DUMP_VRAM"))
        g_gsDump.vramEvery = std::strtoull(every, nullptr, 10);
    g_gsDump.file = std::fopen(path, "wb");
    if (!g_gsDump.file)
    {
        std::fprintf(stderr, "[mksm-gsdump] cannot write %s\n", path);
        return;
    }
    std::fwrite("MKGD", 1u, 4u, g_gsDump.file);
    put32(g_gsDump.file, 1u);
    runtime.gifArbiter().setProcessPacketFn([&runtime](GifPathId gifPath, const uint8_t *data, uint32_t size) {
        bool recorded = false;
        uint64_t tick = 0;
        if (g_gsDump.file)
        {
            tick = runtime.memory().gs().vsyncTick.load(std::memory_order_relaxed);
            if (tick >= g_gsDump.first && tick <= g_gsDump.last)
            {
                if (!g_gsDump.started)
                {
                    g_gsDump.started = true;
                    putVram(runtime, tick);
                }
                put32(g_gsDump.file, static_cast<uint32_t>(tick));
                put32(g_gsDump.file, static_cast<uint32_t>(gifPath));
                put32(g_gsDump.file, size);
                std::fwrite(data, 1u, size, g_gsDump.file);
                ++g_gsDump.packets;
                g_gsDump.bytes += size;
                recorded = true;
            }
            else if (tick > g_gsDump.last)
            {
                std::fclose(g_gsDump.file);
                g_gsDump.file = nullptr;
                std::fprintf(stderr, "[mksm-gsdump] %llu packets, %llu bytes and %llu memory records written\n",
                             static_cast<unsigned long long>(g_gsDump.packets), static_cast<unsigned long long>(g_gsDump.bytes),
                             static_cast<unsigned long long>(g_gsDump.vramRecords));
            }
        }
        // Upstream's arbiter function (PS2Runtime::syncCoreSubsystems).
        runtime.gs().processGIFPacket(data, size, static_cast<uint32_t>(gifPath));
        if (recorded && g_gsDump.vramEvery != 0 && g_gsDump.packets % g_gsDump.vramEvery == 0)
            putVram(runtime, tick);
    });
    std::fprintf(stderr, "[mksm-gsdump] recording the GIF packets of vertical blanks %llu to %llu into %s\n",
                 static_cast<unsigned long long>(g_gsDump.first), static_cast<unsigned long long>(g_gsDump.last), path);
}
