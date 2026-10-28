// MKSM-Port: overrides for Mortal Kombat: Shaolin Monks (SLES-53524).
// Only game-independent setup for now; game functions are replaced as the first runs show which ones need it.
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_host_aspect.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "mksm_gs_replay.h" // MKSM-Port: optional, bounded capture to compare backends.
#include "ps2_stubs.h"
#include <ps2_recompiled_functions.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

extern std::atomic<uint64_t> g_mksmPresentedFrames; // gs_host_present.cpp: frames the host window has shown
extern std::atomic<uint64_t> g_mksmAudioFramesPlayed;   // ps2_runtime.cpp: frames the audio device asked for
extern std::atomic<uint64_t> g_mksmAudioFramesProduced; // ps2_runtime.cpp: frames the sound engine had for it
void mksmDumpBranchRing(const char *why);            // ps2_runtime.cpp: the last 256 inter-function branches (JAL included)
namespace ps2_stubs                                  // Stubs/Pad.h: the pad state scePadRead returns while it is set
{
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();
}

namespace
{
    // MKSM-Port: the game reads its data from the files extracted next to the ELF (tools/iso_extract.py --layout:
    // .disc_layout + .disc_meta), each at its original LSN. MKSM_ISO=<iso> selects the original image instead.
    void configureMksmCdImage()
    {
        namespace fs = std::filesystem;
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        if (!paths.cdImage.empty())
            return;
        std::error_code ec;
        fs::path image;
#ifdef _WIN32
        // Windows environment variables are UTF-16; getenv's ANSI bytes are not UTF-8.
        if (const wchar_t *env = _wgetenv(L"MKSM_ISO"); env != nullptr && env[0] != L'\0')
            image = fs::path(env);
#else
        if (const char *env = std::getenv("MKSM_ISO"); env != nullptr && env[0] != '\0')
            image = fs::u8path(env);
#endif
        if (image.empty())
        {
            const fs::path root = paths.cdRoot.empty() ? paths.elfDirectory : paths.cdRoot;
            if (!fs::is_regular_file(root / ".disc_layout", ec))
                std::fprintf(stderr, "[mksm-cd] no .disc_layout in %s: extract the disc with tools/iso_extract.py --layout\n",
                             reinterpret_cast<const char *>(root.u8string().c_str()));
            return;
        }
        if (!fs::is_regular_file(image, ec))
        {
            std::fprintf(stderr, "[mksm-cd] MKSM_ISO is not a file; using the extracted files\n");
            return;
        }
        paths.cdImage = image;
        PS2Runtime::setIoPaths(paths);
        const auto imageUtf8 = image.u8string();
        std::fprintf(stderr, "[mksm-cd] disc image (MKSM_ISO): %s\n", reinterpret_cast<const char *>(imageUtf8.c_str()));
    }

    // Return-address candidates on a guest stack, from sp up: words inside .text that point just after a JAL/JALR
    // (ra = call + 8). A cheap backtrace for a thread that waits inside a stub, whose ra is the stub's caller only.
    std::string stackCallers(const uint8_t *ram, uint32_t sp)
    {
        constexpr uint32_t kTextStart = 0x00100000u, kTextEnd = 0x0049A9C8u; // SLES_535.24 .text
        std::string out;
        int found = 0;
        for (uint32_t offset = 0; offset < 0x800u && found < 8; offset += 4u)
        {
            const uint32_t at = (sp + offset) & 0x01FFFFFCu;
            uint32_t value = 0, call = 0;
            std::memcpy(&value, ram + at, sizeof(value));
            if (value < kTextStart + 8u || value >= kTextEnd || (value & 3u) != 0u)
                continue;
            std::memcpy(&call, ram + value - 8u, sizeof(call));
            if ((call >> 26) != 3u && !((call >> 26) == 0u && (call & 0x3Fu) == 9u))
                continue;
            char item[16];
            std::snprintf(item, sizeof(item), " %08x", value);
            out += item;
            ++found;
        }
        return out;
    }

    // MKSM-Port: MKSM_THREAD_WATCH=<s> (debug): every <s> seconds a host thread prints the EE threads (status, wait,
    // pc, ra, return addresses found on the stack) from the scheduler's snapshot, so a guest that blocks for good still
    // shows where each thread waits. At the third tick it also dumps the last 256 inter-function branches.
    void startThreadWatch(PS2Runtime &runtime)
    {
        const char *watch = std::getenv("MKSM_THREAD_WATCH");
        if (!watch || std::atof(watch) <= 0.0)
            return;
        const double period = std::max(1.0, std::atof(watch));
        // MKSM_THREAD_WATCH_RING=<tick>: the tick that dumps the branch ring (default 3)
        const unsigned ringTick = std::getenv("MKSM_THREAD_WATCH_RING") ? std::atoi(std::getenv("MKSM_THREAD_WATCH_RING")) : 3u;
        std::thread([&runtime, period, ringTick] {
            uint64_t lastPlayed = 0, lastProduced = 0;
            for (unsigned tick = 1;; ++tick)
            {
                std::this_thread::sleep_for(std::chrono::duration<double>(period));
                const EeKernelSnapshot snapshot = runtime.eeScheduler().snapshot();
                std::fprintf(stderr, "[mksm-threads] watch running=%d cycle=%llu\n", snapshot.runningThreadId,
                             static_cast<unsigned long long>(snapshot.eeCycle));
                for (const EeThreadSnapshot &t : snapshot.threads)
                    std::fprintf(stderr, "[mksm-threads]   id=%d prio=%d status=%d wait=%d/%d pc=%08x ra=%08x sp=%08x "
                                 "callers:%s\n", t.id, t.currentPriority, int(t.status), int(t.waitReason), t.waitId,
                                 t.pc, t.ra, t.sp, stackCallers(runtime.memory().getRDRAM(), t.sp).c_str());
                if (tick == ringTick)
                    mksmDumpBranchRing("thread watch");
                {
                    // audio the device asked for in this period and how much of it the sound engine had (the rest is
                    // the silence filled in when it ran short)
                    const uint64_t played = g_mksmAudioFramesPlayed.load(), produced = g_mksmAudioFramesProduced.load();
                    std::fprintf(stderr, "[mksm-audio] asked=%llu got=%llu\n", static_cast<unsigned long long>(played - lastPlayed),
                                 static_cast<unsigned long long>(produced - lastProduced));
                    lastPlayed = played;
                    lastProduced = produced;
                }
                {
                    // The GS work counters and the display registers the host presents from: PMODE with no read
                    // circuit on (or DISPLAY 0) is the blank, magenta frame. "presented" also counts blank frames.
                    PS2Memory &mem = runtime.memory();
                    const GSRegisters &gs = mem.gs();
                    std::fprintf(stderr,
                                 "[mksm-gs] dma=%llu gif=%llu gswrites=%llu vifwrites=%llu presented=%llu pmode=%llx "
                                 "smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx bgcolor=%llx\n",
                                 static_cast<unsigned long long>(mem.dmaStartCount()),
                                 static_cast<unsigned long long>(mem.gifCopyCount()),
                                 static_cast<unsigned long long>(mem.gsWriteCount()),
                                 static_cast<unsigned long long>(mem.vifWriteCount()),
                                 static_cast<unsigned long long>(g_mksmPresentedFrames.load()),
                                 static_cast<unsigned long long>(gs.pmode), static_cast<unsigned long long>(gs.smode2),
                                 static_cast<unsigned long long>(gs.dispfb1), static_cast<unsigned long long>(gs.display1),
                                 static_cast<unsigned long long>(gs.dispfb2), static_cast<unsigned long long>(gs.display2),
                                 static_cast<unsigned long long>(gs.bgcolor));
                }
                // MKSM_WATCH_WORDS=<addr>:<count>,... also prints those guest RAM words at every tick.
                if (const char *spec = std::getenv("MKSM_WATCH_WORDS"))
                {
                    const uint8_t *ram = runtime.memory().getRDRAM();
                    for (const char *p = spec; *p;)
                    {
                        char *end = nullptr;
                        const uint32_t address = static_cast<uint32_t>(std::strtoul(p, &end, 16)) & 0x01FFFFFCu;
                        uint32_t count = 1u;
                        if (end && *end == ':')
                            count = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
                        std::string line;
                        for (uint32_t i = 0; i < count && i < 64u; ++i)
                        {
                            uint32_t value;
                            std::memcpy(&value, ram + address + i * 4u, sizeof(value));
                            char item[16];
                            std::snprintf(item, sizeof(item), " %08x", value);
                            line += item;
                        }
                        std::fprintf(stderr, "[mksm-words] %08x:%s\n", address, line.c_str());
                        p = (end && *end == ',') ? end + 1 : (end ? end : p + std::strlen(p));
                        if (end && *end == '\0')
                            break;
                    }
                }
            }
        }).detach();
    }

    // MKSM-Port: the movies are CRI Sofdec streams that the game decodes through the IPU, which the runtime does not
    // emulate (its MPEG support replaces Sony's libmpeg, not the IPU itself): a movie never reports its end and the
    // game would wait in the frame loop of 0x270038 (play the movie at a0) forever. Until movies are supported, the
    // call returns at once as if the movie had finished (4, what 0x32ECE0 expects at the end of a movie);
    // MKSM_MOVIES=1 plays them (to work on movie support).
    void mksmPlayMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool play = [] { const char *v = std::getenv("MKSM_MOVIES"); return v && v[0] == '1'; }();
        if (play || ctx->pc != 0x00270038u)
        {
            sub_00270038_0x270038(rdram, ctx, runtime);
            return;
        }
        char name[64] = {};
        const uint32_t address = GPR_U32(ctx, 4) & 0x01FFFFFFu;
        if (address + sizeof(name) <= PS2_RAM_SIZE)
            std::memcpy(name, rdram + address, sizeof(name) - 1u);
        std::fprintf(stderr, "[mksm-movie] skipped %s (MKSM_MOVIES=1 plays the movies)\n", name);
        SET_GPR_S32(ctx, 2, 4);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // MKSM-Port: libpkt. The config stubs five libpkt functions (the analyzer knows their names) and the rest runs as
    // recompiled game code. The runtime's handlers also rewrite the size of the open DMA tag (sceGifPacket +8) every
    // time they move the packet pointer, but the game's own terminate (0x384C58, called by the next cnt/end/ref) adds
    // the size when it closes the tag: the tag counts its quadwords twice (a cnt of 4 says 8), the DMAC sends the next
    // tags as GS data and the texture uploads after them never reach the GS (black frames with ~660 draws each).
    // These are the game's own five functions, which leave the DMA tag alone.
    void closePacketGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t gifTagField)
    {
        const uint32_t packet = GPR_U32(ctx, 4);
        const uint32_t tagAddress = READ32(packet + gifTagField);
        uint32_t current = READ32(packet);
        const uint64_t tag = READ64(tagAddress);
        // NLOOP: doublewords after the tag for REGLIST, quadwords for PACKED and IMAGE, in register sets but for IMAGE.
        uint32_t count = ((current - tagAddress) >> 3) - 2u;
        const uint32_t flg = static_cast<uint32_t>(tag >> 58) & 3u;
        if (flg != 1u)
            count >>= 1;
        if (flg != 2u)
        {
            const uint32_t nreg = (tag >> 60) != 0u ? static_cast<uint32_t>(tag >> 60) : 16u;
            count = (count + nreg - 1u) / nreg;
        }
        WRITE32(packet + gifTagField, 0u);
        WRITE64(tagAddress, tag + count);
        for (; (current & 0xCu) != 0u; current += 4u)
            WRITE32(current, 0u);
        WRITE32(packet, current);
        ctx->pc = GPR_U32(ctx, 31);
    }

    void mksmGifPkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x384F10
    {
        closePacketGifTag(rdram, ctx, runtime, 0x0Cu);
    }

    void mksmVif1PkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x385428
    {
        closePacketGifTag(rdram, ctx, runtime, 0x14u);
    }

    void mksmGifPkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x384FE0
    {
        const uint32_t packet = GPR_U32(ctx, 4), current = READ32(packet);
        WRITE64(current, GPR_U64(ctx, 6));
        WRITE64(current + 8u, static_cast<uint64_t>(GPR_U32(ctx, 5)));
        WRITE32(packet, current + 16u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    void mksmVif1PkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x385610
    {
        const uint32_t packet = GPR_U32(ctx, 4), current = READ32(packet);
        const uint64_t data = GPR_U64(ctx, 6);
        WRITE32(current, static_cast<uint32_t>(data));
        WRITE32(current + 4u, static_cast<uint32_t>(data >> 32));
        WRITE32(current + 8u, GPR_U32(ctx, 5));
        WRITE32(current + 12u, 0u);
        WRITE32(packet, current + 16u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // 0x385388: pads the packet up to its fourth word (sceVif1PkAlign(packet, 2, 3), 0x3854D0) and opens a DIRECT
    // VIFcode there, with the interrupt bit when a1 is not zero; sceVif1PkCloseDirectCode fills in its size.
    void mksmVif1PkOpenDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t packet = GPR_U32(ctx, 4);
        uint32_t current = READ32(packet);
        uint32_t target = (current & ~0xFu) + 12u;
        if (target < current)
            target += 16u;
        for (; current < target; current += 4u)
            WRITE32(current, 0u);
        WRITE32(current, GPR_U64(ctx, 5) != 0u ? 0xD0000000u : 0x50000000u);
        WRITE32(packet + 0x0Cu, current);
        WRITE32(packet, current + 4u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // MKSM-Port: MKSM_CD_READ_TRACE=<n> (debug): the first <n> calls of sceCdRead (0x467BF0; CRI's SRD device reads the
    // disc with it): sector, count, destination, caller, result and how many of the bytes read are not zero.
    void mksmTraceCdRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const long limit = [] { const char *v = std::getenv("MKSM_CD_READ_TRACE"); return v ? std::atol(v) : 0L; }();
        static long traced = 0;
        const uint32_t lbn = GPR_U32(ctx, 4), sectors = GPR_U32(ctx, 5), dst = GPR_U32(ctx, 6) & 0x01FFFFFFu;
        const uint32_t ra = GPR_U32(ctx, 31);
        ctx->pc = ra;
        ps2_stubs::sceCdRead(rdram, ctx, runtime);
        if (traced >= limit)
            return;
        ++traced;
        const size_t bytes = std::min<size_t>(static_cast<size_t>(sectors) * 2048u, PS2_RAM_SIZE - dst);
        size_t nonzero = 0;
        for (size_t i = 0; i < bytes; ++i)
            nonzero += rdram[dst + i] != 0u;
        std::fprintf(stderr, "[mksm-cdread] lbn=%x sectors=%u dst=%08x ra=%08x ret=%d nonzero=%zu/%zu\n", lbn, sectors,
                     dst, ra, static_cast<int>(GPR_U32(ctx, 2)), nonzero, bytes);
    }

    // MKSM-Port: MKSM_PAD_SCRIPT=<s>:<button>[+<button>...],... (testing): scePadRead (0x206C48) also holds those
    // buttons for a quarter of a second, <s> seconds after start, so a timed run can get past the boot prompts. Buttons:
    // start, select, up, down, left, right, cross, circle, square, triangle, l1, r1, l2, r2.
    struct PadPress
    {
        double at;
        uint16_t mask;
    };
    std::chrono::steady_clock::time_point g_mksmStart = std::chrono::steady_clock::now();

    std::vector<PadPress> parsePadScript(const std::string &spec)
    {
        static const struct
        {
            const char *name;
            uint16_t mask;
        } kButtons[] = {{"select", 0x0001u}, {"start", 0x0008u}, {"up", 0x0010u}, {"right", 0x0020u},
                        {"down", 0x0040u}, {"left", 0x0080u}, {"l2", 0x0100u}, {"r2", 0x0200u}, {"l1", 0x0400u},
                        {"r1", 0x0800u}, {"triangle", 0x1000u}, {"circle", 0x2000u}, {"cross", 0x4000u},
                        {"square", 0x8000u}};
        std::vector<PadPress> presses;
        for (size_t pos = 0; pos < spec.size();)
        {
            const size_t comma = std::min(spec.find(',', pos), spec.size());
            const std::string item = spec.substr(pos, comma - pos);
            pos = comma + 1;
            const size_t colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            PadPress press{std::atof(item.c_str()), 0u};
            for (size_t b = colon + 1; b <= item.size();)
            {
                const size_t plus = std::min(item.find('+', b), item.size());
                const std::string name = item.substr(b, plus - b);
                for (const auto &button : kButtons)
                    if (name == button.name)
                        press.mask |= button.mask;
                b = plus + 1;
            }
            if (press.mask != 0u)
                presses.push_back(press);
        }
        return presses;
    }

    void mksmScriptedPadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const std::vector<PadPress> presses = parsePadScript(std::getenv("MKSM_PAD_SCRIPT"));
        static size_t announced = 0;
        static bool holding = false;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_mksmStart).count();
        uint16_t mask = 0u;
        for (size_t i = 0; i < presses.size(); ++i)
            if (now >= presses[i].at && now < presses[i].at + 0.25)
            {
                mask |= presses[i].mask;
                if (i >= announced)
                {
                    announced = i + 1;
                    std::fprintf(stderr, "[mksm-pad] %.1f s: buttons %04x\n", now, presses[i].mask);
                }
            }
        // While a press is held the runtime's pad override replaces the pad; scePadRead returns it and also writes it
        // into the port's DMA buffer, where the game reads its buttons.
        if (mask != 0u)
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(0xFFFFu & ~mask), 0x80, 0x80, 0x80, 0x80);
        else if (holding)
            ps2_stubs::clearPadOverrideState();
        holding = mask != 0u;
        ctx->pc = GPR_U32(ctx, 31);
        ps2_stubs::scePadRead(rdram, ctx, runtime);
    }

    // MKSM-Port: MKSM_ASPECT=<w>:<h> wider than 4:3 (16:9, 21:9...) also widens the game's 3D view. The float at
    // 0x4F4E08 scales the projection horizontally by (4/3) / aspect: the game's own setter (0x2268B0) computes it so but
    // is never called, and PCSX2's 16:9 patch by nemesis2000 writes 0.75 there. The 2D screens and the HUD stretch with
    // the window, as with that patch.
    void applyMksmAspect(PS2Runtime &runtime)
    {
        const char *setting = std::getenv("MKSM_ASPECT");
        const float aspect = gsHostDisplayAspect(setting);
        uint8_t *rdram = runtime.memory().getRDRAM();
        float scale = 0.0f;
        std::memcpy(&scale, rdram + 0x004F4E08u, sizeof(scale));
        if (aspect <= 4.0f / 3.0f + 0.01f || scale != 1.0f)
            return;
        scale = (4.0f / 3.0f) / aspect;
        std::memcpy(rdram + 0x004F4E08u, &scale, sizeof(scale));
        std::fprintf(stderr, "[mksm-override] widescreen %s: 3D view scaled by %.4f\n", setting, scale);
    }

    void applyMksmOverrides(PS2Runtime &runtime)
    {
        startThreadWatch(runtime);
        configureMksmCdImage();
        mksm_gs_replay::configure(runtime.memory(), runtime.gs());
        // The runtime's internal heap goes below the ELF (free memory, the ELF loads at 0x100000) so as not to
        // trample the game's own heap at the end of .bss.
        runtime.setPrivateGuestHeap(0x000A0000u, 0x000FF000u);
        applyMksmAspect(runtime);
        // libcdvd's sceCdLayerSearchFile (0x385F90; sceCdSearchFile at 0x386290 calls it with layer 0) binds the IOP
        // CDVDFSV search server 0x80000597 and retries until it exists. The runtime answers libcdvd on the EE side, so
        // the native sceCdSearchFile handler takes it (same fp/name arguments; the disc has a single layer).
        const bool search = ps2_game_overrides::bindAddressHandler(runtime, 0x00385F90u, "sceCdSearchFile");
        const bool movie = runtime.replaceFunction(0x00270038u, mksmPlayMovie);
        const bool libpkt = runtime.replaceFunction(0x00384F10u, mksmGifPkCloseGifTag) &&
                            runtime.replaceFunction(0x00384FE0u, mksmGifPkAddGsAD) &&
                            runtime.replaceFunction(0x00385388u, mksmVif1PkOpenDirectCode) &&
                            runtime.replaceFunction(0x00385428u, mksmVif1PkCloseGifTag) &&
                            runtime.replaceFunction(0x00385610u, mksmVif1PkAddGsAD);
        if (std::getenv("MKSM_CD_READ_TRACE"))
            runtime.replaceFunction(0x00467BF0u, mksmTraceCdRead);
        if (std::getenv("MKSM_PAD_SCRIPT"))
            runtime.replaceFunction(0x00206C48u, mksmScriptedPadRead);
        std::fprintf(stderr, "[mksm-override] Mortal Kombat: Shaolin Monks (SLES-53524): sceCdLayerSearchFile=%d movie=%d "
                     "libpkt=%d\n", search, movie, libpkt);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Mortal Kombat: Shaolin Monks (SLES-53524)", "SLES_535.24", 0x0011C038u, 0u, applyMksmOverrides)
