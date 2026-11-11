// Synthetic launches (DESIGN.md section 6): states and data memory built from captured launches with their float
// values changed, so that the translated programs run the pairs and the arithmetic paths the captures never reach,
// and the result is compared with the oracle's as for a captured launch.
#ifndef MKSM_VU1DIFF_SYNTHETIC_H
#define MKSM_VU1DIFF_SYNTHETIC_H

#include "captures.h"
#include "oracle.h"

#include "vu1n_api.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vu1diff
{
    struct SyntheticEntryStats
    {
        uint64_t launches = 0;
        uint64_t oracleEnded = 0;
        uint64_t oracleBudget = 0; // the oracle ran out of cycles: not comparable
        uint64_t oracleError = 0;  // the oracle stopped on an error (a packet past 64 KB): not comparable
        uint64_t translated = 0;
        uint64_t unknownTarget = 0;
        uint64_t compared = 0;
        uint64_t mismatches = 0;
        // Launches whose packets, read whole at each XGKICK, are not the packets the interpreter transferred while
        // the program went on (DESIGN.md fact 4 does not hold for the changed data, typically because a GIF tag
        // changed and a packet now runs into memory the program writes next). The translation is compared with
        // the packets read at the instructions there, and with the oracle for the state and data memory.
        uint64_t transferDiffers = 0;
    };

    struct SyntheticReport
    {
        std::map<uint32_t, SyntheticEntryStats> entries; // by entry pc
        std::vector<uint8_t> executed = std::vector<uint8_t>(kCodeBytes / 8u, uint8_t{0}); // by pair, synthetic only
        std::map<std::string, uint64_t> mismatchFields;
        uint64_t steppedDifferent = 0; // the oracle one pair a call gave another result than at once
        uint64_t saved = 0;            // mismatching launches written to the failure file
    };

    struct SyntheticOptions
    {
        uint64_t count = 0;   // launches per image
        uint64_t seed = 1;
        uint32_t maxReport = 10;
        std::string failureFile; // capture-format file for the mismatching launches ("" for none)
    };

    // Runs options.count synthetic launches built from the bases (captured records of this image). program may be
    // null (oracle side only: coverage). Returns false on an I/O error.
    bool runSynthetic(const SyntheticOptions &options, Oracle &oracle, const std::vector<std::unique_ptr<LaunchRecord>> &bases,
                      const Vu1nProgram *program, uint64_t codeHash, SyntheticReport &report, std::string &error);
}

#endif
