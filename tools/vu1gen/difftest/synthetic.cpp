#include "synthetic.h"

#include "launch.h"

#include "vu_decode.h"

#include <cstdio>
#include <cstring>

namespace vu1diff
{
    namespace
    {
        struct Rng
        {
            uint64_t state;
            explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
            uint64_t next()
            {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                return state;
            }
            uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
            bool chance(uint32_t oneIn) { return below(oneIn) == 0u; }
        };

        // A word that holds a float of the normal range. Words with exponent 0 (small integers: counts, pointers,
        // flags, the low words of GIF tags) and 255 are left alone, so that loops keep their counts.
        bool floatLike(uint32_t word)
        {
            const uint32_t exponent = (word >> 23) & 0xFFu;
            return exponent != 0u && exponent != 0xFFu;
        }

        uint32_t mutateWord(Rng &rng, uint32_t word)
        {
            switch (rng.below(16))
            {
            case 0: // sign
                return word ^ 0x80000000u;
            case 1: // exponent moved by up to 3, kept in the normal range
            {
                int exponent = static_cast<int>((word >> 23) & 0xFFu) + static_cast<int>(rng.below(7)) - 3;
                exponent = exponent < 1 ? 1 : (exponent > 254 ? 254 : exponent);
                return (word & 0x807FFFFFu) | (static_cast<uint32_t>(exponent) << 23);
            }
            case 2: // a value from the classes the arithmetic treats apart
            {
                static constexpr uint32_t kSpecial[] = {0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800001u, 0x7F7FFFFFu,
                                                        0x00800000u, 0x80800001u, 0x00000001u, 0x3F800000u, 0xBF800000u,
                                                        0x1C800000u, 0x4B0000FFu};
                return kSpecial[rng.below(sizeof(kSpecial) / sizeof(kSpecial[0]))];
            }
            case 3: // a large change of the mantissa
                return word ^ (static_cast<uint32_t>(rng.next()) & 0x007FFFFFu);
            default: // a few units in the last place up or down
                return word + static_cast<uint32_t>(static_cast<int>(rng.below(65)) - 32);
            }
        }

        // One in oneIn float-like words of the state (VF01..VF31, ACC, Q, P, I) and of data memory is changed.
        void mutate(Rng &rng, VU1State &state, uint8_t *data, uint32_t oneIn)
        {
            const auto touch = [&](float &value) {
                uint32_t word;
                std::memcpy(&word, &value, 4u);
                if (floatLike(word) && rng.chance(oneIn))
                {
                    word = mutateWord(rng, word);
                    std::memcpy(&value, &word, 4u);
                }
            };
            for (uint32_t reg = 1; reg < 32u; ++reg)
                for (float &lane : state.vf[reg])
                    touch(lane);
            for (float &lane : state.acc)
                touch(lane);
            touch(state.q);
            touch(state.p);
            touch(state.i);
            for (uint32_t offset = 0; offset < kDataBytes; offset += 4u)
            {
                uint32_t word;
                std::memcpy(&word, data + offset, 4u);
                if (floatLike(word) && rng.chance(oneIn))
                {
                    word = mutateWord(rng, word);
                    std::memcpy(data + offset, &word, 4u);
                }
            }
        }

        // What a translated XGKICK does, done when the stepped oracle executes one: the packet read whole from data
        // memory as it is at the instruction.
        struct KickReader
        {
            std::vector<vu1gen::Pair> pairs;
            Vu1nHost host;
        };

        void readKick(void *context, uint32_t pc, const int32_t *viBefore, const uint8_t *data)
        {
            KickReader &reader = *static_cast<KickReader *>(context);
            const vu1gen::Instr &lower = reader.pairs[pc / 8u].lower;
            if (lower.kickReg >= 0)
                reader.host.kick(data, static_cast<uint16_t>(viBefore[lower.kickReg]));
        }

        void put32(uint8_t *p, uint32_t value)
        {
            for (int index = 0; index < 4; ++index)
                p[index] = static_cast<uint8_t>(value >> (8 * index));
        }

        // A record of the capture format (captures.h), as an MSCAL at the entry: vu1difftest --captures replays it.
        bool saveRecord(const std::string &path, uint64_t codeHash, uint64_t index, uint32_t entryPc, uint32_t top,
                        uint32_t itop, const VU1State &state, const uint8_t *data, std::string &error)
        {
            std::FILE *file = std::fopen(path.c_str(), "ab");
            if (!file)
            {
                error = "cannot write " + path;
                return false;
            }
            uint8_t header[48] = {'V', 'U', '1', 'L'};
            put32(header + 4, 1u);
            put32(header + 8, sizeof(VU1State));
            put32(header + 12, kDataBytes);
            put32(header + 16, entryPc);
            put32(header + 20, 0u);
            put32(header + 24, top);
            put32(header + 28, itop);
            put32(header + 32, static_cast<uint32_t>(codeHash));
            put32(header + 36, static_cast<uint32_t>(codeHash >> 32));
            put32(header + 40, static_cast<uint32_t>(index));
            put32(header + 44, static_cast<uint32_t>(index >> 32));
            const bool ok = std::fwrite(header, 1u, sizeof(header), file) == sizeof(header) &&
                            std::fwrite(&state, 1u, sizeof(VU1State), file) == sizeof(VU1State) &&
                            std::fwrite(data, 1u, kDataBytes, file) == kDataBytes;
            std::fclose(file);
            if (!ok)
                error = "cannot write " + path;
            return ok;
        }
    }

    bool runSynthetic(const SyntheticOptions &options, Oracle &oracle, const std::vector<std::unique_ptr<LaunchRecord>> &bases,
                      const Vu1nProgram *program, uint64_t codeHash, SyntheticReport &report, std::string &error)
    {
        if (bases.empty() || options.count == 0u)
            return true;

        // Every entry the translation handles; without a translation, the entries of the bases.
        std::vector<uint32_t> entries;
        if (program)
        {
            for (uint32_t pc = 0; pc < kCodeBytes; pc += 8u)
            {
                if (program->handles(pc))
                    entries.push_back(pc);
            }
        }
        if (entries.empty())
        {
            for (const auto &base : bases)
                entries.push_back(base->entryPc());
        }

        Rng rng(options.seed ^ codeHash);
        OracleMachine glue;
        OracleMachine stepper;
        LaunchResult expected;
        LaunchResult stepped;
        LaunchResult candidate;
        auto input = std::make_unique<LaunchRecord>();
        auto reader = std::make_unique<KickReader>();
        reader->pairs = vu1gen::decodeProgram(oracle.code(), kCodeBytes);

        for (uint64_t number = 0; number < options.count; ++number)
        {
            const LaunchRecord &base = *bases[rng.below(static_cast<uint32_t>(bases.size()))];
            std::memcpy(input.get(), &base, sizeof(LaunchRecord));
            // Three launches in four start where the base started, the fourth at the next entry in turn (entries the
            // captures never launched included).
            uint32_t entryPc = base.entryPc();
            if ((number & 3u) == 3u)
                entryPc = entries[(number / 4u) % entries.size()];
            static constexpr uint32_t kIntensity[] = {64u, 16u, 6u, 3u};
            mutate(rng, input->state, input->data, kIntensity[rng.below(4)]);
            input->startPc = entryPc;
            input->resume = false;

            SyntheticEntryStats &stats = report.entries[entryPc];
            ++stats.launches;

            // The oracle, entered at the entry as the glue enters the interpreter.
            std::memcpy(&glue.state(), &input->state, sizeof(VU1State));
            std::memcpy(expected.data.data(), input->data, kDataBytes);
            expected.packets.beginLaunch();
            const OracleRun run = oracle.run(glue, OracleEntry::Execute, entryPc, input->top, input->itop, expected.data.data(), expected.packets);
            std::memcpy(&expected.state, &glue.state(), sizeof(VU1State));
            if (run.end == OracleEnd::Budget)
            {
                ++stats.oracleBudget;
                continue;
            }
            if (run.end == OracleEnd::Error)
            {
                ++stats.oracleError;
                continue;
            }
            ++stats.oracleEnded;

            // Which pairs it executed: the oracle one pair a call (which must give the same result).
            std::memcpy(&stepper.state(), &input->state, sizeof(VU1State));
            std::memcpy(stepped.data.data(), input->data, kDataBytes);
            stepped.packets.beginLaunch();
            reader->host.beginLaunch();
            uint64_t pairs = 0;
            const OracleRun steppedRun = oracle.runStepping(stepper, entryPc, input->top, input->itop, stepped.data.data(), stepped.packets,
                                                            report.executed.data(), pairs, readKick, reader.get());
            std::memcpy(&stepped.state, &stepper.state(), sizeof(VU1State));
            if (steppedRun.end != run.end || compareLaunch(expected, stepped))
                ++report.steppedDifferent;

            if (!program || !program->handles(entryPc))
                continue;
            std::memcpy(&candidate.state, &input->state, sizeof(VU1State));
            prepareLaunch(candidate.state, entryPc, input->top, input->itop);
            std::memcpy(candidate.data.data(), input->data, kDataBytes);
            const Vu1nStatus status = vu1nRun(*program, candidate.state, candidate.data.data(), candidate.packets, entryPc);
            if (status == Vu1nStatus::UnknownTarget)
            {
                ++stats.unknownTarget;
                continue;
            }
            if (status != Vu1nStatus::Done)
                continue;
            ++stats.translated;
            ++stats.compared;
            if (comparePackets(expected.packets, reader->host))
            {
                // Fact 4 does not hold for this data: the reference for the packets is the read at the instruction.
                ++stats.transferDiffers;
                expected.packets = reader->host;
            }
            const Difference difference = compareLaunch(expected, candidate);
            if (!difference)
                continue;
            ++stats.mismatches;
            ++report.mismatchFields[difference.group];
            const uint64_t mismatches = [&] {
                uint64_t total = 0;
                for (const auto &entry : report.entries)
                    total += entry.second.mismatches;
                return total;
            }();
            if (mismatches <= options.maxReport)
                std::printf("   SYNTHETIC MISMATCH %llu at %04X: %s: oracle %s, translation %s\n",
                            static_cast<unsigned long long>(number), entryPc, difference.field.c_str(),
                            difference.expected.c_str(), difference.actual.c_str());
            if (!options.failureFile.empty() && report.saved < 100u)
            {
                if (!saveRecord(options.failureFile, codeHash, number, entryPc, input->top, input->itop, input->state, input->data, error))
                    return false;
                ++report.saved;
            }
        }
        return true;
    }
}
