// VU1 programs translated ahead of time (tools/vu1gen) in place of upstream's interpreter.
//
// The game's VU1 microcode is translated to C++ at build time (tools/vu1gen/PROGRESS.md); the generated library
// (vu1n_programs) is linked into the runner by tools/vu1gen/cmake/targets.cmake. This file replaces the VIF1 MSCAL
// and MSCNT callbacks upstream's runtime installs (PS2Runtime::syncCoreSubsystems) and runs a launch with the
// translated program when the micro memory is one of the translated images and the entry is one of its entries;
// everything else stays with the interpreter. No upstream patch: the callbacks are public PS2Memory members, set
// once at initialize(), before applyMksmOverrides runs.
//
//   MKSM_VU1=0       upstream's interpreter for every launch (the callbacks are not replaced)
//   MKSM_VU1=1       the translated programs (the default)
//   MKSM_VU1=verify  the interpreter runs every launch on the real state, as with 0; each launch the translation
//                    handles also runs on a copy, and the two results are compared: the compared fields of
//                    VU1State, the 16 KB of data memory and the PATH1 packets. [mksm-vu1] lines report the
//                    differences; MKSM_VU1_DUMP=<folder> writes the launches that differ as launch records
//                    (launches_<hash>.bin and code_<hash>.bin, which tools/vu1gen's vu1difftest --captures replays).
//   MKSM_VU1=verify:<n>  the translated programs run the game, as with 1; every n-th launch is also run by a second
//                    interpreter on a copy of its input, and the two results are compared the same way. With n = 1
//                    that is every launch (at the interpreter's speed); a larger n checks the launches the game
//                    makes at full speed, which differ from the ones it makes when it runs slowly.
//   MKSM_VU1_STATS=1 (diagnostic) with MKSM_VU1=1: a [mksm-vu1] line every 10 s with the launches translated,
//                    interpreted and given back at an indirect jump. The verify modes always print their counts.
//
// Rules this follows (vu1-study/integration.md 1.2, 1.9): a translated launch is prepared as upstream's execute()
// prepares one, an MSCNT included; after a translated launch the interpreter's private pipeline state is stale, so
// an interpreted MSCNT then enters through execute() at the saved pc instead of resume(); the PATH1 packets of a
// translated launch are submitted after it returned, under the caller's MXCSR (vu1nRun sets the VU rounding inside).
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include "vu1n_api.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{
    enum class Vu1Mode
    {
        Interpreter,
        Translated,
        Verify,
    };

    struct Vu1Stats
    {
        uint64_t translated = 0;
        uint64_t interpreted = 0;   // unknown micro memory or an entry the translation does not handle
        uint64_t unknownTarget = 0; // given back at an indirect jump, then interpreted
        uint64_t verified = 0;
        uint64_t differences = 0;
        uint64_t droppedKicks = 0;  // XGKICKs of translated launches whose packet passes 64 KB (corrupt data)
    };

    struct Vu1Glue
    {
        PS2Runtime *runtime = nullptr;
        Vu1Mode mode = Vu1Mode::Translated;
        PS2Memory::Vu1MscalCallback interpreterMscal;
        PS2Memory::Vu1MscntCallback interpreterMscnt;

        uint64_t generation = ~0ull;
        const Vu1nProgram *program = nullptr;
        std::vector<const Vu1nProgram *> announced; // the images already named in the log
        std::vector<uint64_t> unknownImages;        // hashes of micro memories that are no translated image
        // The interpreter's private pipeline state no longer belongs to the state in VU1State (a translated launch
        // ran since the interpreter's last one): an interpreted MSCNT must enter through execute().
        bool interpreterStale = false;

        Vu1nHost host;
        std::unique_ptr<VU1State> snapshot = std::make_unique<VU1State>();
        std::vector<uint8_t> snapshotData = std::vector<uint8_t>(PS2_VU1_DATA_SIZE);

        // verify
        bool capturing = false;
        Vu1nHost oracleGif; // the interpreter's PATH1 packets of the launch being verified
        std::unique_ptr<VU1State> shadow = std::make_unique<VU1State>();
        std::vector<uint8_t> shadowData = std::vector<uint8_t>(PS2_VU1_DATA_SIZE);
        std::vector<uint8_t> inputData = std::vector<uint8_t>(PS2_VU1_DATA_SIZE);
        std::string dumpFolder;
        bool statsLines = false; // MKSM_VU1_STATS: the periodic line in the translated mode too

        // verify:<n>
        uint32_t sampleEvery = 0;                  // 0: not this mode
        uint64_t sampleCounter = 0;
        bool shadowRun = false;                    // the second interpreter is running: its packets are only copied
        std::unique_ptr<VU1Interpreter> shadowVu1; // runs the sampled launches on a copy
        uint64_t dumped = 0;

        Vu1Stats stats;
        Vu1Stats reported;
        std::chrono::steady_clock::time_point lastReport{};
    };

    Vu1Glue g_vu1;

    R5900Context &guestContext(PS2Runtime &runtime)
    {
        R5900Context *context = runtime.eeScheduler().currentContext();
        return context ? *context : runtime.cpu();
    }

    uint64_t imageHash(const uint8_t *bytes, size_t size)
    {
        // The hash the launch captures are named by (tools/vu1gen/src/images.cpp, imageHash): FNV-1a 64 with the
        // offset basis 1469598103934665603 (the standard basis without its last digit).
        uint64_t hash = 1469598103934665603ull;
        for (size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    // The translated program for the micro memory, looked up again only after an upload changed it.
    const Vu1nProgram *currentProgram(PS2Memory &memory)
    {
        const uint64_t generation = memory.getVU1CodeGeneration();
        if (generation == g_vu1.generation)
            return g_vu1.program;
        g_vu1.generation = generation;
        const Vu1nProgram *program = vu1nFindProgram(memory.getVU1Code());
        g_vu1.program = program;
        // Each image is announced the first time it is seen (the game switches programs several times a frame).
        if (program)
        {
            for (const Vu1nProgram *seen : g_vu1.announced)
            {
                if (seen == program)
                    return program;
            }
            g_vu1.announced.push_back(program);
            std::fprintf(stderr, "[mksm-vu1] micro memory %016llX: %s\n",
                         static_cast<unsigned long long>(imageHash(memory.getVU1Code(), PS2_VU1_CODE_SIZE)), program->name);
        }
        else if (g_vu1.unknownImages.size() < 16u)
        {
            const uint64_t hash = imageHash(memory.getVU1Code(), PS2_VU1_CODE_SIZE);
            for (uint64_t seen : g_vu1.unknownImages)
            {
                if (seen == hash)
                    return program;
            }
            g_vu1.unknownImages.push_back(hash);
            std::fprintf(stderr, "[mksm-vu1] micro memory %016llX: not a translated image, interpreted\n",
                         static_cast<unsigned long long>(hash));
        }
        return program;
    }

    // What upstream's callbacks do, with execute() at the saved pc for an MSCNT after a translated launch.
    void interpret(bool mscal, uint32_t startPc, uint32_t top, uint32_t itop)
    {
        PS2Runtime &runtime = *g_vu1.runtime;
        if (mscal)
            g_vu1.interpreterMscal(startPc, top, itop);
        else if (!g_vu1.interpreterStale)
            g_vu1.interpreterMscnt(top, itop);
        else
        {
            R5900Context &context = guestContext(runtime);
            VU1State &state = runtime.vu1().state();
            state.dBitEnabled = (context.vu0_fbrst & (1u << 10)) != 0u;
            state.tBitEnabled = (context.vu0_fbrst & (1u << 11)) != 0u;
            PS2Memory &memory = runtime.memory();
            runtime.vu1().execute(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                  runtime.gs(), &memory, state.pc, top, itop, 65536);
            context.vu0_vpu_stat = (context.vu0_vpu_stat & ~0x0600u) | (state.stoppedByD ? 0x0200u : 0u) |
                                   (state.stoppedByT ? 0x0400u : 0u);
        }
        g_vu1.interpreterStale = false;
    }

    // What execute() does to the state before it runs (tools/vu1gen/difftest/launch.cpp, prepareLaunch).
    void prepare(VU1State &state, uint32_t entryPc, uint32_t top, uint32_t itop, const R5900Context &context)
    {
        state.dBitEnabled = (context.vu0_fbrst & (1u << 10)) != 0u;
        state.tBitEnabled = (context.vu0_fbrst & (1u << 11)) != 0u;
        state.pc = entryPc;
        state.ebit = false;
        state.haltAfterDelaySlot = false;
        state.stoppedByD = false;
        state.stoppedByT = false;
        state.top = top;
        state.itop = itop;
        state.branchPending = false;
        state.branchTarget = 0u;
        state.branchDelay = 0u;
        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
    }

    void submit(PS2Memory &memory, const Vu1nHost &host)
    {
        for (size_t index = 0; index < host.kicks.size(); ++index)
            memory.submitGifPacket(GifPathId::Path1, host.packet(index), host.kicks[index].bytes);
    }

    void report()
    {
        if (g_vu1.mode != Vu1Mode::Verify && g_vu1.sampleEvery == 0u && !g_vu1.statsLines)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (g_vu1.lastReport.time_since_epoch().count() == 0)
        {
            g_vu1.lastReport = now;
            return;
        }
        if (now - g_vu1.lastReport < std::chrono::seconds(10))
            return;
        g_vu1.lastReport = now;
        const Vu1Stats &s = g_vu1.stats;
        Vu1Stats &r = g_vu1.reported;
        if (g_vu1.sampleEvery != 0u)
            std::fprintf(stderr, "[mksm-vu1] last 10 s: %llu launches translated, %llu of them verified, %llu differ, %llu interpreted (total %llu verified, %llu differ)\n",
                         static_cast<unsigned long long>(s.translated - r.translated),
                         static_cast<unsigned long long>(s.verified - r.verified),
                         static_cast<unsigned long long>(s.differences - r.differences),
                         static_cast<unsigned long long>(s.interpreted - r.interpreted),
                         static_cast<unsigned long long>(s.verified), static_cast<unsigned long long>(s.differences));
        else if (g_vu1.mode == Vu1Mode::Verify)
            std::fprintf(stderr, "[mksm-vu1] last 10 s: %llu launches verified, %llu differ, %llu interpreted only (total %llu verified, %llu differ)\n",
                         static_cast<unsigned long long>(s.verified - r.verified),
                         static_cast<unsigned long long>(s.differences - r.differences),
                         static_cast<unsigned long long>(s.interpreted - r.interpreted),
                         static_cast<unsigned long long>(s.verified), static_cast<unsigned long long>(s.differences));
        else
            std::fprintf(stderr, "[mksm-vu1] last 10 s: %llu launches translated, %llu interpreted, %llu given back at an indirect jump\n",
                         static_cast<unsigned long long>(s.translated - r.translated),
                         static_cast<unsigned long long>(s.interpreted - r.interpreted),
                         static_cast<unsigned long long>(s.unknownTarget - r.unknownTarget));
        r = s;
    }

    void verifySampled(const Vu1nProgram &program, bool mscal, uint32_t entryPc, uint32_t top, uint32_t itop);

    // ---- MKSM_VU1=1 ----

    void launch(bool mscal, uint32_t startPc, uint32_t top, uint32_t itop)
    {
        PS2Runtime &runtime = *g_vu1.runtime;
        PS2Memory &memory = runtime.memory();
        VU1State &state = runtime.vu1().state();
        const uint32_t entryPc = mscal ? (startPc & 0x3FFFu) : (state.pc & 0x3FFFu);
        const Vu1nProgram *program = currentProgram(memory);
        if (!program || !program->handles(entryPc))
        {
            ++g_vu1.stats.interpreted;
            interpret(mscal, startPc, top, itop);
            report();
            return;
        }

        R5900Context &context = guestContext(runtime);
        const bool sampled = g_vu1.sampleEvery != 0u && ++g_vu1.sampleCounter % g_vu1.sampleEvery == 0u;
        if (sampled)
        {
            std::memcpy(g_vu1.shadow.get(), &state, sizeof(VU1State));
            std::memcpy(g_vu1.inputData.data(), memory.getVU1Data(), PS2_VU1_DATA_SIZE);
        }
        const bool snapshot = program->needsSnapshot(entryPc);
        if (snapshot)
        {
            std::memcpy(g_vu1.snapshot.get(), &state, sizeof(VU1State));
            std::memcpy(g_vu1.snapshotData.data(), memory.getVU1Data(), PS2_VU1_DATA_SIZE);
        }
        prepare(state, entryPc, top, itop, context);
        const Vu1nStatus status = vu1nRun(*program, state, memory.getVU1Data(), g_vu1.host, entryPc);
        if (status == Vu1nStatus::UnknownTarget)
        {
            // Back to the state before the launch, and the interpreter runs it whole.
            std::memcpy(&state, g_vu1.snapshot.get(), sizeof(VU1State));
            std::memcpy(memory.getVU1Data(), g_vu1.snapshotData.data(), PS2_VU1_DATA_SIZE);
            ++g_vu1.stats.unknownTarget;
            interpret(mscal, startPc, top, itop);
            report();
            return;
        }
        submit(memory, g_vu1.host);
        if (g_vu1.host.dropped != 0u)
        {
            // The interpreter reports an error for such a packet and stops the launch where the transfer reaches
            // 64 KB; the translated launch ran to its end without the packet. Only corrupt data gets here.
            if (g_vu1.stats.droppedKicks == 0u)
                std::fprintf(stderr, "[mksm-vu1] %s 0x%04X: an XGKICK packet passes 64 KB and was not sent (reported once)\n",
                             program->name, entryPc);
            g_vu1.stats.droppedKicks += g_vu1.host.dropped;
        }
        context.vu0_vpu_stat &= ~0x0600u; // the translated programs have no D or T bit
        g_vu1.interpreterStale = true;
        ++g_vu1.stats.translated;
        if (sampled)
            verifySampled(*program, mscal, entryPc, top, itop);
        report();
    }

    // ---- MKSM_VU1=verify ----

    void put32(uint8_t *p, uint32_t value)
    {
        for (int index = 0; index < 4; ++index)
            p[index] = static_cast<uint8_t>(value >> (8 * index));
    }

    // A launch record of the capture format (tools/vu1gen/difftest/captures.h), as an MSCAL at the entry.
    void dumpLaunch(const VU1State &input, const uint8_t *data, uint32_t entryPc, uint32_t top, uint32_t itop)
    {
        if (g_vu1.dumpFolder.empty() || g_vu1.dumped >= 200u)
            return;
        PS2Memory &memory = g_vu1.runtime->memory();
        const uint64_t hash = imageHash(memory.getVU1Code(), PS2_VU1_CODE_SIZE);
        char hashText[20];
        std::snprintf(hashText, sizeof(hashText), "%016llX", static_cast<unsigned long long>(hash));
        const std::filesystem::path folder(g_vu1.dumpFolder);
        std::error_code code;
        std::filesystem::create_directories(folder, code);
        const std::filesystem::path codePath = folder / ("code_" + std::string(hashText) + ".bin");
        if (!std::filesystem::exists(codePath, code))
        {
            if (std::FILE *file = std::fopen(codePath.string().c_str(), "wb"))
            {
                std::fwrite(memory.getVU1Code(), 1u, PS2_VU1_CODE_SIZE, file);
                std::fclose(file);
            }
        }
        std::FILE *file = std::fopen((folder / ("launches_" + std::string(hashText) + ".bin")).string().c_str(), "ab");
        if (!file)
            return;
        uint8_t header[48] = {'V', 'U', '1', 'L'};
        put32(header + 4, 1u);
        put32(header + 8, sizeof(VU1State));
        put32(header + 12, PS2_VU1_DATA_SIZE);
        put32(header + 16, entryPc);
        put32(header + 20, 0u);
        put32(header + 24, top);
        put32(header + 28, itop);
        put32(header + 32, static_cast<uint32_t>(hash));
        put32(header + 36, static_cast<uint32_t>(hash >> 32));
        put32(header + 40, static_cast<uint32_t>(g_vu1.dumped));
        put32(header + 44, 0u);
        std::fwrite(header, 1u, sizeof(header), file);
        std::fwrite(&input, 1u, sizeof(VU1State), file);
        std::fwrite(data, 1u, PS2_VU1_DATA_SIZE, file);
        std::fclose(file);
        ++g_vu1.dumped;
    }

    // The first field of the compared ones that differs, or an empty string.
    std::string firstDifference(const VU1State &oracle, const VU1State &translated)
    {
        char text[160];
        for (uint32_t reg = 1; reg < 32u; ++reg)
        {
            for (uint32_t lane = 0; lane < 4u; ++lane)
            {
                uint32_t a, b;
                std::memcpy(&a, &oracle.vf[reg][lane], 4u);
                std::memcpy(&b, &translated.vf[reg][lane], 4u);
                if (a != b)
                {
                    std::snprintf(text, sizeof(text), "vf%02u.%c: interpreter %08X, translation %08X", reg, "xyzw"[lane], a, b);
                    return text;
                }
            }
        }
        for (uint32_t reg = 1; reg < 16u; ++reg)
        {
            if (oracle.vi[reg] != translated.vi[reg])
            {
                std::snprintf(text, sizeof(text), "vi%02u: interpreter %d, translation %d", reg, oracle.vi[reg], translated.vi[reg]);
                return text;
            }
        }
        const struct
        {
            const char *name;
            const void *a;
            const void *b;
            size_t size;
        } fields[] = {
            {"acc", oracle.acc, translated.acc, sizeof(oracle.acc)},
            {"q", &oracle.q, &translated.q, 4u},
            {"p", &oracle.p, &translated.p, 4u},
            {"i", &oracle.i, &translated.i, 4u},
            {"r", &oracle.r, &translated.r, 4u},
            {"pc", &oracle.pc, &translated.pc, 4u},
            {"clip", &oracle.clip, &translated.clip, 4u},
            {"branchTarget", &oracle.branchTarget, &translated.branchTarget, 4u},
            {"branchDelay", &oracle.branchDelay, &translated.branchDelay, 4u},
        };
        for (const auto &field : fields)
        {
            if (std::memcmp(field.a, field.b, field.size) != 0)
            {
                std::snprintf(text, sizeof(text), "%s", field.name);
                return text;
            }
        }
        if (oracle.ebit != translated.ebit || oracle.haltAfterDelaySlot != translated.haltAfterDelaySlot ||
            oracle.branchPending != translated.branchPending || oracle.stoppedByD != translated.stoppedByD ||
            oracle.stoppedByT != translated.stoppedByT)
            return "ebit, haltAfterDelaySlot, branchPending or stoppedByD/T";
        return {};
    }

    // The first difference between the interpreter's result of a launch and the translation's: the compared fields
    // of the state, the data memory, then the PATH1 packets. Empty when they are equal.
    std::string describeDifference(const VU1State &oracle, const uint8_t *oracleData, const Vu1nHost &oraclePackets,
                                   const VU1State &translated, const uint8_t *translatedData, const Vu1nHost &translatedPackets)
    {
        std::string difference = firstDifference(oracle, translated);
        if (difference.empty())
        {
            for (uint32_t offset = 0; offset < PS2_VU1_DATA_SIZE; offset += 4u)
            {
                if (std::memcmp(oracleData + offset, translatedData + offset, 4u) != 0)
                {
                    uint32_t a, b;
                    std::memcpy(&a, oracleData + offset, 4u);
                    std::memcpy(&b, translatedData + offset, 4u);
                    char text[96];
                    std::snprintf(text, sizeof(text), "data memory at 0x%04X: interpreter %08X, translation %08X", offset, a, b);
                    difference = text;
                    break;
                }
            }
        }
        if (difference.empty())
        {
            const Vu1nHost &a = oraclePackets;
            const Vu1nHost &b = translatedPackets;
            if (a.kicks.size() != b.kicks.size())
                difference = "packet count: interpreter " + std::to_string(a.kicks.size()) + ", translation " + std::to_string(b.kicks.size());
            for (size_t index = 0; difference.empty() && index < a.kicks.size(); ++index)
            {
                if (a.kicks[index].bytes != b.kicks[index].bytes)
                    difference = "packet " + std::to_string(index + 1u) + " of " + std::to_string(a.kicks.size()) + ": " +
                                 std::to_string(a.kicks[index].bytes) + " bytes in the interpreter's, " +
                                 std::to_string(b.kicks[index].bytes) + " in the translation's";
                else if (std::memcmp(a.packet(index), b.packet(index), a.kicks[index].bytes) != 0)
                {
                    uint32_t at = 0;
                    while (a.packet(index)[at] == b.packet(index)[at])
                        ++at;
                    difference = "packet " + std::to_string(index + 1u) + " of " + std::to_string(a.kicks.size()) +
                                 ": first different byte at " + std::to_string(at) + " of " + std::to_string(a.kicks[index].bytes);
                }
            }
        }
        return difference;
    }

    void noteDifference(const Vu1nProgram &program, bool mscal, uint32_t entryPc, const std::string &difference,
                        const VU1State &input, const uint8_t *inputData, uint32_t top, uint32_t itop)
    {
        if (++g_vu1.stats.differences <= 20u)
            std::fprintf(stderr, "[mksm-vu1] DIFFERENCE %s %s 0x%04X: %s\n", program.name, mscal ? "MSCAL" : "MSCNT", entryPc,
                         difference.c_str());
        dumpLaunch(input, inputData, entryPc, top, itop);
    }

    // verify:<n>: the launch the translation just ran on the real state, run again by the second interpreter on the
    // copy of its input (g_vu1.shadow, g_vu1.inputData), and compared.
    void verifySampled(const Vu1nProgram &program, bool mscal, uint32_t entryPc, uint32_t top, uint32_t itop)
    {
        PS2Runtime &runtime = *g_vu1.runtime;
        PS2Memory &memory = runtime.memory();
        VU1State &oracle = g_vu1.shadowVu1->state();
        std::memcpy(&oracle, g_vu1.shadow.get(), sizeof(VU1State));
        const R5900Context &context = guestContext(runtime);
        oracle.dBitEnabled = (context.vu0_fbrst & (1u << 10)) != 0u;
        oracle.tBitEnabled = (context.vu0_fbrst & (1u << 11)) != 0u;
        std::memcpy(g_vu1.shadowData.data(), g_vu1.inputData.data(), PS2_VU1_DATA_SIZE);
        g_vu1.oracleGif.beginLaunch();
        g_vu1.shadowRun = true;
        g_vu1.shadowVu1->execute(memory.getVU1Code(), PS2_VU1_CODE_SIZE, g_vu1.shadowData.data(), PS2_VU1_DATA_SIZE,
                                 runtime.gs(), &memory, entryPc, top, itop, 65536);
        g_vu1.shadowRun = false;
        ++g_vu1.stats.verified;
        const std::string difference = describeDifference(oracle, g_vu1.shadowData.data(), g_vu1.oracleGif,
                                                          runtime.vu1().state(), memory.getVU1Data(), g_vu1.host);
        if (!difference.empty())
            noteDifference(program, mscal, entryPc, difference, *g_vu1.shadow, g_vu1.inputData.data(), top, itop);
    }

    void verify(bool mscal, uint32_t startPc, uint32_t top, uint32_t itop)
    {
        PS2Runtime &runtime = *g_vu1.runtime;
        PS2Memory &memory = runtime.memory();
        VU1State &state = runtime.vu1().state();
        const uint32_t entryPc = mscal ? (startPc & 0x3FFFu) : (state.pc & 0x3FFFu);
        const Vu1nProgram *program = currentProgram(memory);
        const bool handled = program && program->handles(entryPc);
        if (handled)
        {
            std::memcpy(g_vu1.shadow.get(), &state, sizeof(VU1State));
            std::memcpy(g_vu1.inputData.data(), memory.getVU1Data(), PS2_VU1_DATA_SIZE);
        }

        // The interpreter, on the real state, as without the translation; its packets go to the GS and are copied.
        g_vu1.oracleGif.beginLaunch();
        g_vu1.capturing = handled;
        if (mscal)
            g_vu1.interpreterMscal(startPc, top, itop);
        else
            g_vu1.interpreterMscnt(top, itop);
        g_vu1.capturing = false;
        if (!handled)
        {
            ++g_vu1.stats.interpreted;
            report();
            return;
        }

        // The translation on the copy.
        const VU1State input = *g_vu1.shadow;
        std::memcpy(g_vu1.shadowData.data(), g_vu1.inputData.data(), PS2_VU1_DATA_SIZE);
        prepare(*g_vu1.shadow, entryPc, top, itop, guestContext(runtime));
        const Vu1nStatus status = vu1nRun(*program, *g_vu1.shadow, g_vu1.shadowData.data(), g_vu1.host, entryPc);
        ++g_vu1.stats.verified;
        std::string difference;
        if (status == Vu1nStatus::UnknownTarget)
        {
            ++g_vu1.stats.unknownTarget;
            --g_vu1.stats.verified;
            report();
            return;
        }
        difference = describeDifference(state, memory.getVU1Data(), g_vu1.oracleGif, *g_vu1.shadow,
                                        g_vu1.shadowData.data(), g_vu1.host);
        if (!difference.empty())
            noteDifference(*program, mscal, entryPc, difference, input, g_vu1.inputData.data(), top, itop);
        report();
    }
}

// Called by applyMksmOverrides (mksm_overrides.cpp).
void mksmInstallVu1(PS2Runtime &runtime)
{
    size_t count = 0;
    const Vu1nProgram *programs = vu1nPrograms(count);
    const char *setting = std::getenv("MKSM_VU1");
    const std::string value = setting ? setting : "1";
    if (value == "0" || count == 0u)
    {
        std::fprintf(stderr, "[mksm-vu1] VU1 interpreted (%s)\n", count == 0u ? "no translated program in this build" : "MKSM_VU1=0");
        return;
    }
    g_vu1.runtime = &runtime;
    g_vu1.mode = value == "verify" ? Vu1Mode::Verify : Vu1Mode::Translated;
    if (value.compare(0u, 7u, "verify:") == 0)
    {
        g_vu1.sampleEvery = static_cast<uint32_t>(std::strtoul(value.c_str() + 7, nullptr, 10));
        if (g_vu1.sampleEvery == 0u)
            g_vu1.sampleEvery = 1u;
        g_vu1.shadowVu1 = std::make_unique<VU1Interpreter>();
    }
    PS2Memory &memory = runtime.memory();
    g_vu1.interpreterMscal = memory.m_vu1MscalCallback;
    g_vu1.interpreterMscnt = memory.m_vu1MscntCallback;
    if (!g_vu1.interpreterMscal || !g_vu1.interpreterMscnt)
    {
        std::fprintf(stderr, "[mksm-vu1] the runtime's VU1 callbacks are not set: VU1 interpreted\n");
        return;
    }
    std::string names;
    for (size_t index = 0; index < count; ++index)
        names += (index != 0u ? ", " : "") + std::string(programs[index].name);

    if (g_vu1.mode == Vu1Mode::Verify || g_vu1.sampleEvery != 0u)
    {
        if (const char *folder = std::getenv("MKSM_VU1_DUMP"))
            g_vu1.dumpFolder = folder;
        // The interpreter's PATH1 packets, copied on their way to the GS (the body is upstream's arbiter function,
        // PS2Runtime::syncCoreSubsystems). The packets of the second interpreter (verify:<n>) are only copied.
        runtime.gifArbiter().setProcessPacketFn([&runtime](GifPathId path, const uint8_t *data, uint32_t size) {
            if ((g_vu1.capturing || g_vu1.shadowRun) && path == GifPathId::Path1)
            {
                const uint32_t offset = static_cast<uint32_t>(g_vu1.oracleGif.arena.size());
                g_vu1.oracleGif.arena.insert(g_vu1.oracleGif.arena.end(), data, data + size);
                g_vu1.oracleGif.kicks.push_back({offset, size});
                if (g_vu1.shadowRun)
                    return;
            }
            runtime.gs().processGIFPacket(data, size, static_cast<uint32_t>(path));
        });
    }
    if (g_vu1.mode == Vu1Mode::Verify)
    {
        memory.setVu1MscalCallback([](uint32_t startPc, uint32_t top, uint32_t itop) { verify(true, startPc, top, itop); });
        memory.setVu1MscntCallback([](uint32_t top, uint32_t itop) { verify(false, 0u, top, itop); });
        std::fprintf(stderr, "[mksm-vu1] verify: the interpreter runs every launch, the translation (%s) is compared%s\n",
                     names.c_str(), g_vu1.dumpFolder.empty() ? "" : "; differing launches are written to MKSM_VU1_DUMP");
        return;
    }
    if (g_vu1.sampleEvery != 0u)
        std::fprintf(stderr, "[mksm-vu1] verify:%u: the translation (%s) runs every launch, a second interpreter checks one in %u%s\n",
                     g_vu1.sampleEvery, names.c_str(), g_vu1.sampleEvery,
                     g_vu1.dumpFolder.empty() ? "" : "; differing launches are written to MKSM_VU1_DUMP");
    const char *statsSetting = std::getenv("MKSM_VU1_STATS");
    g_vu1.statsLines = statsSetting && *statsSetting != '0';
    memory.setVu1MscalCallback([](uint32_t startPc, uint32_t top, uint32_t itop) { launch(true, startPc, top, itop); });
    memory.setVu1MscntCallback([](uint32_t top, uint32_t itop) { launch(false, 0u, top, itop); });
    std::fprintf(stderr, "[mksm-vu1] VU1 programs translated: %s (MKSM_VU1=0 interprets, MKSM_VU1=verify compares)\n", names.c_str());
}
