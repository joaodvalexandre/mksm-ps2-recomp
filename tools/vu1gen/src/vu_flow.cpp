#include "vu_flow.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace vu1gen
{
    namespace
    {
        constexpr uint32_t kMaxStatesPerLaunch = 4000000u;
        constexpr uint8_t kMaxFlagEntries = 8u; // the interpreter's flag pipeline (MAC and clip entries together)
        constexpr uint8_t kMaxEfuEntries = 2u;

        // The timing state at the top of the interpreter's loop for the pair at pc. Every time is relative to the
        // cycle the pair would issue without a stall ("remaining" cycles until a result is visible). Plain bytes, so
        // that the whole struct is the key of the visited set: every unused slot stays zero.
        struct TState
        {
            uint32_t pc;
            int32_t pend; // target of the branch executed in the previous pair, -1: none
            int32_t link; // return address of the last BAL / JALR, -1: none
            uint8_t ebit; // the previous pair had the E bit: this pair is the last
            uint8_t xg;   // an XGKICK transfer may still be active
            uint8_t backupReg; // VI register a branch would read the old value of (delaysNextBranchRead)
            uint8_t fdiv;      // division in flight
            uint8_t efures;    // cycles until the EFU accepts an instruction
            uint8_t nefu, nmac, nclip;
            uint8_t efu[kMaxEfuEntries];
            uint8_t mac[kMaxFlagEntries];  // in issue order
            uint8_t clip[kMaxFlagEntries]; // in issue order
            uint8_t vf[32 * 4];            // per register lane
            uint8_t vi[16];
        };

        std::string keyOf(const TState &s)
        {
            return std::string(reinterpret_cast<const char *>(&s), sizeof(s));
        }

        TState startState(const LaunchStart &start)
        {
            TState s;
            std::memset(&s, 0, sizeof(s));
            s.pc = start.pc;
            s.pend = -1;
            s.link = start.link;
            s.backupReg = start.backupReg;
            return s;
        }

        uint8_t sub(uint8_t value, uint32_t n)
        {
            return value > n ? static_cast<uint8_t>(value - n) : uint8_t{0};
        }

        // Drops the entries that become visible within n cycles from a queue kept in issue order.
        void advanceQueue(uint8_t *queue, uint8_t &count, uint32_t n)
        {
            uint8_t kept = 0;
            for (uint8_t index = 0; index < count; ++index)
            {
                if (queue[index] > n)
                    queue[kept++] = static_cast<uint8_t>(queue[index] - n);
            }
            for (uint8_t index = kept; index < count; ++index)
                queue[index] = 0;
            count = kept;
        }

        void advance(TState &s, uint32_t n)
        {
            if (n == 0u)
                return;
            for (uint8_t &value : s.vf)
                value = sub(value, n);
            for (uint8_t &value : s.vi)
                value = sub(value, n);
            s.fdiv = sub(s.fdiv, n);
            s.efures = sub(s.efures, n);
            advanceQueue(s.efu, s.nefu, n);
            advanceQueue(s.mac, s.nmac, n);
            advanceQueue(s.clip, s.nclip, n);
        }

        uint32_t maxPending(const TState &s)
        {
            uint32_t m = std::max<uint32_t>(s.fdiv, s.efures);
            for (uint8_t value : s.vf)
                m = std::max<uint32_t>(m, value);
            for (uint8_t value : s.vi)
                m = std::max<uint32_t>(m, value);
            for (uint8_t index = 0; index < s.nefu; ++index)
                m = std::max<uint32_t>(m, s.efu[index]);
            for (uint8_t index = 0; index < s.nmac; ++index)
                m = std::max<uint32_t>(m, s.mac[index]);
            for (uint8_t index = 0; index < s.nclip; ++index)
                m = std::max<uint32_t>(m, s.clip[index]);
            return m;
        }

        // calculatePairReadyCycle without the XGKICK rule.
        uint32_t staticStall(const TState &s, const Pair &pair)
        {
            uint32_t stall = 0;
            for (const Instr *instr : {&pair.upper, &pair.lower})
            {
                for (uint32_t index = 0; index < instr->vfReadCount; ++index)
                {
                    const VfAccess &access = instr->vfRead[index];
                    for (uint32_t component = 0; component < 4u; ++component)
                    {
                        if ((access.lanes & laneOfComponent(component)) != 0u)
                            stall = std::max<uint32_t>(stall, s.vf[access.reg * 4u + component]);
                    }
                }
                for (uint32_t reg = 1; reg < 16u; ++reg)
                {
                    if ((instr->viRead & (1u << reg)) != 0u)
                        stall = std::max<uint32_t>(stall, s.vi[reg]);
                }
                // ACC is written with a latency of one cycle: a pair that reads it never waits.
            }
            const Instr &lower = pair.lower;
            if ((lower.pipeline == Pipeline::Fdiv || lower.waitQ) && s.fdiv != 0u)
                stall = std::max<uint32_t>(stall, s.fdiv);
            if (lower.pipeline == Pipeline::Efu)
                stall = std::max<uint32_t>(stall, s.efures);
            if (lower.waitP)
            {
                for (uint8_t index = 0; index < s.nefu; ++index)
                    stall = std::max<uint32_t>(stall, s.efu[index]);
            }
            return stall;
        }

        uint32_t branchTarget(const Pair &pair)
        {
            return (pair.pc + 8u + static_cast<uint32_t>(pair.lower.imm * 8)) & kPcMask;
        }

        // The VF register an upper instruction writes in place when its destination is VF00 (core:41-49): the
        // usage decoder does not declare it, and the lower of the same pair would read the clobbered value.
        bool upperWritesVf0(const Instr &upper, uint8_t &lanes)
        {
            lanes = upper.dest;
            if (upper.dest == 0u)
                return false;
            switch (upper.op)
            {
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Madd:
            case Op::Msub:
            case Op::Max:
            case Op::Mini:
            case Op::Opmsub:
                return !upper.toAcc && upper.fd == 0u;
            case Op::Itof:
            case Op::Ftoi:
            case Op::Abs:
                return upper.ft == 0u;
            default:
                return false;
            }
        }

        // The refusals that depend on the pair alone (DESIGN.md, fact 5).
        void staticRefusals(const Pair &pair, std::vector<std::string> &out)
        {
            if (pair.dBit || pair.tBit)
                out.push_back("D or T bit");
            const Instr &up = pair.upper;
            const Instr &lo = pair.lower;
            if ((up.op == Op::Opmula || up.op == Op::Opmsub) && up.dest != kLanesXyz)
                out.push_back("OPMULA / OPMSUB with a mask other than xyz");
            uint8_t vf0Lanes = 0;
            if (upperWritesVf0(up, vf0Lanes))
            {
                for (uint32_t index = 0; index < lo.vfReadCount; ++index)
                {
                    if (lo.vfRead[index].reg == 0u && (lo.vfRead[index].lanes & vf0Lanes) != 0u)
                        out.push_back("upper writes VF00 lanes the lower reads");
                }
            }
            if (lo.readsStatus || lo.writesSticky || lo.op == Op::Fseq || lo.op == Op::Fsand || lo.op == Op::Fsor ||
                lo.op == Op::Fsset)
                out.push_back("status flag instruction");
            if (pair.eBit && lo.branch)
                out.push_back("E bit on a branch pair");
        }

        void addRefusal(PairFlow &flow, const std::string &reason)
        {
            if (std::find(flow.refusals.begin(), flow.refusals.end(), reason) == flow.refusals.end())
                flow.refusals.push_back(reason);
        }

        class Explorer
        {
        public:
            Explorer(ProgramFlow &program, uint32_t ownEnd) : m_program(program), m_ownEnd(ownEnd) {}

            // Explores every state a launch from start can reach.
            bool run(LaunchFlow &launch, std::set<LaunchStart> &continuations, std::string &error)
            {
                std::unordered_set<std::string> seen;
                std::vector<TState> work{startState(launch.start)};
                while (!work.empty())
                {
                    const TState s = work.back();
                    work.pop_back();
                    if (!seen.insert(keyOf(s)).second)
                        continue;
                    if (seen.size() > kMaxStatesPerLaunch)
                    {
                        char text[96];
                        std::snprintf(text, sizeof(text), "state explosion in the launch from 0x%04X", launch.start.pc);
                        error = text;
                        return false;
                    }
                    step(s, launch, continuations, work);
                }
                launch.states = seen.size();
                return true;
            }

        private:
            ProgramFlow &m_program;
            uint32_t m_ownEnd;

            void step(const TState &s0, LaunchFlow &launch, std::set<LaunchStart> &continuations, std::vector<TState> &work)
            {
                const Pair &pair = m_program.pairs[s0.pc / 8u];
                PairFlow &flow = m_program.flow[s0.pc / 8u];
                launch.pcs.insert(s0.pc);
                const uint8_t context = s0.ebit ? kContextFinal : (s0.pend >= 0 ? kContextDelaySlot : kContextNormal);
                flow.contexts |= context;
                launch.contexts[s0.pc] |= context;
                if (pair.reserved || s0.pc >= m_ownEnd)
                {
                    addRefusal(flow, pair.reserved ? "reserved instruction" : "outside the image's own code");
                    return;
                }
                const Instr &up = pair.upper;
                const Instr &lo = pair.lower;
                staticRefusals(pair, flow.refusals);
                if (lo.branch && s0.pend >= 0)
                    addRefusal(flow, "branch in a delay slot");
                if (pair.eBit && s0.pend >= 0)
                    addRefusal(flow, "E bit in a delay slot");
                if (s0.ebit && lo.branch)
                    addRefusal(flow, "branch in the pair after an E bit");
                if (lo.op == Op::Jr || lo.op == Op::Jalr)
                    launch.indirect = true;

                // An XGKICK that finds a transfer possibly still active waits for it: every length of that wait
                // up to the point where nothing is pending any more.
                const uint32_t stall = staticStall(s0, pair);
                uint32_t extraMax = 0;
                if (lo.pipeline == Pipeline::Xgkick && s0.xg)
                {
                    const uint32_t pending = maxPending(s0);
                    extraMax = (pending > stall ? pending - stall : 0u) + 1u;
                }
                for (uint32_t extra = 0; extra <= extraMax; ++extra)
                {
                    TState s = s0;
                    advance(s, stall + extra);
                    flow.stall.insert(static_cast<uint8_t>(std::min<uint32_t>(stall + extra, 255u)));

                    // ---- what the pair reads at execution
                    if (up.readsQ)
                        flow.qClass.insert(s.fdiv != 0u ? 1u : 0u);
                    if (lo.readsP)
                        flow.pClass.insert(s.nefu);
                    if (lo.readsMac)
                        flow.macClass.insert(s.nmac);
                    if (lo.readsClip)
                        flow.clipClass.insert(s.nclip);
                    if (lo.branch && s.backupReg != 0u && (lo.viRead & (1u << s.backupReg)) != 0u)
                        addRefusal(flow, "branch reads a VI register the pair before it wrote");
                    if (lo.op == Op::Jr)
                    {
                        if (s.link < 0)
                            addRefusal(flow, "JR without a known return address");
                        else
                            flow.jrTargets.insert(static_cast<uint32_t>(s.link));
                    }

                    // ---- what the pair issues
                    TState n = s;
                    if (up.writesMacStatus)
                    {
                        if (n.nmac + n.nclip >= kMaxFlagEntries)
                            addRefusal(flow, "flag pipeline full");
                        else
                            n.mac[n.nmac++] = 4u;
                    }
                    if (up.op == Op::Clip)
                    {
                        if (n.nmac + n.nclip >= kMaxFlagEntries)
                            addRefusal(flow, "flag pipeline full");
                        else
                            n.clip[n.nclip++] = 4u;
                    }
                    if (lo.op == Op::Fcset)
                    {
                        // FCSET cancels the clip write of the upper of its own pair (queueFcset).
                        uint8_t kept = 0;
                        for (uint8_t index = 0; index < n.nclip; ++index)
                        {
                            if (n.clip[index] != 4u)
                                n.clip[kept++] = n.clip[index];
                        }
                        for (uint8_t index = kept; index < n.nclip; ++index)
                            n.clip[index] = 0;
                        n.nclip = kept;
                        n.clip[n.nclip++] = 4u;
                    }
                    if (lo.writesQ != 0u)
                        n.fdiv = lo.writesQ;
                    if (lo.writesP != 0u)
                    {
                        if (n.nefu >= kMaxEfuEntries)
                            addRefusal(flow, "EFU queue full");
                        else
                            n.efu[n.nefu++] = lo.writesP;
                        n.efures = static_cast<uint8_t>(lo.writesP - 1u);
                    }
                    if (lo.vfWrite.reg != 0u && pair.suppressedLowerVf != lo.vfWrite.reg)
                    {
                        const uint8_t latency = lo.vfLatency != 0u ? lo.vfLatency : lo.latency;
                        for (uint32_t component = 0; component < 4u; ++component)
                        {
                            if ((lo.vfWrite.lanes & laneOfComponent(component)) != 0u)
                                n.vf[lo.vfWrite.reg * 4u + component] = latency;
                        }
                    }
                    if (up.vfWrite.reg != 0u)
                    {
                        const uint8_t latency = up.vfLatency != 0u ? up.vfLatency : up.latency;
                        for (uint32_t component = 0; component < 4u; ++component)
                        {
                            if ((up.vfWrite.lanes & laneOfComponent(component)) != 0u)
                                n.vf[up.vfWrite.reg * 4u + component] = latency;
                        }
                    }
                    uint8_t written = 0;
                    for (uint32_t reg = 1; reg < 16u; ++reg)
                    {
                        if ((lo.viWrite & (1u << reg)) != 0u)
                        {
                            n.vi[reg] = lo.viLatency != 0u ? lo.viLatency : lo.latency;
                            if (written == 0u)
                                written = static_cast<uint8_t>(reg);
                        }
                    }
                    n.backupReg = lo.delaysNextBranchRead ? written : uint8_t{0};
                    if (lo.pipeline == Pipeline::Xgkick)
                        n.xg = 1u;
                    if (lo.op == Op::Bal || lo.op == Op::Jalr)
                        n.link = static_cast<int32_t>((s.pc + 16u) & kPcMask);

                    // ---- the next pair, and the branch this one starts
                    const uint32_t next = s.pend >= 0 ? static_cast<uint32_t>(s.pend) : ((s.pc + 8u) & kPcMask);
                    std::vector<int32_t> targets{-1};
                    if (lo.branch)
                    {
                        targets.clear();
                        if (lo.op == Op::B || lo.op == Op::Bal)
                            targets.push_back(static_cast<int32_t>(branchTarget(pair)));
                        else if (lo.op == Op::Jr)
                        {
                            if (s.link >= 0)
                                targets.push_back(s.link);
                        }
                        else if (lo.op == Op::Jalr)
                        {
                            for (uint32_t target : m_program.config->subroutines)
                                targets.push_back(static_cast<int32_t>(target));
                        }
                        else
                        {
                            targets.push_back(static_cast<int32_t>(branchTarget(pair)));
                            targets.push_back(-1);
                        }
                    }
                    const bool ended = s.ebit != 0u;
                    for (int32_t target : targets)
                    {
                        TState m = n;
                        m.pc = next;
                        m.pend = target;
                        m.ebit = (pair.eBit && !ended) ? 1u : 0u;
                        advance(m, 1u);
                        if (ended)
                        {
                            // flushPipelines: everything commits, and the launch is over. What a launch from the
                            // continuation inherits is the start of another exploration.
                            LaunchStart cont;
                            cont.pc = m.pc;
                            cont.backupReg = m.backupReg;
                            cont.link = m.link;
                            continuations.insert(cont);
                            launch.ends.insert(m.pc);
                            continue;
                        }
                        work.push_back(m);
                    }
                }
            }
        };

        std::string hex4(uint32_t value)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%04X", value);
            return text;
        }
    }

    bool analyzeProgram(const Image &image, ProgramFlow &program, std::string &error)
    {
        program = ProgramFlow{};
        program.config = image.config;
        program.pairs = decodeProgram(image.code.data(), image.code.size());
        program.flow.assign(program.pairs.size(), PairFlow{});
        program.emitContexts.assign(program.pairs.size(), 0u);
        const uint32_t ownEnd = image.config->ownPairs != 0u ? image.config->ownPairs * 8u : kCodeBytes;

        // Every launch start, the MSCAL entries first and then the continuations their launches end at.
        std::set<LaunchStart> pending;
        for (uint32_t pc : image.config->mscalEntries)
        {
            LaunchStart start;
            start.pc = pc;
            start.mscal = true;
            pending.insert(start);
        }
        std::set<LaunchStart> done;
        Explorer explorer(program, ownEnd);
        while (!pending.empty())
        {
            const LaunchStart start = *pending.begin();
            pending.erase(pending.begin());
            if (!done.insert(start).second)
                continue;
            LaunchFlow launch;
            launch.start = start;
            std::set<LaunchStart> continuations;
            if (!explorer.run(launch, continuations, error))
                return false;
            program.states += launch.states;
            for (const LaunchStart &cont : continuations)
            {
                if (done.find(cont) == done.end())
                    pending.insert(cont);
            }
            program.launches.push_back(std::move(launch));
        }

        // A launch is translatable when no pair it reaches is refused.
        for (LaunchFlow &launch : program.launches)
        {
            for (uint32_t pc : launch.pcs)
            {
                const PairFlow &flow = program.at(pc);
                if (!flow.refused())
                    continue;
                std::string reason = flow.refusals.empty() ? std::string("a timed read with two answers") : flow.refusals.front();
                launch.refusal = "reaches " + hex4(pc) + ": " + reason;
                break;
            }
        }

        // Entries: an entry pc is handled when every launch start at it is translatable.
        for (const LaunchFlow &launch : program.launches)
        {
            EntryInfo &entry = program.entries[launch.start.pc];
            entry.pc = launch.start.pc;
            if (launch.start.mscal)
                entry.mscal = true;
            else
                entry.continuation = true;
            if (!launch.refusal.empty() && entry.refusal.empty())
                entry.refusal = launch.refusal;
            if (launch.indirect)
                entry.snapshot = true;
        }
        for (auto &[pc, entry] : program.entries)
            entry.handled = entry.refusal.empty();

        for (const LaunchFlow &launch : program.launches)
        {
            if (!program.entries[launch.start.pc].handled)
                continue;
            program.emitPcs.insert(launch.pcs.begin(), launch.pcs.end());
            for (const auto &[pc, context] : launch.contexts)
                program.emitContexts[pc / 8u] |= context;
        }

        for (uint32_t pc : program.emitPcs)
        {
            const PairFlow &flow = program.at(pc);
            if (!flow.qClass.empty())
                program.qDepth = std::max<uint8_t>(program.qDepth, static_cast<uint8_t>(flow.q() + 1u));
            if (!flow.pClass.empty())
                program.pDepth = std::max<uint8_t>(program.pDepth, static_cast<uint8_t>(flow.p() + 1u));
            if (!flow.macClass.empty())
                program.macDepth = std::max<uint8_t>(program.macDepth, static_cast<uint8_t>(flow.mac() + 1u));
            if (!flow.clipClass.empty())
                program.clipDepth = std::max<uint8_t>(program.clipDepth, static_cast<uint8_t>(flow.clip() + 1u));
        }
        return true;
    }

    std::string flowReport(const ProgramFlow &program)
    {
        std::string out;
        char line[256];
        const auto add = [&](const char *format, auto... args)
        {
            std::snprintf(line, sizeof(line), format, args...);
            out += line;
        };

        uint32_t reachable = 0, stalling = 0, refused = 0;
        uint32_t qSites[2] = {}, pSites[3] = {}, macSites[8] = {}, clipSites[8] = {};
        uint32_t qMulti = 0, pMulti = 0, macMulti = 0, clipMulti = 0;
        for (const Pair &pair : program.pairs)
        {
            const PairFlow &flow = program.flow[pair.index()];
            if (flow.contexts == 0u)
                continue;
            ++reachable;
            if (!flow.stall.empty() && *flow.stall.rbegin() != 0u && flow.stall.size() == 1u)
                ++stalling;
            if (flow.refused())
                ++refused;
            if (flow.qClass.size() > 1u)
                ++qMulti;
            else if (!flow.qClass.empty())
                ++qSites[std::min<uint32_t>(flow.q(), 1u)];
            if (flow.pClass.size() > 1u)
                ++pMulti;
            else if (!flow.pClass.empty())
                ++pSites[std::min<uint32_t>(flow.p(), 2u)];
            if (flow.macClass.size() > 1u)
                ++macMulti;
            else if (!flow.macClass.empty())
                ++macSites[std::min<uint32_t>(flow.mac(), 7u)];
            if (flow.clipClass.size() > 1u)
                ++clipMulti;
            else if (!flow.clipClass.empty())
                ++clipSites[std::min<uint32_t>(flow.clip(), 7u)];
        }

        add("image %s: %u launch starts, %llu states, %u reachable pairs, %u refused, %u with a fixed stall\n",
            program.config->name.c_str(), static_cast<unsigned>(program.launches.size()),
            static_cast<unsigned long long>(program.states), reachable, refused, stalling);
        add("  Q reads: %u see the last division, %u the one before, %u both\n", qSites[0], qSites[1], qMulti);
        add("  MFP: %u / %u / %u see the last EFU result / the one before / two before, %u several\n", pSites[0],
            pSites[1], pSites[2], pMulti);
        add("  MAC reads: %u / %u / %u / %u with 0 / 1 / 2 / 3 writers in flight, %u several\n", macSites[0], macSites[1],
            macSites[2], macSites[3], macMulti);
        add("  clip reads: %u / %u / %u / %u with 0 / 1 / 2 / 3 writers in flight, %u several\n", clipSites[0],
            clipSites[1], clipSites[2], clipSites[3], clipMulti);
        add("  rings kept by the code: Q %u, P %u, MAC %u, clip %u; pairs emitted %u\n", program.qDepth, program.pDepth,
            program.macDepth, program.clipDepth, static_cast<unsigned>(program.emitPcs.size()));

        uint32_t handled = 0;
        for (const auto &[pc, entry] : program.entries)
            handled += entry.handled ? 1u : 0u;
        add("  entries: %u, handled %u\n", static_cast<unsigned>(program.entries.size()), handled);
        for (const auto &[pc, entry] : program.entries)
        {
            add("    %04X %-12s %-9s%s%s%s\n", pc,
                entry.mscal && entry.continuation ? "mscal+mscnt" : (entry.mscal ? "mscal" : "mscnt"),
                entry.handled ? "handled" : "refused", entry.snapshot ? "  snapshot" : "",
                entry.refusal.empty() ? "" : "  ", entry.refusal.c_str());
        }
        for (const Pair &pair : program.pairs)
        {
            const PairFlow &flow = program.flow[pair.index()];
            if (flow.contexts == 0u || !flow.refused())
                continue;
            std::string reasons;
            for (const std::string &reason : flow.refusals)
                reasons += (reasons.empty() ? "" : "; ") + reason;
            if (flow.qClass.size() > 1u)
                reasons += (reasons.empty() ? "" : "; ") + std::string("Q class depends on the path");
            if (flow.pClass.size() > 1u)
                reasons += (reasons.empty() ? "" : "; ") + std::string("P class depends on the path");
            if (flow.macClass.size() > 1u)
                reasons += (reasons.empty() ? "" : "; ") + std::string("MAC class depends on the path");
            if (flow.clipClass.size() > 1u)
                reasons += (reasons.empty() ? "" : "; ") + std::string("clip class depends on the path");
            add("  refused pair %04X: %s\n", pair.pc, reasons.c_str());
        }
        return out;
    }
}
