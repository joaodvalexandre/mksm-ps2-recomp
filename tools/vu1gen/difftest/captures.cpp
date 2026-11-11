#include "captures.h"

#include <cstddef>
#include <cstring>
#include <filesystem>

namespace vu1diff
{
    namespace
    {
        constexpr size_t kHeaderBytes = 48u;
        constexpr uint32_t kVersion = 1u;

        // The records are raw dumps of the struct: they can only be read by a build with the same layout.
        static_assert(sizeof(VU1State) == 664u, "the captures hold a VU1State of 664 bytes");
        static_assert(offsetof(VU1State, vi) == 512u && offsetof(VU1State, acc) == 576u && offsetof(VU1State, q) == 592u);
        static_assert(offsetof(VU1State, pc) == 608u && offsetof(VU1State, cycles) == 624u && offsetof(VU1State, ebit) == 632u);
        static_assert(offsetof(VU1State, top) == 640u && offsetof(VU1State, branchPending) == 648u);
        static_assert(offsetof(VU1State, branchTarget) == 652u && offsetof(VU1State, branchDelay) == 656u);

        uint32_t read32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        uint64_t read64(const uint8_t *p)
        {
            return static_cast<uint64_t>(read32(p)) | (static_cast<uint64_t>(read32(p + 4)) << 32);
        }
    }

    CaptureFile::~CaptureFile()
    {
        if (m_file)
            std::fclose(m_file);
    }

    bool CaptureFile::open(const std::string &path, std::string &error)
    {
        if (m_file)
            std::fclose(m_file);
        m_file = std::fopen(path.c_str(), "rb");
        if (!m_file)
        {
            error = "cannot open " + path;
            return false;
        }
        std::setvbuf(m_file, nullptr, _IOFBF, 1u << 20);
        std::error_code code;
        const uint64_t size = std::filesystem::file_size(path, code);
        m_records = code ? 0u : size / (kHeaderBytes + sizeof(VU1State) + kDataBytes);
        m_read = 0;
        return true;
    }

    bool CaptureFile::next(LaunchRecord &record, std::string &error)
    {
        if (!m_file || m_read >= m_records)
            return false;

        uint8_t header[kHeaderBytes];
        if (std::fread(header, 1u, sizeof(header), m_file) != sizeof(header))
            return false;
        if (std::memcmp(header, "VU1L", 4u) != 0 || read32(header + 4) != kVersion ||
            read32(header + 8) != sizeof(VU1State) || read32(header + 12) != kDataBytes)
        {
            error = "record " + std::to_string(m_read) + " is not a version 1 launch record with a " +
                    std::to_string(sizeof(VU1State)) + " byte state and 16 KB of data";
            return false;
        }
        record.startPc = read32(header + 16);
        record.resume = read32(header + 20) != 0u;
        record.top = read32(header + 24);
        record.itop = read32(header + 28);
        record.codeHash = read64(header + 32);
        record.index = read64(header + 40);
        if (std::fread(&record.state, 1u, sizeof(VU1State), m_file) != sizeof(VU1State) ||
            std::fread(record.data, 1u, kDataBytes, m_file) != kDataBytes)
        {
            return false;
        }
        ++m_read;
        return true;
    }
}
