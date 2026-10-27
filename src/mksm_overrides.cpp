// MKSM-Port: overrides for Mortal Kombat: Shaolin Monks (SLES-53524).
// Only game-independent setup for now; game functions are replaced as the first runs show which ones need it.
#include "runtime/ee_scheduler.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "mksm_gs_replay.h" // MKSM-Port: optional, bounded capture to compare backends.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

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

    // MKSM-Port: MKSM_THREAD_WATCH=<s> (debug): every <s> seconds a host thread prints the EE threads (status, wait,
    // pc, ra) from the scheduler's snapshot, so a guest that blocks for good still shows where each thread waits.
    void startThreadWatch(PS2Runtime &runtime)
    {
        const char *watch = std::getenv("MKSM_THREAD_WATCH");
        if (!watch)
            return;
        const double period = std::max(1.0, std::atof(watch));
        std::thread([&runtime, period] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::duration<double>(period));
                const EeKernelSnapshot snapshot = runtime.eeScheduler().snapshot();
                std::fprintf(stderr, "[mksm-threads] watch running=%d cycle=%llu\n", snapshot.runningThreadId,
                             static_cast<unsigned long long>(snapshot.eeCycle));
                for (const EeThreadSnapshot &t : snapshot.threads)
                    std::fprintf(stderr, "[mksm-threads]   id=%d prio=%d status=%d wait=%d/%d pc=%08x ra=%08x\n", t.id,
                                 t.currentPriority, int(t.status), int(t.waitReason), t.waitId, t.pc, t.ra);
            }
        }).detach();
    }

    void applyMksmOverrides(PS2Runtime &runtime)
    {
        startThreadWatch(runtime);
        configureMksmCdImage();
        mksm_gs_replay::configure(runtime.memory(), runtime.gs());
        // The runtime's internal heap goes below the ELF (free memory, the ELF loads at 0x100000) so as not to
        // trample the game's own heap at the end of .bss.
        runtime.setPrivateGuestHeap(0x000A0000u, 0x000FF000u);
        std::fprintf(stderr, "[mksm-override] Mortal Kombat: Shaolin Monks (SLES-53524): base setup only\n");
    }
}

PS2_REGISTER_GAME_OVERRIDE("Mortal Kombat: Shaolin Monks (SLES-53524)", "SLES_535.24", 0x0011C038u, 0u, applyMksmOverrides)
