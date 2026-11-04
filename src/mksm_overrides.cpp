// Game override module for Mortal Kombat: Shaolin Monks (PAL, SLES-53524), compiled into ps2EntryRunner next to the
// generated code (scripts\build.ps1 copies src\*.cpp into PS2Recomp\ps2xRuntime\src\runner).
//
// The runtime applies it when the booted ELF is named SLES_535.24 and its entry point is 0x0011C038. Everything
// specific to this game (address bindings, replacements, diagnostics) goes here; the runtime itself stays generic.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "ps2x/iop/iop_types.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <filesystem>

namespace
{
    uint32_t readU32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
        return value;
    }

    void writeU32(uint8_t *rdram, uint32_t address, uint32_t value)
    {
        std::memcpy(rdram + (address & 0x1FFFFFFFu), &value, sizeof(value));
    }

    // sbrk (0x4807F0) of the game's C library: one EndOfHeap syscall between DI and EI per call. At boot the game
    // looks for its largest allocatable block by calling malloc with a size that starts at 0x1FF0000 minus the
    // heap start and goes down 4 bytes at a time until a call succeeds. The runtime ends the heap at 0x1F00000
    // (its own guest pools sit above), so the search makes about a quarter of a million failing calls: 18 s of
    // boot through the syscall path. Served natively, with the same break pointer, errno and limit, it is instant.
    constexpr uint32_t kSbrkAddress = 0x004807F0u;
    constexpr uint32_t kSbrkFirstWord = 0x27BDFFC0u;  // addiu sp, sp, -0x40
    constexpr uint32_t kSbrkStatusWord = 0x40116000u; // mfc0 s1, Status (at +0x14)
    constexpr uint32_t kHeapBreakAddress = 0x00564294u; // the C library's current break
    constexpr uint32_t kImpurePtrAddress = 0x00563A7Cu; // newlib _impure_ptr; errno is its first field
    constexpr uint32_t kErrnoNoMemory = 12u;

    void hostSbrk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = getRegU32(ctx, 31);
        const uint32_t increment = getRegU32(ctx, 4);
        const uint32_t current = readU32(rdram, kHeapBreakAddress);
        const uint32_t next = current + increment; // 32-bit wrap, as the original
        if (runtime->guestHeapLimit() < next)
        {
            const uint32_t reent = readU32(rdram, kImpurePtrAddress);
            if (reent != 0u)
                writeU32(rdram, reent, kErrnoNoMemory);
            setReturnU32(ctx, 0xFFFFFFFFu);
            return;
        }
        writeU32(rdram, kHeapBreakAddress, next);
        setReturnU32(ctx, current);
    }

    void replaceSbrk(PS2Runtime &runtime)
    {
        const uint8_t *rdram = runtime.memory().getRDRAM();
        if (readU32(rdram, kSbrkAddress) != kSbrkFirstWord || readU32(rdram, kSbrkAddress + 0x14u) != kSbrkStatusWord)
        {
            std::fprintf(stderr, "[mksm] sbrk not found at 0x%08X, left to the recompiled code\n", kSbrkAddress);
            return;
        }
        if (!runtime.replaceFunction(kSbrkAddress, hostSbrk))
            std::fprintf(stderr, "[mksm] sbrk replacement refused by the runtime\n");
    }

    // MKSM_TRACE_SYNCV=1: logs the game's vsync wait wrapper (0x24D050: sceGsSyncV(0) and return) to tell a wait
    // that never completes from a game loop that waits every frame. "call" is a fresh entry, "resume" the return
    // from the wait (the runtime dispatches the function again at the instruction after the call).
    constexpr uint32_t kSyncVWrapperAddress = 0x0024D050u;
    PS2Runtime::RecompiledFunction g_originalSyncVWrapper = nullptr;

    void tracedSyncVWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t count = 0;
        ++count;
        if (count <= 40u || (count % 500u) == 0u)
        {
            std::fprintf(stderr, "[mksm-syncv] %s #%u pc=0x%08X ra=0x%08X vsyncTick=%llu\n",
                         ctx->pc == kSyncVWrapperAddress ? "call" : "resume", count, ctx->pc, getRegU32(ctx, 31),
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
        }
        if (count <= 3u || (count % 300u) == 0u)
        {
            const EeKernelSnapshot snapshot = runtime->eeScheduler().snapshot();
            std::fprintf(stderr, "[mksm-syncv] threads at call #%u:", count);
            for (const EeThreadSnapshot &thread : snapshot.threads)
            {
                std::fprintf(stderr, " [id=%d st=%d wait=%d/%d pc=0x%08X entry=0x%08X prio=%d wake=%u]", thread.id,
                             static_cast<int>(thread.status), static_cast<int>(thread.waitReason), thread.waitId,
                             thread.pc, thread.entry, thread.currentPriority, thread.wakeupCount);
            }
            std::fprintf(stderr, " semaphores=%u\n", static_cast<unsigned>(snapshot.semaphores.size()));
        }
        g_originalSyncVWrapper(rdram, ctx, runtime);
    }

    void installSyncVTrace(PS2Runtime &runtime)
    {
        const char *flag = std::getenv("MKSM_TRACE_SYNCV");
        if (!flag || !*flag || *flag == '0')
            return;
        g_originalSyncVWrapper = runtime.lookupFunction(kSyncVWrapperAddress);
        if (!g_originalSyncVWrapper || !runtime.replaceFunction(kSyncVWrapperAddress, tracedSyncVWrapper))
        {
            std::fprintf(stderr, "[mksm-syncv] cannot install the trace at 0x%08X\n", kSyncVWrapperAddress);
            return;
        }
        std::fprintf(stderr, "[mksm-syncv] tracing the vsync wait wrapper at 0x%08X\n", kSyncVWrapperAddress);
    }

    // MKSM_TRACE_THREADS=1: logs the arguments and results of the game's CreateThread (0x47FF20) and StartThread
    // (0x47FF40) syscall wrappers, to see why the worker threads of the streaming system do not appear.
    constexpr uint32_t kCreateThreadWrapper = 0x0047FF20u;
    constexpr uint32_t kStartThreadWrapper = 0x0047FF40u;
    PS2Runtime::RecompiledFunction g_originalCreateThread = nullptr;
    PS2Runtime::RecompiledFunction g_originalStartThread = nullptr;

    void tracedCreateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t param = getRegU32(ctx, 4);
        uint32_t fields[8] = {};
        for (uint32_t i = 0; i < 8; ++i)
            fields[i] = readU32(rdram, param + 4 * i);
        g_originalCreateThread(rdram, ctx, runtime);
        std::fprintf(stderr,
                     "[mksm-threads] CreateThread(param=0x%08X: status=%u func=0x%08X stack=0x%08X size=0x%X gp=0x%08X prio=%d cur=%d attr=0x%X) = %d\n",
                     param, fields[0], fields[1], fields[2], fields[3], fields[4], static_cast<int>(fields[5]),
                     static_cast<int>(fields[6]), fields[7], static_cast<int>(getRegU32(ctx, 2)));
    }

    void tracedStartThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t id = getRegU32(ctx, 4);
        const uint32_t arg = getRegU32(ctx, 5);
        g_originalStartThread(rdram, ctx, runtime);
        std::fprintf(stderr, "[mksm-threads] StartThread(id=%d, arg=0x%08X) = %d\n", static_cast<int>(id), arg,
                     static_cast<int>(getRegU32(ctx, 2)));
    }

    void installThreadTrace(PS2Runtime &runtime)
    {
        const char *flag = std::getenv("MKSM_TRACE_THREADS");
        if (!flag || !*flag || *flag == '0')
            return;
        g_originalCreateThread = runtime.lookupFunction(kCreateThreadWrapper);
        g_originalStartThread = runtime.lookupFunction(kStartThreadWrapper);
        if (!g_originalCreateThread || !g_originalStartThread ||
            !runtime.replaceFunction(kCreateThreadWrapper, tracedCreateThread) ||
            !runtime.replaceFunction(kStartThreadWrapper, tracedStartThread))
        {
            std::fprintf(stderr, "[mksm-threads] cannot install the trace\n");
            return;
        }
        std::fprintf(stderr, "[mksm-threads] tracing CreateThread and StartThread\n");
    }

    // MKSM_TRACE_WATCH=0xADDR:words,...: guest memory words printed with every call-count summary.
    struct WatchRange
    {
        uint32_t address = 0;
        uint32_t words = 0;
        bool indirect = false; // "*0xADDR": the address holds a pointer to the words
    };
    WatchRange g_watches[8];
    size_t g_watchCount = 0;

    void parseWatches()
    {
        const char *list = std::getenv("MKSM_TRACE_WATCH");
        if (!list || !*list)
            return;
        const char *cursor = list;
        while (*cursor && g_watchCount < 8)
        {
            bool indirect = false;
            if (*cursor == '*')
            {
                indirect = true;
                ++cursor;
            }
            char *end = nullptr;
            const unsigned long address = std::strtoul(cursor, &end, 16);
            if (end == cursor)
                break;
            unsigned long words = 4;
            if (*end == ':')
                words = std::strtoul(end + 1, &end, 10);
            cursor = (*end == ',') ? end + 1 : end;
            g_watches[g_watchCount++] = WatchRange{static_cast<uint32_t>(address), static_cast<uint32_t>(words > 64 ? 64 : words), indirect};
        }
    }

    void printWatches(const uint8_t *rdram)
    {
        for (size_t i = 0; i < g_watchCount; ++i)
        {
            uint32_t base = g_watches[i].address;
            if (g_watches[i].indirect)
                base = readU32(rdram, base);
            std::fprintf(stderr, "[mksm-watch] %s0x%08X (0x%08X):", g_watches[i].indirect ? "*" : "", g_watches[i].address, base);
            if (base >= 0x02000000u)
                base = 0;
            for (uint32_t w = 0; w < g_watches[i].words; ++w)
                std::fprintf(stderr, " %08X", readU32(rdram, base + 4 * w));
            std::fprintf(stderr, "\n");
        }
    }

    void printThreads(PS2Runtime *runtime, uint32_t stamp)
    {
        const EeKernelSnapshot snapshot = runtime->eeScheduler().snapshot();
        std::fprintf(stderr, "[mksm-threads] at %u:", stamp);
        for (const EeThreadSnapshot &thread : snapshot.threads)
        {
            std::fprintf(stderr, " [id=%d st=%d wait=%d/%d pc=0x%08X prio=%d wake=%u]", thread.id,
                         static_cast<int>(thread.status), static_cast<int>(thread.waitReason), thread.waitId,
                         thread.pc, thread.currentPriority, thread.wakeupCount);
        }
        std::fprintf(stderr, "\n");
    }

    // MKSM_TRACE_CALLS=0xADDR,0xADDR,...: counts the calls of up to 32 guest functions and logs the first eight of
    // each with the argument registers and the caller; then a summary of the counts every 300 calls of the first
    // traced function. A traced function is entered again when it resumes after a blocking stub (pc != address),
    // those entries are counted separately.
    constexpr size_t kMaxTracedCalls = 32;
    struct TracedCall
    {
        uint32_t address = 0;
        PS2Runtime::RecompiledFunction original = nullptr;
        uint32_t calls = 0;
        uint32_t resumes = 0;
        uint64_t nanos = 0; // host time spent inside (inclusive; calls that block do not return and are not timed)
    };
    TracedCall g_tracedCalls[kMaxTracedCalls];
    size_t g_tracedCallCount = 0;
    uint32_t g_traceSummaryInterval = 3000; // MKSM_TRACE_SUMMARY=<calls of the first traced function>
    std::chrono::steady_clock::time_point g_traceStart;

    void tracedCallEntry(size_t index, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TracedCall &entry = g_tracedCalls[index];
        if (ctx->pc != entry.address)
        {
            ++entry.resumes;
        }
        else
        {
            ++entry.calls;
            if (entry.calls <= 8u)
            {
                std::fprintf(stderr, "[mksm-calls] 0x%08X #%u a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X ra=0x%08X\n",
                             entry.address, entry.calls, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                             getRegU32(ctx, 7), getRegU32(ctx, 31));
            }
            if (index == 0 && (entry.calls % g_traceSummaryInterval) == 0u)
            {
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_traceStart).count();
                std::fprintf(stderr, "[mksm-calls] t=%.1fs counts:", seconds);
                for (size_t i = 0; i < g_tracedCallCount; ++i)
                {
                    const TracedCall &traced = g_tracedCalls[i];
                    const double averageMicros = traced.calls ? static_cast<double>(traced.nanos) / traced.calls / 1000.0 : 0.0;
                    std::fprintf(stderr, " 0x%X=%u/%u(%.1fus)", traced.address, traced.calls, traced.resumes, averageMicros);
                }
                std::fprintf(stderr, "\n");
                printWatches(rdram);
                printThreads(runtime, entry.calls);
            }
        }
        const auto start = std::chrono::steady_clock::now();
        entry.original(rdram, ctx, runtime);
        entry.nanos += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }

    template <size_t Index>
    void tracedCallThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        tracedCallEntry(Index, rdram, ctx, runtime);
    }

    template <size_t... Indices>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Indices)> makeTracedCallThunks(std::index_sequence<Indices...>)
    {
        return {&tracedCallThunk<Indices>...};
    }

    void installCallTrace(PS2Runtime &runtime)
    {
        const char *list = std::getenv("MKSM_TRACE_CALLS");
        if (!list || !*list)
            return;
        if (const char *interval = std::getenv("MKSM_TRACE_SUMMARY"))
            g_traceSummaryInterval = std::max(1u, static_cast<uint32_t>(std::strtoul(interval, nullptr, 10)));
        g_traceStart = std::chrono::steady_clock::now();
        static const auto thunks = makeTracedCallThunks(std::make_index_sequence<kMaxTracedCalls>{});
        const char *cursor = list;
        while (*cursor && g_tracedCallCount < kMaxTracedCalls)
        {
            char *end = nullptr;
            const unsigned long address = std::strtoul(cursor, &end, 16);
            if (end == cursor)
                break;
            cursor = (*end == ',') ? end + 1 : end;
            TracedCall &entry = g_tracedCalls[g_tracedCallCount];
            entry.address = static_cast<uint32_t>(address);
            entry.original = runtime.lookupFunction(entry.address);
            if (!entry.original || !runtime.replaceFunction(entry.address, thunks[g_tracedCallCount]))
            {
                std::fprintf(stderr, "[mksm-calls] cannot trace 0x%08X\n", entry.address);
                continue;
            }
            ++g_tracedCallCount;
        }
        std::fprintf(stderr, "[mksm-calls] tracing %u functions\n", static_cast<unsigned>(g_tracedCallCount));
    }

    // MKSM_PROFILE=1: a host thread samples the scheduler snapshot every millisecond (running thread and its pc)
    // and prints, every ten seconds, the share of samples per thread and the hottest 64-byte code buckets.
    std::thread g_profilerThread;
    std::atomic<bool> g_profilerStop{false};
    uint64_t g_lastIopInstructions = 0;
    PS2Runtime *g_profiledRuntime = nullptr;

    void profilerLoop(PS2Runtime *runtime)
    {
        std::unordered_map<uint32_t, uint32_t> buckets;
        std::unordered_map<int, uint32_t> threads;
        uint32_t samples = 0;
        auto lastReport = std::chrono::steady_clock::now();
        while (!g_profilerStop.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const EeKernelSnapshot snapshot = runtime->eeScheduler().snapshot();
            const int running = snapshot.runningThreadId;
            ++samples;
            ++threads[running];
            for (const EeThreadSnapshot &thread : snapshot.threads)
            {
                if (thread.id == running)
                    ++buckets[thread.pc & ~0x3Fu];
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport < std::chrono::seconds(10))
                continue;
            lastReport = now;
            const ps2x::iop::DebugSnapshot iop = runtime->iopDebugSnapshot();
            std::fprintf(stderr, "[mksm-prof] iop: %.1f M instructions/s, %u threads, %u rpc servers\n",
                         (iop.emulatorInstructions - g_lastIopInstructions) / 10.0e6, iop.emulatorThreads, iop.emulatorRpcServers);
            g_lastIopInstructions = iop.emulatorInstructions;
            std::fprintf(stderr, "[mksm-prof] samples=%u threads:", samples);
            for (const auto &[id, count] : threads)
                std::fprintf(stderr, " %d=%.0f%%", id, 100.0 * count / samples);
            std::vector<std::pair<uint32_t, uint32_t>> sorted(buckets.begin(), buckets.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            std::fprintf(stderr, " top:");
            for (size_t i = 0; i < sorted.size() && i < 16; ++i)
                std::fprintf(stderr, " 0x%X=%.1f%%", sorted[i].first, 100.0 * sorted[i].second / samples);
            std::fprintf(stderr, "\n");
            buckets.clear();
            threads.clear();
            samples = 0;
        }
    }

    // MKSM_PROFILE=host: samples the host call stack of the runtime's game thread (the EE executor) every two
    // milliseconds and prints, every ten seconds, the hottest functions by leaf and by inclusive samples, with the
    // names from the build's debug information. Windows only.
#if defined(_WIN32)
    struct HostSample
    {
        uint64_t leaf = 0;
        uint64_t inclusive = 0;
    };

    std::string hostSymbol(HANDLE process, uint64_t address)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol))
            return std::string(symbol->Name);
        char fallback[32];
        std::snprintf(fallback, sizeof(fallback), "0x%llX", static_cast<unsigned long long>(address));
        return fallback;
    }

    HANDLE findGameThread()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return nullptr;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        HANDLE found = nullptr;
        const DWORD self = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snapshot, &entry); ok && !found; ok = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != self)
                continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            PWSTR description = nullptr;
            if (SUCCEEDED(GetThreadDescription(thread, &description)) && description)
            {
                if (std::wcscmp(description, L"GameThread") == 0)
                    found = thread;
                LocalFree(description);
            }
            if (!found)
                CloseHandle(thread);
        }
        CloseHandle(snapshot);
        return found;
    }

    void hostProfilerLoop()
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        HANDLE thread = nullptr;
        while (!thread && !g_profilerStop.load(std::memory_order_relaxed))
        {
            thread = findGameThread();
            if (!thread)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::fprintf(stderr, "[mksm-hostprof] game thread found, sampling every 2 ms\n");
        std::unordered_map<uint64_t, HostSample> samples;
        uint64_t total = 0;
        auto lastReport = std::chrono::steady_clock::now();
        while (!g_profilerStop.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (SuspendThread(thread) == static_cast<DWORD>(-1))
                continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            uint64_t frames[24];
            size_t frameCount = 0;
            if (GetThreadContext(thread, &context))
            {
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = context.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                while (frameCount < 24 &&
                       StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                                   SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
                       frame.AddrPC.Offset != 0)
                {
                    frames[frameCount++] = frame.AddrPC.Offset;
                }
            }
            ResumeThread(thread);
            if (frameCount == 0)
                continue;
            ++total;
            std::unordered_map<uint64_t, bool> seen;
            for (size_t i = 0; i < frameCount; ++i)
            {
                HostSample &sample = samples[frames[i]];
                if (i == 0)
                    ++sample.leaf;
                if (!seen[frames[i]])
                {
                    seen[frames[i]] = true;
                    ++sample.inclusive;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport < std::chrono::seconds(10))
                continue;
            lastReport = now;
            if (g_profiledRuntime)
            {
                const ps2x::iop::DebugSnapshot iop = g_profiledRuntime->iopDebugSnapshot();
                std::fprintf(stderr, "[mksm-hostprof] iop: %.1f M instructions/s, %u threads\n",
                             (iop.emulatorInstructions - g_lastIopInstructions) / 10.0e6, iop.emulatorThreads);
                g_lastIopInstructions = iop.emulatorInstructions;
            }
            // Aggregate by symbol so that the many return addresses inside one function add up.
            std::unordered_map<std::string, HostSample> bySymbol;
            for (const auto &[address, sample] : samples)
            {
                HostSample &aggregate = bySymbol[hostSymbol(process, address)];
                aggregate.leaf += sample.leaf;
                aggregate.inclusive += sample.inclusive;
            }
            std::vector<std::pair<std::string, HostSample>> sorted(bySymbol.begin(), bySymbol.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.leaf > b.second.leaf; });
            std::fprintf(stderr, "[mksm-hostprof] %llu samples, by leaf:", static_cast<unsigned long long>(total));
            for (size_t i = 0; i < sorted.size() && i < 14; ++i)
                std::fprintf(stderr, " %s=%.1f%%", sorted[i].first.c_str(), 100.0 * sorted[i].second.leaf / total);
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.inclusive > b.second.inclusive; });
            std::fprintf(stderr, "\n[mksm-hostprof] by inclusive:");
            for (size_t i = 0; i < sorted.size() && i < 24; ++i)
                std::fprintf(stderr, " %s=%.1f%%", sorted[i].first.c_str(), 100.0 * sorted[i].second.inclusive / total);
            std::fprintf(stderr, "\n");
            samples.clear();
            total = 0;
        }
        CloseHandle(thread);
    }
#endif

    void startProfiler(PS2Runtime &runtime)
    {
        const char *flag = std::getenv("MKSM_PROFILE");
        if (!flag || !*flag || *flag == '0')
            return;
        g_profiledRuntime = &runtime;
#if defined(_WIN32)
        if (std::strcmp(flag, "host") == 0)
        {
            g_profilerThread = std::thread(hostProfilerLoop);
            g_profilerThread.detach();
            return;
        }
#endif
        g_profilerThread = std::thread(profilerLoop, &runtime);
        g_profilerThread.detach();
        std::fprintf(stderr, "[mksm-prof] sampling the running guest thread every millisecond\n");
    }

    // MKSM_ISO=<path>: the disc image for the sector reads the game does outside the files extracted into game\
    // (the runtime serves sceCdRead from the registered files first, then from this image).
    void configureDiscImage()
    {
        const char *iso = std::getenv("MKSM_ISO");
        if (!iso || !*iso)
            return;
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = std::filesystem::path(iso);
        PS2Runtime::setIoPaths(paths);
        std::fprintf(stderr, "[mksm] disc image: %s\n", iso);
    }

    void applyMksmOverrides(PS2Runtime &runtime)
    {
        // The runtime logs through stdout, which is block-buffered when redirected to a file; without this the end
        // of logs/run.log is lost when scripts/run.cmd stops the game.
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        configureDiscImage();
        replaceSbrk(runtime);
        installSyncVTrace(runtime);
        installThreadTrace(runtime);
        parseWatches();
        installCallTrace(runtime);
        startProfiler(runtime);
        std::fprintf(stderr, "[mksm] overrides applied: Mortal Kombat: Shaolin Monks (SLES-53524)\n");
    }
}

PS2_REGISTER_GAME_OVERRIDE("Mortal Kombat: Shaolin Monks (SLES-53524)", "SLES_535.24", 0x0011C038u, 0u, applyMksmOverrides)
