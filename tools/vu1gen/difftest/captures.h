// The launch captures a diagnostic of the game's override module wrote (logs/vu1cap): launches_<hash>.bin is a
// stream of records, one per VU1 launch of the program whose 16 KB of micro memory (code_<hash>.bin) have that hash.
//
//   header, 48 bytes, little endian: "VU1L", version (1), state size (664), data size (16384), start pc, resume flag,
//                                    TOP, ITOP, code hash (64 bits), index (64 bits)
//   state:  the raw VU1State at the start of the launch, before execute() or resume() touched it
//   data:   the 16 KB of VU1 data memory at that moment
//
// The records hold inputs only. The index counts the launches of this program; a file holds the first 4000 in
// sequence and then one launch in 41. An MSCNT record has the start pc 0xFFFF: it continues at state.pc.
#ifndef MKSM_VU1DIFF_CAPTURES_H
#define MKSM_VU1DIFF_CAPTURES_H

#include "runtime/ps2_vu1.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace vu1diff
{
    constexpr uint32_t kCodeBytes = 0x4000u;
    constexpr uint32_t kDataBytes = 0x4000u;
    constexpr uint32_t kPcMask = 0x3FFFu;

    struct LaunchRecord
    {
        uint64_t index = 0;
        uint64_t codeHash = 0;
        uint32_t startPc = 0;
        uint32_t top = 0;
        uint32_t itop = 0;
        bool resume = false; // MSCNT
        VU1State state;
        uint8_t data[kDataBytes];

        // Where the launch starts executing.
        uint32_t entryPc() const { return (resume ? state.pc : startPc) & kPcMask; }
    };

    class CaptureFile
    {
    public:
        CaptureFile() = default;
        ~CaptureFile();
        CaptureFile(const CaptureFile &) = delete;
        CaptureFile &operator=(const CaptureFile &) = delete;

        bool open(const std::string &path, std::string &error);
        // The number of whole records in the file.
        uint64_t recordCount() const { return m_records; }
        // Reads the next record. False at the end of the file, and with a message in error when a header is not
        // one of this format.
        bool next(LaunchRecord &record, std::string &error);

    private:
        std::FILE *m_file = nullptr;
        uint64_t m_records = 0;
        uint64_t m_read = 0;
    };
}

#endif
