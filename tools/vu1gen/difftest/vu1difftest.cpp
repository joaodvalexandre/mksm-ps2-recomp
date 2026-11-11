// vu1difftest: upstream's VU1 interpreter (the oracle) against the translated programs on captured launches.
//
//   vu1difftest --captures <folder> [--images <config/vu1_images.txt>] [--only <text>] [--limit <n>]
//               [--self | --self-fault <n>] [--bench] [--repeat <n>] [--coverage] [--max-report <n>]
//   vu1difftest --kick-check [<n>]
//   vu1difftest --decode-check [<n>] [--captures <folder>]
//
// --captures: the folder with launches_<hash>.bin and code_<hash>.bin (logs/vu1cap, see captures.h). For every
// record the oracle runs the launch the way the glue enters the interpreter (execute() at the entry pc, also for an
// MSCNT), and the tool reports, per image:
//   - the sanity of the harness: the oracle's state after a launch against the input of the next record;
//   - execute() at the saved pc against resume(), for the MSCNT records;
//   - the comparison of the translated launch with the oracle's, per entry point (launch.h has the rules).
// --images names the images in the reports. --only keeps the capture files whose name, or image name, contains the
// text. --limit stops after n records of each file.
// --self takes a second instance of the interpreter for the translation, through the same interface: it tests the
// machinery. --self-fault damages one result in every n-th launch on top of that; the run succeeds when exactly
// the damaged launches are reported, each with the damaged field.
// --bench measures the host time of every launch (best of --repeat runs, 3 by default).
// --coverage runs every launch a second time on the oracle one pair a call: it reports the pairs a launch executes,
// the distinct pairs the captures reach, and that the stepped launch gives the result of the launch run at once.
// --kick-check compares Vu1nHost::kick with the interpreter's XGKICK transfer on n random packets, after checking
// that the harness tells apart a launch that ended, one that ran out of cycles and one stopped by an error.
// --decode-check compares vu1gen's decoder with the interpreter's own on every encoding class, on n random pairs
// and on the code files of the captures.
//
// The exit code is 0 when no compared launch differs (and, for the checks, when they pass).
#include "captures.h"
#include "checks.h"
#include "launch.h"
#include "oracle.h"

#include "images.h"
#include "vu_decode.h"
#include "vu1n_api.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace vu1diff;

namespace
{
    struct Options
    {
        std::string captures;
        std::string imagesPath;
        std::string only;
        uint64_t limit = 0;
        bool self = false;
        uint32_t selfFault = 0;
        bool bench = false;
        bool coverage = false;
        uint32_t repeat = 3;
        uint32_t maxReport = 10;
        bool kickCheck = false;
        bool decodeCheck = false;
        uint64_t checkCount = 0;
    };

    struct EntryKey
    {
        bool resume;
        uint32_t pc;

        bool operator<(const EntryKey &other) const
        {
            return resume != other.resume ? !resume : pc < other.pc;
        }
    };

    std::string entryName(const EntryKey &key)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%s %04X", key.resume ? "MSCNT" : "MSCAL", key.pc);
        return text;
    }

    struct EntryStats
    {
        uint64_t launches = 0;
        uint64_t cycles = 0;
        uint64_t packets = 0; // PATH1 packets of the oracle
        uint64_t packetBytes = 0;
        uint64_t pairs = 0; // executed pairs (--coverage)
        uint64_t oracleBudget = 0;
        uint64_t oracleError = 0;
        uint64_t translated = 0;    // the translation ran the launch to its end
        uint64_t notHandled = 0;    // no translated program for the image, or the entry is not one of its entries
        uint64_t unknownTarget = 0; // the translation gave the launch up at an indirect jump
        uint64_t compared = 0;
        uint64_t mismatches = 0;
        uint64_t oracleTicks = 0;
        uint64_t candidateTicks = 0;
        uint64_t candidateTimed = 0;

        void add(const EntryStats &other)
        {
            launches += other.launches;
            cycles += other.cycles;
            packets += other.packets;
            packetBytes += other.packetBytes;
            pairs += other.pairs;
            oracleBudget += other.oracleBudget;
            oracleError += other.oracleError;
            translated += other.translated;
            notHandled += other.notHandled;
            unknownTarget += other.unknownTarget;
            compared += other.compared;
            mismatches += other.mismatches;
            oracleTicks += other.oracleTicks;
            candidateTicks += other.candidateTicks;
            candidateTimed += other.candidateTimed;
        }
    };

    using FieldCounts = std::map<std::string, uint64_t>;

    std::string fieldCountsText(const FieldCounts &counts)
    {
        std::vector<std::pair<std::string, uint64_t>> sorted(counts.begin(), counts.end());
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::string text;
        for (size_t index = 0; index < sorted.size() && index < 12u; ++index)
            text += (index != 0u ? ", " : "") + sorted[index].first + " " + std::to_string(sorted[index].second);
        if (sorted.size() > 12u)
            text += ", ...";
        return text;
    }

    struct SanityStats
    {
        uint64_t pairs = 0;         // consecutive records
        uint64_t equalCompared = 0; // glue-style oracle: the fields a translation is compared on
        uint64_t equalAll = 0;      // glue-style oracle: every field and the cycle count
        uint64_t equalUpstream = 0; // execute() / resume() as upstream runs them: every field and the cycle count
        // Of the pairs that differ: the captured clock advanced by more cycles than the launch took, so another
        // launch (of another program) ran between the two records.
        uint64_t otherLaunchBetween = 0;
        FieldCounts firstField;         // glue-style oracle: the first field that differs
        FieldCounts firstFieldUpstream;

        void add(const SanityStats &other)
        {
            pairs += other.pairs;
            equalCompared += other.equalCompared;
            equalAll += other.equalAll;
            equalUpstream += other.equalUpstream;
            otherLaunchBetween += other.otherLaunchBetween;
            for (const auto &entry : other.firstField)
                firstField[entry.first] += entry.second;
            for (const auto &entry : other.firstFieldUpstream)
                firstFieldUpstream[entry.first] += entry.second;
        }
    };

    struct ResumeStats
    {
        uint64_t launches = 0;
        uint64_t equal = 0;          // the compared fields, data memory and packets
        uint64_t equalEverything = 0; // and mac, status and the cycle count
        FieldCounts firstField;

        void add(const ResumeStats &other)
        {
            launches += other.launches;
            equal += other.equal;
            equalEverything += other.equalEverything;
            for (const auto &entry : other.firstField)
                firstField[entry.first] += entry.second;
        }
    };

    struct ImageReport
    {
        std::string name;
        uint64_t records = 0;
        uint64_t mscal = 0;
        uint64_t mscnt = 0;
        std::map<EntryKey, EntryStats> entries;
        SanityStats sanity;
        ResumeStats trueResume;       // resume() on the interpreter that ran the launch before
        ResumeStats resumeAfterReset; // resume() after the state was loaded and the scheduler reset
        uint64_t unflushedInputs = 0; // records that start with ebit, haltAfterDelaySlot or branchPending set
        uint64_t snapshotErrors = 0;  // UnknownTarget from an entry that does not ask for a snapshot
        uint64_t reportedMismatches = 0;
        FieldCounts mismatchFields;   // the first field that differs, over the mismatches of the translation
        // --coverage
        uint32_t ownPairs = 0; // of the image, when the image list names it
        std::vector<uint8_t> executed = std::vector<uint8_t>(kCodeBytes / 8u, uint8_t{0}); // by pair
        uint64_t steppedLaunches = 0;
        uint64_t steppedEqual = 0; // the launch run pair by pair gave the result of the launch run at once
        uint64_t kicksExecuted = 0; // XGKICKs of the stepped launches
        uint64_t kicksEqual = 0;    // the packet read whole at the instruction is the packet the interpreter sent
        uint64_t kickLaunchesDifferent = 0;
    };

    // ---- the interpreter as a "translated program": tests the machinery through the real interface ----

    struct SelfProgram
    {
        Oracle *oracle = nullptr;
        OracleMachine machine;
        std::vector<uint8_t> entryFlags = std::vector<uint8_t>(kCodeBytes / 8u, static_cast<uint8_t>(kVu1nEntry));
        Vu1nProgram program{};
        uint64_t ticks = 0;

        // --self-fault
        uint32_t faultEvery = 0;
        uint64_t launches = 0;
        uint64_t faults = 0;
        std::vector<uint64_t> faultsByKind;
        std::string expectedGroup; // of the launch just run: the group the comparison must name, "" for none
        bool faultIsCompared = false;
    };
    SelfProgram *g_self = nullptr;

    // The kinds of damage of --self-fault: one bit of each field of the state in turn, then a byte of data memory,
    // then the packets (a byte, the number of packets, the size of one).
    size_t faultKinds()
    {
        return stateFields().size() + 4u;
    }

    // Damages one thing in the result of a launch. Returns the group the comparison must name, or "" when the
    // damage is in a field that is not compared (vf00, vi00, mac, status, cycles, the inputs).
    std::string injectFault(uint64_t number, VU1State &state, uint8_t *data, Vu1nHost &host, size_t &kind)
    {
        const std::vector<StateField> &fields = stateFields();
        kind = static_cast<size_t>(number % faultKinds());
        if (kind > fields.size() && host.kicks.empty())
            kind = fields.size(); // no packet to damage in this launch: data memory instead
        if (kind < fields.size())
        {
            const StateField &field = fields[kind];
            reinterpret_cast<uint8_t *>(&state)[field.offset] ^= 1u;
            return (field.fieldClass & kFieldCompared) != 0u ? field.group : std::string();
        }
        switch (kind - fields.size())
        {
        case 0:
            data[(number * 97u) % kDataBytes] ^= 0x10u;
            return "data";
        case 1:
            host.arena[host.kicks.back().offset + host.kicks.back().bytes - 1u] ^= 0x80u;
            return "packets";
        case 2:
            host.arena.resize(host.kicks.back().offset);
            host.kicks.pop_back();
            return "packets";
        default:
            if (host.kicks.back().bytes >= 32u)
            {
                host.kicks.back().bytes -= 16u;
                host.arena.resize(host.arena.size() - 16u);
            }
            else
            {
                host.arena[host.kicks.back().offset] ^= 1u;
            }
            return "packets";
        }
    }

    Vu1nStatus selfRun(VU1State &state, uint8_t *data, Vu1nHost &host, uint32_t entryPc)
    {
        SelfProgram &self = *g_self;
        std::memcpy(&self.machine.state(), &state, sizeof(VU1State));
        const OracleRun run = self.oracle->run(self.machine, OracleEntry::Execute, entryPc, state.top, state.itop, data, host);
        std::memcpy(&state, &self.machine.state(), sizeof(VU1State));
        self.ticks = run.ticks;
        self.expectedGroup.clear();
        self.faultIsCompared = false;
        ++self.launches;
        if (self.faultEvery != 0u && self.launches % self.faultEvery == 0u)
        {
            size_t kind = 0;
            self.expectedGroup = injectFault(self.faults++, state, data, host, kind);
            ++self.faultsByKind[kind];
            self.faultIsCompared = !self.expectedGroup.empty();
            if (!self.faultIsCompared)
                self.expectedGroup = "(not compared)";
        }
        return Vu1nStatus::Done;
    }

    // --coverage: what a translated XGKICK will do, done at the moment the stepped oracle executes one. The packet
    // is read whole from data memory as it is at the instruction.
    struct KickObserver
    {
        std::vector<vu1gen::Pair> pairs; // the image, decoded
        Vu1nHost host;
    };

    void observeKick(void *context, uint32_t pc, const int32_t *viBefore, const uint8_t *data)
    {
        KickObserver &observer = *static_cast<KickObserver *>(context);
        const vu1gen::Instr &lower = observer.pairs[pc / 8u].lower;
        if (lower.kickReg >= 0)
            observer.host.kick(data, static_cast<uint16_t>(viBefore[lower.kickReg]));
    }

    struct FaultTally
    {
        uint64_t compared = 0;      // faults in compared fields
        uint64_t reported = 0;      // of those, reported with the damaged field
        uint64_t wrongField = 0;    // reported with another field
        uint64_t missed = 0;        // not reported
        uint64_t notCompared = 0;   // faults in fields that are not compared
        uint64_t falseAlarms = 0;   // differences where nothing was damaged
    };

    // ---- one capture file ----

    bool processFile(const Options &options, Oracle &oracle, const std::filesystem::path &path, const std::string &name,
                     SelfProgram *self, FaultTally &faults, ImageReport &report, std::string &error)
    {
        const std::string fileName = path.filename().string();
        const std::string hashText = fileName.substr(9u, fileName.size() - 13u); // launches_<hash>.bin
        std::vector<uint8_t> code;
        if (!vu1gen::readFile((path.parent_path() / ("code_" + hashText + ".bin")).string(), code, error))
            return false;
        if (code.size() != kCodeBytes)
        {
            error = "code_" + hashText + ".bin is not 16 KB";
            return false;
        }
        char computed[20];
        std::snprintf(computed, sizeof(computed), "%016llX", static_cast<unsigned long long>(vu1gen::imageHash(code.data(), code.size())));
        if (hashText != computed)
            std::printf("   warning: code_%s.bin has the hash %s\n", hashText.c_str(), computed);

        CaptureFile capture;
        if (!capture.open(path.string(), error))
            return false;

        oracle.setCode(code.data());
        const Vu1nProgram *program = self ? &self->program : vu1nFindProgram(code.data());
        report.name = name;

        OracleMachine glue;     // enters every launch with execute(): the oracle of the comparison
        OracleMachine upstream; // MSCAL execute(), MSCNT resume(): what upstream's runtime does
        OracleMachine stepper;  // --coverage: the launch one pair at a time
        auto kickObserver = std::make_unique<KickObserver>();
        kickObserver->pairs = vu1gen::decodeProgram(code.data(), code.size());
        auto record = std::make_unique<LaunchRecord>();
        LaunchResult glueResult;
        LaunchResult upstreamResult;
        LaunchResult candidate;
        LaunchResult scratch;
        Vu1nHost scratchPackets;

        bool havePrevious = false;
        uint64_t previousIndex = 0;
        uint64_t previousCycleStamp = 0; // state.cycles of the previous record
        uint64_t previousGlueCycles = 0;
        uint64_t previousUpstreamCycles = 0;
        VU1State previousGlueState{};
        VU1State previousUpstreamState{};

        uint64_t position = 0;
        while ((options.limit == 0u || position < options.limit) && capture.next(*record, error))
        {
            const LaunchRecord &rec = *record;
            const uint32_t entryPc = rec.entryPc();
            EntryStats &stats = report.entries[{rec.resume, entryPc}];
            ++stats.launches;
            ++report.records;
            ++(rec.resume ? report.mscnt : report.mscal);
            if (rec.state.ebit || rec.state.haltAfterDelaySlot || rec.state.branchPending)
                ++report.unflushedInputs;

            // The oracle, entered as the glue will enter the interpreter.
            std::memcpy(&glue.state(), &rec.state, sizeof(VU1State));
            std::memcpy(glueResult.data.data(), rec.data, kDataBytes);
            glueResult.packets.beginLaunch();
            const OracleRun glueRun = oracle.run(glue, OracleEntry::Execute, entryPc, rec.top, rec.itop, glueResult.data.data(), glueResult.packets);
            std::memcpy(&glueResult.state, &glue.state(), sizeof(VU1State));
            stats.cycles += glueRun.cycles;
            stats.packets += glueResult.packets.kicks.size();
            stats.packetBytes += glueResult.packets.arena.size();
            stats.oracleBudget += glueRun.end == OracleEnd::Budget ? 1u : 0u;
            stats.oracleError += glueRun.end == OracleEnd::Error ? 1u : 0u;

            // Which pairs the launch executes: the oracle again, one pair a call. It must give the same result.
            if (options.coverage)
            {
                std::memcpy(&stepper.state(), &rec.state, sizeof(VU1State));
                std::memcpy(scratch.data.data(), rec.data, kDataBytes);
                scratch.packets.beginLaunch();
                kickObserver->host.beginLaunch();
                uint64_t pairs = 0;
                const OracleRun stepped = oracle.runStepping(stepper, entryPc, rec.top, rec.itop, scratch.data.data(), scratch.packets,
                                                             report.executed.data(), pairs, observeKick, kickObserver.get());
                std::memcpy(&scratch.state, &stepper.state(), sizeof(VU1State));
                stats.pairs += pairs;
                ++report.steppedLaunches;
                if (stepped.end == glueRun.end && stepped.cycles == glueRun.cycles && !compareLaunch(glueResult, scratch) &&
                    !compareState(glueResult.state, scratch.state, kFieldFlags))
                {
                    ++report.steppedEqual;
                }

                // The packets read whole at each XGKICK against the packets the interpreter transferred, two cycles
                // a qword, while the program went on.
                const Vu1nHost &atInstruction = kickObserver->host;
                const Vu1nHost &sent = glueResult.packets;
                report.kicksExecuted += atInstruction.kicks.size() + atInstruction.dropped;
                for (size_t index = 0; index < atInstruction.kicks.size() && index < sent.kicks.size(); ++index)
                {
                    if (atInstruction.kicks[index].bytes == sent.kicks[index].bytes &&
                        std::memcmp(atInstruction.packet(index), sent.packet(index), sent.kicks[index].bytes) == 0)
                    {
                        ++report.kicksEqual;
                    }
                }
                const Difference kickDifference = comparePackets(sent, atInstruction);
                if (kickDifference)
                {
                    if (++report.kickLaunchesDifferent <= options.maxReport)
                    {
                        std::printf("   XGKICK record %llu (launch %llu) %s: %s: transferred %s, read at the instruction %s\n",
                                    static_cast<unsigned long long>(position), static_cast<unsigned long long>(rec.index),
                                    entryName({rec.resume, entryPc}).c_str(), kickDifference.field.c_str(),
                                    kickDifference.expected.c_str(), kickDifference.actual.c_str());
                    }
                }
            }

            // Sanity of the harness: this record's input is the state the previous launch left, when no other
            // launch came between them.
            bool chained = false;
            if (havePrevious && rec.index == previousIndex + 1u)
            {
                SanityStats &sanity = report.sanity;
                ++sanity.pairs;
                const uint64_t cycles = rec.state.cycles - previousCycleStamp;

                Difference difference = compareState(rec.state, previousGlueState, kFieldCompared);
                if (!difference)
                    ++sanity.equalCompared;
                if (!difference)
                    difference = compareState(rec.state, previousGlueState, kFieldAllButCycles);
                if (difference)
                    ++sanity.firstField[difference.group];
                else if (cycles != previousGlueCycles)
                    ++sanity.firstField["cycles"];
                else
                    ++sanity.equalAll;
                if ((difference || cycles != previousGlueCycles) && cycles > previousGlueCycles)
                    ++sanity.otherLaunchBetween;

                difference = compareState(rec.state, previousUpstreamState, kFieldAllButCycles);
                if (difference)
                    ++sanity.firstFieldUpstream[difference.group];
                else if (cycles != previousUpstreamCycles)
                    ++sanity.firstFieldUpstream["cycles"];
                else
                    ++sanity.equalUpstream;
                // The other interpreter then holds exactly the state of this record, private parts included.
                chained = !difference && cycles == previousUpstreamCycles;
            }

            // The launch as upstream's runtime runs it.
            std::memcpy(upstreamResult.data.data(), rec.data, kDataBytes);
            upstreamResult.packets.beginLaunch();
            OracleRun upstreamRun;
            if (!rec.resume)
            {
                std::memcpy(&upstream.state(), &rec.state, sizeof(VU1State));
                upstreamRun = oracle.run(upstream, OracleEntry::Execute, entryPc, rec.top, rec.itop, upstreamResult.data.data(), upstreamResult.packets);
            }
            else if (chained)
            {
                upstreamRun = oracle.run(upstream, OracleEntry::Resume, 0u, rec.top, rec.itop, upstreamResult.data.data(), upstreamResult.packets);
            }
            else
            {
                std::memcpy(&upstream.state(), &rec.state, sizeof(VU1State));
                upstreamRun = oracle.run(upstream, OracleEntry::ResumeAfterReset, 0u, rec.top, rec.itop, upstreamResult.data.data(), upstreamResult.packets);
            }
            std::memcpy(&upstreamResult.state, &upstream.state(), sizeof(VU1State));

            // execute() at the saved pc against resume().
            if (rec.resume)
            {
                ResumeStats &resume = chained ? report.trueResume : report.resumeAfterReset;
                ++resume.launches;
                Difference difference = compareLaunch(upstreamResult, glueResult);
                if (!difference)
                {
                    ++resume.equal;
                    difference = compareState(upstreamResult.state, glueResult.state, kFieldFlags);
                    if (!difference && upstreamRun.cycles != glueRun.cycles)
                    {
                        difference.any = true;
                        difference.group = "cycles";
                    }
                    if (!difference)
                        ++resume.equalEverything;
                }
                if (difference)
                    ++resume.firstField[difference.group];
            }

            // The translation.
            uint64_t candidateTicks = 0;
            bool candidateTimed = false;
            if (program && program->handles(entryPc))
            {
                std::memcpy(&candidate.state, &rec.state, sizeof(VU1State));
                prepareLaunch(candidate.state, entryPc, rec.top, rec.itop);
                std::memcpy(candidate.data.data(), rec.data, kDataBytes);
                const uint64_t start = ticksNow();
                const Vu1nStatus status = vu1nRun(*program, candidate.state, candidate.data.data(), candidate.packets, entryPc);
                candidateTicks = ticksNow() - start;
                if (status == Vu1nStatus::Done)
                {
                    ++stats.translated;
                    candidateTimed = true;
                    const bool faultExpected = self && !self->expectedGroup.empty();
                    if (glueRun.end == OracleEnd::Ended)
                    {
                        ++stats.compared;
                        const Difference difference = compareLaunch(glueResult, candidate);
                        if (difference)
                        {
                            ++stats.mismatches;
                            ++report.mismatchFields[difference.group];
                            if (report.reportedMismatches < options.maxReport)
                            {
                                ++report.reportedMismatches;
                                std::printf("   MISMATCH record %llu (launch %llu) %s: %s: oracle %s, translation %s\n",
                                            static_cast<unsigned long long>(position), static_cast<unsigned long long>(rec.index),
                                            entryName({rec.resume, entryPc}).c_str(), difference.field.c_str(),
                                            difference.expected.c_str(), difference.actual.c_str());
                            }
                        }
                        if (self && self->faultEvery != 0u)
                        {
                            if (faultExpected && self->faultIsCompared)
                            {
                                ++faults.compared;
                                if (!difference)
                                    ++faults.missed;
                                else if (difference.group == self->expectedGroup)
                                    ++faults.reported;
                                else
                                    ++faults.wrongField;
                            }
                            else
                            {
                                faults.notCompared += faultExpected ? 1u : 0u;
                                faults.falseAlarms += difference ? 1u : 0u;
                            }
                        }
                    }
                }
                else if (status == Vu1nStatus::UnknownTarget)
                {
                    // The glue restores its snapshot and interprets: the result is the oracle's by construction.
                    ++stats.unknownTarget;
                    if (!program->needsSnapshot(entryPc))
                        ++report.snapshotErrors;
                }
                else
                {
                    ++stats.notHandled;
                }
            }
            else
            {
                ++stats.notHandled;
            }

            if (options.bench)
            {
                uint64_t best = glueRun.ticks;
                for (uint32_t run = 1; run < options.repeat; ++run)
                {
                    std::memcpy(&glue.state(), &rec.state, sizeof(VU1State));
                    std::memcpy(scratch.data.data(), rec.data, kDataBytes);
                    scratchPackets.beginLaunch();
                    best = std::min(best, oracle.run(glue, OracleEntry::Execute, entryPc, rec.top, rec.itop, scratch.data.data(), scratchPackets).ticks);
                }
                stats.oracleTicks += best;
                if (candidateTimed)
                {
                    best = self ? self->ticks : candidateTicks;
                    for (uint32_t run = 1; run < options.repeat; ++run)
                    {
                        std::memcpy(&scratch.state, &rec.state, sizeof(VU1State));
                        prepareLaunch(scratch.state, entryPc, rec.top, rec.itop);
                        std::memcpy(scratch.data.data(), rec.data, kDataBytes);
                        const uint64_t start = ticksNow();
                        vu1nRun(*program, scratch.state, scratch.data.data(), scratchPackets, entryPc);
                        const uint64_t ticks = ticksNow() - start;
                        best = std::min(best, self ? self->ticks : ticks);
                    }
                    stats.candidateTicks += best;
                    ++stats.candidateTimed;
                }
            }

            havePrevious = true;
            previousIndex = rec.index;
            previousCycleStamp = rec.state.cycles;
            previousGlueCycles = glueRun.cycles;
            previousUpstreamCycles = upstreamRun.cycles;
            std::memcpy(&previousGlueState, &glueResult.state, sizeof(VU1State));
            std::memcpy(&previousUpstreamState, &upstreamResult.state, sizeof(VU1State));
            ++position;
        }
        return error.empty();
    }

    // ---- reports ----

    void printSanity(const SanityStats &sanity)
    {
        std::printf("   harness sanity: the state after a launch against the input of the next record\n");
        std::printf("      consecutive records compared                              %8llu\n", static_cast<unsigned long long>(sanity.pairs));
        std::printf("      oracle (execute at the entry pc): equal, compared fields  %8llu\n", static_cast<unsigned long long>(sanity.equalCompared));
        std::printf("      oracle: equal in every field and in the cycle count       %8llu\n", static_cast<unsigned long long>(sanity.equalAll));
        std::printf("      upstream's way (MSCNT by resume): every field and cycles  %8llu\n", static_cast<unsigned long long>(sanity.equalUpstream));
        if (sanity.pairs != sanity.equalAll)
        {
            std::printf("      pairs that differ                                         %8llu, %llu of them with another launch between the two records "
                        "(the captured clock advanced by more than the launch)\n",
                        static_cast<unsigned long long>(sanity.pairs - sanity.equalAll), static_cast<unsigned long long>(sanity.otherLaunchBetween));
        }
        if (!sanity.firstField.empty())
            std::printf("      first field that differs, oracle:   %s\n", fieldCountsText(sanity.firstField).c_str());
        if (!sanity.firstFieldUpstream.empty())
            std::printf("      first field that differs, upstream: %s\n", fieldCountsText(sanity.firstFieldUpstream).c_str());
    }

    void printResume(const ResumeStats &trueResume, const ResumeStats &afterReset)
    {
        std::printf("   MSCNT: execute(startPC = state.pc) against resume()\n");
        std::printf("      resume() on the interpreter that ran the launch before   %8llu launches, %llu equal (state, data, packets), %llu also in mac, status, cycles\n",
                    static_cast<unsigned long long>(trueResume.launches), static_cast<unsigned long long>(trueResume.equal),
                    static_cast<unsigned long long>(trueResume.equalEverything));
        if (!trueResume.firstField.empty())
            std::printf("         first field that differs: %s\n", fieldCountsText(trueResume.firstField).c_str());
        std::printf("      resume() after loading the state (scheduler reset)       %8llu launches, %llu equal (state, data, packets), %llu also in mac, status, cycles\n",
                    static_cast<unsigned long long>(afterReset.launches), static_cast<unsigned long long>(afterReset.equal),
                    static_cast<unsigned long long>(afterReset.equalEverything));
        if (!afterReset.firstField.empty())
            std::printf("         first field that differs: %s\n", fieldCountsText(afterReset.firstField).c_str());
    }

    void printEntryHeader(const Options &options)
    {
        std::printf("   %-11s %8s %9s %8s %9s %10s %8s %10s %11s %8s", "entry", "launches", "cycles/l.", "kicks/l.", "qwords/l.", "translated",
                    "compared", "mismatches", "not handled", "unk.tgt.");
        if (options.coverage)
            std::printf(" | %8s", "pairs/l.");
        if (options.bench)
            std::printf(" | %12s %8s %13s %8s %8s", "oracle us/l.", "ns/cycle", "transl. us/l.", "ns/cycle", "speed-up");
        std::printf("\n");
    }

    void printEntryRow(const Options &options, const std::string &name, const EntryStats &stats)
    {
        const double launches = stats.launches != 0u ? static_cast<double>(stats.launches) : 1.0;
        std::printf("   %-11s %8llu %9.1f %8.2f %9.1f %10llu %8llu %10llu %11llu %8llu", name.c_str(), static_cast<unsigned long long>(stats.launches),
                    static_cast<double>(stats.cycles) / launches, static_cast<double>(stats.packets) / launches,
                    static_cast<double>(stats.packetBytes) / 16.0 / launches, static_cast<unsigned long long>(stats.translated),
                    static_cast<unsigned long long>(stats.compared), static_cast<unsigned long long>(stats.mismatches),
                    static_cast<unsigned long long>(stats.notHandled), static_cast<unsigned long long>(stats.unknownTarget));
        if (options.coverage)
            std::printf(" | %8.1f", static_cast<double>(stats.pairs) / launches);
        if (options.bench)
        {
            const double oracleNs = static_cast<double>(stats.oracleTicks) * nanosecondsPerTick();
            std::printf(" | %12.2f %8.1f", oracleNs / 1000.0 / launches, stats.cycles != 0u ? oracleNs / static_cast<double>(stats.cycles) : 0.0);
            if (stats.candidateTimed != 0u && stats.candidateTimed == stats.launches)
            {
                const double candidateNs = static_cast<double>(stats.candidateTicks) * nanosecondsPerTick();
                std::printf(" %13.2f %8.1f %8.1f", candidateNs / 1000.0 / launches,
                            stats.cycles != 0u ? candidateNs / static_cast<double>(stats.cycles) : 0.0,
                            candidateNs > 0.0 ? oracleNs / candidateNs : 0.0);
            }
            else
            {
                std::printf(" %13s %8s %8s", "-", "-", "-");
            }
        }
        std::printf("\n");
    }

    void printEntries(const Options &options, const std::map<EntryKey, EntryStats> &entries)
    {
        printEntryHeader(options);
        EntryStats total;
        for (const auto &entry : entries)
        {
            printEntryRow(options, entryName(entry.first), entry.second);
            total.add(entry.second);
        }
        printEntryRow(options, "all", total);
        if (total.oracleBudget != 0u || total.oracleError != 0u)
        {
            std::printf("   oracle launches that did not end: %llu out of cycles, %llu stopped by an error (not compared)\n",
                        static_cast<unsigned long long>(total.oracleBudget), static_cast<unsigned long long>(total.oracleError));
        }
    }

    int usage()
    {
        std::fprintf(stderr,
                     "usage: vu1difftest --captures <folder> [--images <config/vu1_images.txt>] [--only <text>] [--limit <n>]\n"
                     "                   [--self | --self-fault <n>] [--bench] [--repeat <n>] [--coverage] [--max-report <n>]\n"
                     "       vu1difftest --kick-check [<n>]\n"
                     "       vu1difftest --decode-check [<n>] [--captures <folder>]\n");
        return 2;
    }

    bool isNumber(const char *text)
    {
        return text && *text != '\0' && std::strspn(text, "0123456789") == std::strlen(text);
    }
}

int main(int argc, char **argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const char *value = index + 1 < argc ? argv[index + 1] : nullptr;
        if (argument == "--captures" && value)
            options.captures = argv[++index];
        else if (argument == "--images" && value)
            options.imagesPath = argv[++index];
        else if (argument == "--only" && value)
            options.only = argv[++index];
        else if (argument == "--limit" && isNumber(value))
            options.limit = std::strtoull(argv[++index], nullptr, 10);
        else if (argument == "--self")
            options.self = true;
        else if (argument == "--self-fault" && isNumber(value))
        {
            options.self = true;
            options.selfFault = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 10));
        }
        else if (argument == "--bench")
            options.bench = true;
        else if (argument == "--coverage")
            options.coverage = true;
        else if (argument == "--repeat" && isNumber(value))
            options.repeat = std::max(1u, static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 10)));
        else if (argument == "--max-report" && isNumber(value))
            options.maxReport = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 10));
        else if (argument == "--kick-check" || argument == "--decode-check")
        {
            (argument == "--kick-check" ? options.kickCheck : options.decodeCheck) = true;
            if (isNumber(value))
                options.checkCount = std::strtoull(argv[++index], nullptr, 10);
        }
        else
        {
            std::fprintf(stderr, "vu1difftest: unknown or incomplete argument '%s'\n", argument.c_str());
            return usage();
        }
    }
    if (options.selfFault != 0u)
        options.bench = false; // the damaged launches are not worth timing

    std::string error;
    Oracle oracle;
    if (!oracle.initialize(error))
    {
        std::fprintf(stderr, "vu1difftest: %s\n", error.c_str());
        return 1;
    }

    if (options.kickCheck)
        return runKickCheck(oracle, options.checkCount != 0u ? options.checkCount : 20000u) ? 0 : 1;

    // The capture files, in name order.
    std::vector<std::filesystem::path> files;
    if (!options.captures.empty())
    {
        std::error_code code;
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(options.captures, code))
        {
            const std::string name = entry.path().filename().string();
            if (name.size() > 13u && name.compare(0u, 9u, "launches_") == 0 && name.compare(name.size() - 4u, 4u, ".bin") == 0)
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        if (files.empty())
        {
            std::fprintf(stderr, "vu1difftest: no launches_<hash>.bin in %s\n", options.captures.c_str());
            return 1;
        }
    }

    if (options.decodeCheck)
        return runDecodeCheck(options.checkCount != 0u ? options.checkCount : 20000000u, files) ? 0 : 1;
    if (files.empty())
        return usage();

    vu1gen::ImagesConfig images;
    if (!options.imagesPath.empty() && !vu1gen::loadImagesConfig(options.imagesPath, images, error))
    {
        std::fprintf(stderr, "vu1difftest: %s\n", error.c_str());
        return 1;
    }

    SelfProgram self;
    if (options.self)
    {
        self.oracle = &oracle;
        self.faultEvery = options.selfFault;
        self.faultsByKind.assign(faultKinds(), 0u);
        self.program.name = "self";
        self.program.image = oracle.code();
        self.program.entryFlags = self.entryFlags.data();
        self.program.mxcsr = 0x7F80u; // round toward zero and nothing else: the interpreter's own environment
        self.program.run = selfRun;
        g_self = &self;
    }

    size_t programCount = 0;
    vu1nPrograms(programCount);
    std::printf("vu1difftest: %s; oracle = upstream's VU1Interpreter, %u cycles a launch\n",
                options.self ? (options.selfFault != 0u ? "the translation is a second interpreter with injected faults (--self-fault)"
                                                        : "the translation is a second interpreter (--self)")
                             : (programCount != 0u ? "translated programs linked" : "no translated program in this build: oracle side only"),
                kCycleBudget);
    if (options.bench)
        std::printf("bench: best of %u run(s) of each launch; the oracle's time is execute() alone, its packets go to a memory callback; 1 tick = %.4f ns\n",
                    options.repeat, nanosecondsPerTick());

    FaultTally faults;
    SanityStats sanityTotal;
    ResumeStats trueResumeTotal;
    ResumeStats afterResetTotal;
    EntryStats grandTotal;
    uint64_t snapshotErrors = 0;
    uint64_t steppingErrors = 0;
    for (const std::filesystem::path &path : files)
    {
        const std::string fileName = path.filename().string();
        const std::string hashText = fileName.substr(9u, fileName.size() - 13u);
        std::string name = hashText;
        const vu1gen::ImageConfig *image = images.findByHash(std::strtoull(hashText.c_str(), nullptr, 16));
        if (image)
            name = image->name + " " + hashText;
        if (!options.only.empty() && name.find(options.only) == std::string::npos && fileName.find(options.only) == std::string::npos)
            continue;

        std::printf("\n== %s ==\n", name.c_str());
        ImageReport report;
        report.ownPairs = image ? image->ownPairs : 0u;
        if (!processFile(options, oracle, path, name, options.self ? &self : nullptr, faults, report, error))
        {
            std::fprintf(stderr, "vu1difftest: %s: %s\n", fileName.c_str(), error.c_str());
            return 1;
        }
        std::printf("   %llu launches (%llu MSCAL, %llu MSCNT); %llu start with ebit, haltAfterDelaySlot or branchPending set\n",
                    static_cast<unsigned long long>(report.records), static_cast<unsigned long long>(report.mscal),
                    static_cast<unsigned long long>(report.mscnt), static_cast<unsigned long long>(report.unflushedInputs));
        printSanity(report.sanity);
        if (report.mscnt != 0u)
            printResume(report.trueResume, report.resumeAfterReset);
        printEntries(options, report.entries);
        if (!report.mismatchFields.empty())
            std::printf("   first field that differs, over the mismatches: %s\n", fieldCountsText(report.mismatchFields).c_str());
        if (report.snapshotErrors != 0u)
            std::printf("   ERROR: %llu launches ended with UnknownTarget from an entry that asks for no snapshot\n",
                        static_cast<unsigned long long>(report.snapshotErrors));
        if (options.coverage)
        {
            uint32_t executed = 0;
            uint32_t executedOwn = 0;
            for (uint32_t pair = 0; pair < report.executed.size(); ++pair)
            {
                executed += report.executed[pair];
                executedOwn += pair < report.ownPairs ? report.executed[pair] : 0u;
            }
            std::printf("   coverage: %u distinct pairs executed", executed);
            if (report.ownPairs != 0u)
                std::printf(" (%u of the image's %u own pairs)", executedOwn, report.ownPairs);
            std::printf("; run one pair a call, the oracle gives the result of the launch run at once in %llu of %llu launches\n",
                        static_cast<unsigned long long>(report.steppedEqual), static_cast<unsigned long long>(report.steppedLaunches));
            std::printf("   XGKICK: %llu executed; the packet read whole at the instruction is the packet the interpreter transferred "
                        "for %llu of them (%llu launches differ)\n",
                        static_cast<unsigned long long>(report.kicksExecuted), static_cast<unsigned long long>(report.kicksEqual),
                        static_cast<unsigned long long>(report.kickLaunchesDifferent));
            steppingErrors += report.steppedLaunches - report.steppedEqual + report.kickLaunchesDifferent;
        }

        sanityTotal.add(report.sanity);
        trueResumeTotal.add(report.trueResume);
        afterResetTotal.add(report.resumeAfterReset);
        for (const auto &entry : report.entries)
            grandTotal.add(entry.second);
        snapshotErrors += report.snapshotErrors;
    }

    std::printf("\n== all images ==\n");
    printSanity(sanityTotal);
    printResume(trueResumeTotal, afterResetTotal);
    printEntryHeader(options);
    printEntryRow(options, "all", grandTotal);

    if (options.selfFault != 0u)
    {
        const bool passed = faults.compared != 0u && faults.reported == faults.compared && faults.wrongField == 0u &&
                            faults.missed == 0u && faults.falseAlarms == 0u && grandTotal.mismatches == faults.compared;
        const size_t kindsUsed = static_cast<size_t>(std::count_if(self.faultsByKind.begin(), self.faultsByKind.end(), [](uint64_t count) { return count != 0u; }));
        std::printf("   injected faults: %llu in compared fields (%llu reported with the damaged field, %llu with another, %llu missed), "
                    "%llu in fields that are not compared; differences without a fault: %llu\n",
                    static_cast<unsigned long long>(faults.compared), static_cast<unsigned long long>(faults.reported),
                    static_cast<unsigned long long>(faults.wrongField), static_cast<unsigned long long>(faults.missed),
                    static_cast<unsigned long long>(faults.notCompared), static_cast<unsigned long long>(faults.falseAlarms));
        std::printf("   kinds of damage used: %llu of %llu (each field of the state, data memory, a packet's bytes, the packet count, a packet's size)\n",
                    static_cast<unsigned long long>(kindsUsed), static_cast<unsigned long long>(faultKinds()));
        std::printf("   comparison machinery: %s\n", passed ? "PASSED" : "FAILED");
        return passed ? 0 : 1;
    }
    return grandTotal.mismatches == 0u && snapshotErrors == 0u && steppingErrors == 0u ? 0 : 1;
}
