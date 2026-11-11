// Control flow and timing of a VU1 program image: what the emitter needs to translate it.
//
// A port of the study's vutiming.py (D:/Projects/Recomps/vu1-study), which follows the scheduling rules of upstream's
// interpreter (ps2_vu1_core.cpp: calculatePairReadyCycle, markPairWrites, queueQ / queueP, the flag pipeline, run()).
// From every launch start (an MSCAL entry with empty pipelines, or the continuation of an E bit with the pipelines
// flushed) it explores every (pc, pipeline timing state) a launch can reach. Values are not computed; what the
// exploration gives is, for each pair, how many producers of Q, P, MAC and clip are still in flight when the pair
// executes. Registers and memory behave sequentially (both instructions of a pair read the state from before the
// pair), so those counts are all the clock decides:
//
//   - a Q read sees the result of the last division executed when no division is in flight, the one before it
//     otherwise (the divider has one slot): class 0 or 1;
//   - an MFP sees the EFU result executed k results ago, k = EFU results in flight (0..2);
//   - FMAND / FMEQ / FMOR see the flags of the flag-writing instruction executed k writers ago;
//   - FCAND / FCEQ / FCOR / FCGET see the clip value as it was k clip writers ago.
//
// A pair whose count is not the same in every state that reaches it, or that does something the translation does not
// model (DESIGN.md, fact 5), is "refused": every entry whose launches can reach it stays with the interpreter.
#ifndef MKSM_VU1GEN_VU_FLOW_H
#define MKSM_VU1GEN_VU_FLOW_H

#include "images.h"
#include "vu_decode.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vu1gen
{
    // The ways a pair is reached inside a launch.
    enum PairContext : uint8_t
    {
        kContextNormal = 1u,    // in sequence or as a branch target: no branch pending, the program goes on after it
        kContextDelaySlot = 2u, // the delay slot of the branch in the pair before it
        kContextFinal = 4u,     // the pair after an E bit: the launch ends after it
    };

    // What the exploration found about one pair.
    struct PairFlow
    {
        uint8_t contexts = 0;           // PairContext bits
        std::set<uint8_t> qClass;       // upper reads Q: divisions in flight
        std::set<uint8_t> pClass;       // MFP: EFU results in flight
        std::set<uint8_t> macClass;     // FMEQ / FMAND / FMOR: flag writers in flight
        std::set<uint8_t> clipClass;    // FCEQ / FCAND / FCOR / FCGET: clip writers in flight
        std::set<uint8_t> stall;        // cycles the pair waits before it issues (with the XGKICK wait)
        std::set<uint32_t> jrTargets;   // JR: the return addresses it can see
        std::vector<std::string> refusals;

        bool refused() const
        {
            return !refusals.empty() || qClass.size() > 1u || pClass.size() > 1u || macClass.size() > 1u ||
                   clipClass.size() > 1u;
        }
        uint8_t q() const { return qClass.empty() ? 0u : *qClass.begin(); }
        uint8_t p() const { return pClass.empty() ? 0u : *pClass.begin(); }
        uint8_t mac() const { return macClass.empty() ? 0u : *macClass.begin(); }
        uint8_t clip() const { return clipClass.empty() ? 0u : *clipClass.begin(); }
    };

    // One launch start: an entry pc and what a launch from it inherits. The state after an E bit carries the VI
    // register a branch may still read the old value of (counted as written by the pair before, which is the
    // conservative reading) and the return address of the last JALR.
    struct LaunchStart
    {
        uint32_t pc = 0;
        uint8_t backupReg = 0;
        int32_t link = -1;
        bool mscal = false;

        bool operator<(const LaunchStart &other) const
        {
            if (pc != other.pc)
                return pc < other.pc;
            if (backupReg != other.backupReg)
                return backupReg < other.backupReg;
            if (link != other.link)
                return link < other.link;
            return mscal < other.mscal;
        }
    };

    struct LaunchFlow
    {
        LaunchStart start;
        std::set<uint32_t> pcs;     // the pairs a launch from this start can execute
        std::map<uint32_t, uint8_t> contexts; // how it reaches them (PairContext bits)
        std::set<uint32_t> ends;    // the continuations it can end at
        bool indirect = false;      // it can execute a JR or JALR
        uint64_t states = 0;
        std::string refusal;        // empty when every pair it reaches is translatable
    };

    struct EntryInfo
    {
        uint32_t pc = 0;
        bool mscal = false;         // listed in the configuration
        bool continuation = false;  // where an E bit's launch ends
        bool handled = false;       // translated: every launch start at this pc is clean
        bool snapshot = false;      // one of them can reach an indirect jump
        std::string refusal;        // why not handled
    };

    struct ProgramFlow
    {
        const ImageConfig *config = nullptr;
        std::vector<Pair> pairs;               // kCodePairs
        std::vector<PairFlow> flow;            // per pair
        std::vector<LaunchFlow> launches;
        std::map<uint32_t, EntryInfo> entries; // by pc
        std::set<uint32_t> emitPcs;            // the pairs reachable from the handled entries
        std::vector<uint8_t> emitContexts;     // per pair: how the handled entries reach it (PairContext bits)
        uint8_t qDepth = 1, pDepth = 1, macDepth = 0, clipDepth = 1; // ring sizes the emitted code keeps
        uint64_t states = 0;

        const PairFlow &at(uint32_t pc) const { return flow[(pc & kPcMask) / 8u]; }
    };

    // Decodes the image and explores it. Returns false only on an internal limit (error says which).
    bool analyzeProgram(const Image &image, ProgramFlow &program, std::string &error);

    // A text report: entries, refusals, the counts of the study's timing table (tpl_20_common.md 5.4).
    std::string flowReport(const ProgramFlow &program);
}

#endif
