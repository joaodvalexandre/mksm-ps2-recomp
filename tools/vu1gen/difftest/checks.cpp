#include "checks.h"

#include "images.h"
#include "vu1n_api.h"
#include "vu_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace vu1diff
{
    namespace
    {
        // splitmix64: plenty for test data, and the same sequence on every run.
        struct Random
        {
            uint64_t state;

            explicit Random(uint64_t seed) : state(seed) {}

            uint64_t next()
            {
                uint64_t z = (state += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return z ^ (z >> 31);
            }

            uint32_t below(uint32_t bound) { return static_cast<uint32_t>(next() % bound); }
        };

        constexpr uint32_t kUpperNop = 0x000002FFu;
        constexpr uint32_t kLowerNop = 0x8000033Cu;
        constexpr uint32_t kEBit = 0x40000000u;

        void putPair(uint8_t *code, uint32_t pc, uint32_t lower, uint32_t upper)
        {
            std::memcpy(code + pc, &lower, sizeof(lower));
            std::memcpy(code + pc + 4u, &upper, sizeof(upper));
        }

        // The first usage field of the interpreter's decoder that vu1gen's instruction does not repeat, or null.
        template <typename Usage>
        const char *usageDifference(const Usage &theirs, const vu1gen::Instr &mine)
        {
            if (theirs.vfReadCount != mine.vfReadCount)
                return "vfReadCount";
            for (uint32_t index = 0; index < 2u; ++index)
            {
                if (theirs.vfRead[index].reg != mine.vfRead[index].reg || theirs.vfRead[index].lanes != mine.vfRead[index].lanes)
                    return "vfRead";
            }
            if (theirs.vfWrite.reg != mine.vfWrite.reg || theirs.vfWrite.lanes != mine.vfWrite.lanes)
                return "vfWrite";
            if (theirs.viRead != mine.viRead)
                return "viRead";
            if (theirs.viWrite != mine.viWrite)
                return "viWrite";
            if (theirs.accRead != mine.accRead)
                return "accRead";
            if (theirs.accWrite != mine.accWrite)
                return "accWrite";
            if (theirs.latency != mine.latency)
                return "latency";
            if (theirs.vfLatency != mine.vfLatency)
                return "vfLatency";
            if (theirs.viLatency != mine.viLatency)
                return "viLatency";
            if (static_cast<uint32_t>(theirs.pipeline) != static_cast<uint32_t>(mine.pipeline))
                return "pipeline";
            if (theirs.waitQ != mine.waitQ)
                return "waitQ";
            if (theirs.waitP != mine.waitP)
                return "waitP";
            if (theirs.readsClip != mine.readsClipUsage)
                return "readsClip";
            if (theirs.writesClip != mine.writesClipUsage)
                return "writesClip";
            if (theirs.delaysNextBranchRead != mine.delaysNextBranchRead)
                return "delaysNextBranchRead";
            if (theirs.reserved != mine.reserved)
                return "reserved";
            return nullptr;
        }

        // Decodes a pair with the interpreter and says where vu1gen's pair differs: null, or the field, with the part
        // ("pair", "upper", "lower") in where.
        using PairComparison = const char *(*)(const VU1Interpreter &interpreter, const vu1gen::Pair &mine, const char *&where);
        PairComparison g_comparePair = nullptr;

        // The interpreter's decoder is private. An explicit template instantiation may name a private member, so
        // the member pointer comes in as a template argument and the code that uses it is reached through a
        // pointer of a public type: no change to upstream's header.
        template <auto DecodePair>
        struct DecoderAccess
        {
            static const char *compare(const VU1Interpreter &interpreter, const vu1gen::Pair &mine, const char *&where)
            {
                uint8_t code[8];
                std::memcpy(code, &mine.lowerWord, 4u);
                std::memcpy(code + 4, &mine.upperWord, 4u);
                const auto theirs = (interpreter.*DecodePair)(code, 0u);

                where = "pair";
                if (theirs.lower != mine.lowerWord || theirs.upper != mine.upperWord)
                    return "words";
                if (theirs.iBit != mine.iBit || theirs.eBit != mine.eBit || theirs.mBit != mine.mBit ||
                    theirs.dBit != mine.dBit || theirs.tBit != mine.tBit)
                    return "bits";
                if (theirs.upperVfShadowReg != mine.upperVfShadowReg)
                    return "upperVfShadowReg";
                if (theirs.suppressedLowerVf != mine.suppressedLowerVf)
                    return "suppressedLowerVf";
                where = "upper";
                if (const char *field = usageDifference(theirs.upperUsage, mine.upper))
                    return field;
                where = "lower";
                return usageDifference(theirs.lowerUsage, mine.lower);
            }

            static inline const bool registered = (g_comparePair = &compare, true);
        };

        template struct DecoderAccess<&VU1Interpreter::decodeInstructionPair>;

        struct DecodeChecker
        {
            VU1Interpreter interpreter;
            uint64_t pairs = 0;
            uint64_t mismatches = 0;

            void check(uint32_t lower, uint32_t upper)
            {
                const vu1gen::Pair mine = vu1gen::decodePair(0u, lower, upper);
                ++pairs;
                const char *where = nullptr;
                const char *field = g_comparePair(interpreter, mine, where);
                if (field && ++mismatches <= 10u)
                    std::printf("   DIFFERENT lower %08X upper %08X: %s %s\n", lower, upper, where, field);
            }
        };
    }

    bool runDecodeCheck(uint64_t randomCount, const std::vector<std::filesystem::path> &captureFiles)
    {
        auto checker = std::make_unique<DecodeChecker>();
        std::printf("decode check: vu1gen's decoder against VU1Interpreter::decodeInstructionPair\n");

        // Every upper word: bits 24..0 hold all its fields; the bits above are the pair's flags.
        uint64_t before = checker->pairs;
        for (uint32_t word = 0; word < (1u << 25); ++word)
            checker->check(0u, word);
        std::printf("   upper words, bits 24..0 exhaustive               %10llu pairs, %llu different so far\n",
                    static_cast<unsigned long long>(checker->pairs - before), static_cast<unsigned long long>(checker->mismatches));

        // Every word of the lower-1 group (the integer instructions and the lower special instructions).
        before = checker->pairs;
        for (uint32_t word = 0; word < (1u << 25); ++word)
            checker->check(0x80000000u | word, kUpperNop);
        std::printf("   lower-1 group, bits 24..0 exhaustive             %10llu pairs, %llu different so far\n",
                    static_cast<unsigned long long>(checker->pairs - before), static_cast<unsigned long long>(checker->mismatches));

        // The other lower opcodes: every lane mask and register field, a few immediates.
        before = checker->pairs;
        for (uint32_t opHi = 0; opHi < 0x80u; ++opHi)
        {
            if (opHi == 0x40u)
                continue;
            for (uint32_t fields = 0; fields < (1u << 14); ++fields)
            {
                for (const uint32_t imm : {0x000u, 0x001u, 0x3FFu, 0x400u, 0x7FFu})
                    checker->check((opHi << 25) | (fields << 11) | imm, kUpperNop);
            }
        }
        std::printf("   other lower opcodes, bits 24..11 exhaustive      %10llu pairs, %llu different so far\n",
                    static_cast<unsigned long long>(checker->pairs - before), static_cast<unsigned long long>(checker->mismatches));

        // Random pairs, for what depends on both words (the shadow register) and on the flag bits.
        before = checker->pairs;
        Random random(0x56553147454E0001ull);
        for (uint64_t index = 0; index < randomCount; ++index)
        {
            const uint64_t bits = random.next();
            uint32_t lower = static_cast<uint32_t>(bits);
            const uint32_t upper = static_cast<uint32_t>(bits >> 32);
            const uint32_t shape = random.below(4u);
            if (shape < 2u)
                lower = 0x80000000u | (lower & 0x01FFFFFFu); // the lower-1 group
            else if (shape == 2u)
                lower &= 0x7FFFFFFFu; // the opcodes with an immediate
            checker->check(lower, upper);
        }
        std::printf("   random pairs                                     %10llu pairs, %llu different so far\n",
                    static_cast<unsigned long long>(checker->pairs - before), static_cast<unsigned long long>(checker->mismatches));

        for (const std::filesystem::path &capture : captureFiles)
        {
            const std::string fileName = capture.filename().string();
            const std::string codeName = "code_" + fileName.substr(9u);
            std::vector<uint8_t> code;
            std::string error;
            if (!vu1gen::readFile((capture.parent_path() / codeName).string(), code, error))
            {
                std::printf("   %s\n", error.c_str());
                return false;
            }
            before = checker->pairs;
            for (size_t pc = 0; pc + 8u <= code.size(); pc += 8u)
            {
                uint32_t lower = 0;
                uint32_t upper = 0;
                std::memcpy(&lower, code.data() + pc, sizeof(lower));
                std::memcpy(&upper, code.data() + pc + 4u, sizeof(upper));
                checker->check(lower, upper);
            }
            std::printf("   %-48s %10llu pairs, %llu different so far\n", codeName.c_str(),
                        static_cast<unsigned long long>(checker->pairs - before), static_cast<unsigned long long>(checker->mismatches));
        }

        std::printf("decode check: %llu pairs, %llu different: %s\n", static_cast<unsigned long long>(checker->pairs),
                    static_cast<unsigned long long>(checker->mismatches), checker->mismatches == 0u ? "PASSED" : "FAILED");
        return checker->mismatches == 0u;
    }

    namespace
    {
        // The three ways a launch of the oracle can end must be told apart: the comparison only uses launches that
        // ended.
        bool checkOracleEnds(Oracle &oracle)
        {
            OracleMachine machine;
            std::vector<uint8_t> data(kDataBytes, 0u);
            Vu1nHost packets;
            std::vector<uint8_t> code(kCodeBytes, 0u);

            // The E bit and its delay slot.
            putPair(code.data(), 0u, kLowerNop, kUpperNop | kEBit);
            putPair(code.data(), 8u, kLowerNop, kUpperNop);
            oracle.setCode(code.data());
            const OracleRun ended = oracle.run(machine, OracleEntry::Execute, 0u, 0u, 0u, data.data(), packets);

            // "B 0x0000" at 0 and its delay slot: never ends.
            putPair(code.data(), 0u, 0x400007FFu, kUpperNop);
            oracle.setCode(code.data());
            const OracleRun looping = oracle.run(machine, OracleEntry::Execute, 0u, 0u, 0u, data.data(), packets);

            // A reserved lower instruction: an error line and a stop.
            putPair(code.data(), 0u, 0x70000000u, kUpperNop);
            oracle.setCode(code.data());
            const OracleRun reserved = oracle.run(machine, OracleEntry::Execute, 0u, 0u, 0u, data.data(), packets);

            const bool passed = ended.end == OracleEnd::Ended && ended.cycles == 2u &&
                                looping.end == OracleEnd::Budget && looping.cycles == kCycleBudget &&
                                reserved.end == OracleEnd::Error && reserved.cycles == 0u;
            std::printf("oracle ends: a program that ends (%llu cycles), one that never does (%llu cycles: the budget) and a reserved "
                        "instruction (%llu cycles, an error) are told apart: %s\n",
                        static_cast<unsigned long long>(ended.cycles), static_cast<unsigned long long>(looping.cycles),
                        static_cast<unsigned long long>(reserved.cycles), passed ? "PASSED" : "FAILED");
            return passed;
        }
    }

    bool runKickCheck(Oracle &oracle, uint64_t count)
    {
        // The interpreter writes a line for every packet it refuses, and for the reserved instruction below.
        std::streambuf *const errorStream = std::cerr.rdbuf(nullptr);
        const bool endsPassed = checkOracleEnds(oracle);

        // XGKICK vi01; then the E bit and its delay slot. The packet is transferred during the flush.
        std::vector<uint8_t> code(kCodeBytes, 0u);
        putPair(code.data(), 0u, 0x800006FCu | (1u << 11), kUpperNop);
        putPair(code.data(), 8u, kLowerNop, kUpperNop | kEBit);
        putPair(code.data(), 16u, kLowerNop, kUpperNop);
        oracle.setCode(code.data());

        OracleMachine machine;
        Random random(0x56553147454E0002ull);
        std::vector<uint8_t> data(kDataBytes);
        std::vector<uint8_t> image(kDataBytes);
        Vu1nHost theirs;
        Vu1nHost mine;

        uint64_t sent = 0;
        uint64_t wrapped = 0;
        uint64_t full = 0;
        uint64_t dropped = 0;
        uint64_t largest = 0;
        uint64_t mismatches = 0;

        for (uint64_t index = 0; index < count; ++index)
        {
            for (uint32_t offset = 0; offset < kDataBytes; offset += 8u)
            {
                const uint64_t bits = random.next();
                std::memcpy(data.data() + offset, &bits, sizeof(bits));
            }

            // A chain of GIF tags from a random qword. The data of a long chain may run over its own tags: the two
            // sides read the same memory whatever it holds.
            uint32_t qword = random.below(1024u);
            const int32_t vi = static_cast<int16_t>(qword | (random.below(64u) << 10));
            const auto putTag = [&](uint32_t nloop, uint32_t format, uint32_t nreg, bool eop)
            {
                uint64_t tag = random.next() & 0x03FFFFFFFFFF0000ull; // the bits no length depends on
                tag |= nloop & 0x7FFFu;
                tag |= eop ? 1ull << 15 : 0ull;
                tag |= static_cast<uint64_t>(format & 3u) << 58;
                tag |= static_cast<uint64_t>(nreg & 0xFu) << 60;
                std::memcpy(data.data() + (qword & 0x3FFu) * 16u, &tag, sizeof(tag));
                const uint32_t registers = (nreg & 0xFu) != 0u ? (nreg & 0xFu) : 16u;
                const uint32_t dataQwords = format == 0u ? nloop * registers : (format == 1u ? (nloop * registers + 1u) / 2u : nloop);
                qword += 1u + dataQwords;
            };
            const uint32_t shape = random.below(100u);
            if (shape < 70u)
            {
                const uint32_t tags = 1u + random.below(5u);
                for (uint32_t tag = 0; tag < tags; ++tag)
                {
                    const uint32_t format = random.below(4u);
                    const uint32_t nreg = random.below(16u);
                    const uint32_t registers = format < 2u ? (nreg != 0u ? nreg : 16u) : 1u;
                    putTag(random.below(64u / registers + 1u), format, nreg, tag + 1u == tags);
                }
            }
            else if (shape < 85u)
            {
                const uint32_t tags = 1u + random.below(6u);
                for (uint32_t tag = 0; tag < tags; ++tag)
                {
                    const uint32_t format = random.below(4u);
                    const uint32_t nreg = random.below(16u);
                    const uint32_t registers = format < 2u ? (nreg != 0u ? nreg : 16u) : 1u;
                    putTag(random.below(1500u / registers + 1u), format, nreg, tag + 1u == tags);
                }
            }
            else if (shape < 92u)
            {
                // At the limit: 4096 qwords in all, or one less, or one more; as one tag or as two.
                const uint32_t total = 4095u + random.below(3u);
                if (random.below(2u) == 0u)
                {
                    putTag(total - 1u, random.below(2u) * 2u, 1u, true);
                }
                else
                {
                    const uint32_t first = random.below(total - 2u);
                    putTag(first, 2u, 0u, false);
                    putTag(total - 2u - first, 0u, 1u, true);
                }
            }
            else
            {
                const uint32_t tags = 1u + random.below(3u);
                for (uint32_t tag = 0; tag < tags; ++tag)
                    putTag(random.below(8u), random.below(4u), 1u + random.below(4u), false);
            }

            image = data;
            machine.state().vi[1] = vi;
            theirs.beginLaunch();
            const OracleRun run = oracle.run(machine, OracleEntry::Execute, 0u, 0u, 0u, data.data(), theirs);

            mine.beginLaunch();
            const bool recorded = mine.kick(image.data(), static_cast<uint16_t>(vi));
            uint32_t bytes = 0;
            const bool sized = vu1nPacketBytes(image.data(), static_cast<uint16_t>(vi), bytes);

            const bool oracleSent = theirs.kicks.size() == 1u;
            bool same = data == image && recorded == oracleSent && sized == oracleSent && theirs.kicks.size() <= 1u &&
                        (run.end == OracleEnd::Error) == !oracleSent && mine.kicks.size() == (recorded ? 1u : 0u) &&
                        mine.dropped == (recorded ? 0u : 1u);
            if (same && oracleSent)
            {
                same = theirs.kicks[0].bytes == mine.kicks[0].bytes && bytes == mine.kicks[0].bytes && theirs.arena == mine.arena;
                ++sent;
                largest = std::max<uint64_t>(largest, theirs.kicks[0].bytes);
                wrapped += (static_cast<uint32_t>(vi) & 0x3FFu) * 16u + theirs.kicks[0].bytes > kDataBytes ? 1u : 0u;
                full += theirs.kicks[0].bytes == kVu1nMaxPacketBytes ? 1u : 0u;
            }
            else if (same)
            {
                ++dropped;
            }
            if (!same && ++mismatches <= 10u)
            {
                std::printf("   DIFFERENT case %llu: vi01 = %d: interpreter %s (%u bytes), kick %s (%u bytes)\n",
                            static_cast<unsigned long long>(index), vi,
                            oracleSent ? "sent a packet" : (run.end == OracleEnd::Error ? "reported an error" : "sent nothing"),
                            oracleSent ? theirs.kicks[0].bytes : 0u, recorded ? "recorded a packet" : "dropped it",
                            recorded ? mine.kicks[0].bytes : 0u);
            }
        }
        std::cerr.rdbuf(errorStream);
        std::cerr.clear();

        std::printf("kick check: %llu random packets: %llu sent by both with the same bytes (%llu wrap at 16 KB, %llu of exactly 64 KB, "
                    "the largest %llu bytes), %llu refused by both, %llu different: %s\n",
                    static_cast<unsigned long long>(count), static_cast<unsigned long long>(sent), static_cast<unsigned long long>(wrapped),
                    static_cast<unsigned long long>(full), static_cast<unsigned long long>(largest), static_cast<unsigned long long>(dropped),
                    static_cast<unsigned long long>(mismatches), mismatches == 0u ? "PASSED" : "FAILED");
        return endsPassed && mismatches == 0u;
    }
}
