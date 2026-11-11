// The result of a launch and the comparison of two results.
#ifndef MKSM_VU1DIFF_LAUNCH_H
#define MKSM_VU1DIFF_LAUNCH_H

#include "captures.h"
#include "vu1n_api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vu1diff
{
    struct LaunchResult
    {
        VU1State state{};
        std::vector<uint8_t> data = std::vector<uint8_t>(kDataBytes); // VU1 data memory after the launch
        Vu1nHost packets;                                             // the PATH1 packets, in order
    };

    // The fields of VU1State by what a comparison does with them.
    enum StateFieldClass : uint32_t
    {
        // What a translated launch must reproduce bit for bit: vf01..vf31, vi01..vi15, acc, q, p, i, r, pc, clip,
        // ebit, haltAfterDelaySlot, stoppedByD, stoppedByT, branchPending, branchTarget, branchDelay.
        kFieldCompared = 1u,
        // vf00 and vi00: constants.
        kFieldConstant = 2u,
        // mac and status: no program of the game can observe them across launches, a translation need not keep them.
        kFieldFlags = 4u,
        // top, itop, dBitEnabled, tBitEnabled: inputs of a launch.
        kFieldInput = 8u,
        // cycles: the interpreter's clock, compared as a difference where it is compared at all.
        kFieldCycles = 16u,
        kFieldAllButCycles = kFieldCompared | kFieldConstant | kFieldFlags | kFieldInput,
    };

    struct StateField
    {
        std::string name;  // "vf05.z", "vi03", "acc.x", "branchTarget"
        std::string group; // the register or field: "vf05", "vi03", "acc", "branchTarget"
        size_t offset;
        size_t size;
        uint32_t fieldClass;
    };

    // Every field of VU1State, in the order of the struct.
    const std::vector<StateField> &stateFields();

    struct Difference
    {
        bool any = false;
        std::string field;    // where: "vf05.z", "vi03", "data qword 0x123 word 2", "packet 1 of 3: word at 0x40"
        std::string group;    // for statistics: "vf05", "vi03", "data", "packets"
        std::string expected; // the value of the first argument
        std::string actual;   // the value of the second

        explicit operator bool() const { return any; }
    };

    // The first field of the given classes that differs, in the order of the struct.
    Difference compareState(const VU1State &expected, const VU1State &actual, uint32_t classes);

    // The comparison of a translated launch with the oracle's: the kFieldCompared fields of the state, the whole
    // 16 KB of data memory, then the packets (count, sizes, bytes). The first difference.
    Difference compareLaunch(const LaunchResult &expected, const LaunchResult &actual);

    // What upstream's execute() does to the state before it runs (ps2_vu1_core.cpp). The glue prepares a translated
    // launch the same way, an MSCNT included: on the flushed state every launch starts from it changes nothing an
    // MSCNT can observe, and it is what the interpreter does when the glue enters it at the saved pc.
    void prepareLaunch(VU1State &state, uint32_t entryPc, uint32_t top, uint32_t itop);
}

#endif
