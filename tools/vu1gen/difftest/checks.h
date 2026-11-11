// Checks of the translator's parts against the interpreter itself, without captures.
#ifndef MKSM_VU1DIFF_CHECKS_H
#define MKSM_VU1DIFF_CHECKS_H

#include "oracle.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace vu1diff
{
    // Vu1nHost::kick against the interpreter's XGKICK transfer on count random data memories (chains of GIF tags
    // of every format, packets that wrap, packets at and over the 64 KB limit, memory without an EOP tag).
    bool runKickCheck(Oracle &oracle, uint64_t count);

    // vu1gen's decoder against VU1Interpreter::decodeInstructionPair: every upper word (bits 24..0), every lower
    // word of the lower-1 group, the other lower opcodes with every register field, randomCount random pairs, and
    // every pair of the code files next to the given capture files.
    bool runDecodeCheck(uint64_t randomCount, const std::vector<std::filesystem::path> &captureFiles);
}

#endif
