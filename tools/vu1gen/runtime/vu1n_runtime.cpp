// The part of the translated programs' runtime that is not generated: the XGKICK packet walk, the launch wrapper
// with the MXCSR guard, the identification of the program. Compiled into the library with the generated code; the
// code that switches MXCSR is here, apart from the generated arithmetic, and has no floating point of its own.
#include "vu1n.h"

#include <algorithm>
#include <cstring>
#include <xmmintrin.h>

namespace
{
    // Sets MXCSR and puts the caller's value back on every way out: the EE code and the GS expect their own state
    // (rounding to nearest), and the control bits of MXCSR are not restored by an unwind.
    class MxcsrScope
    {
    public:
        explicit MxcsrScope(uint32_t value) : m_saved(_mm_getcsr()) { _mm_setcsr(value); }
        ~MxcsrScope() { _mm_setcsr(m_saved); }
        MxcsrScope(const MxcsrScope &) = delete;
        MxcsrScope &operator=(const MxcsrScope &) = delete;

    private:
        unsigned int m_saved;
    };
}

// The interpreter's transfer (progressXgkick, ps2_vu1_core.cpp) reads the packet qword by qword and parses each GIF
// tag as it is copied; on memory that does not change meanwhile that is this walk over the tags.
bool vu1nPacketBytes(const uint8_t *data, uint32_t qwordAddress, uint32_t &bytes) noexcept
{
    const uint32_t source = vu1nQwordOffset(static_cast<int32_t>(qwordAddress));
    uint32_t offset = 0;
    for (;;)
    {
        // The interpreter's buffer is full where the next tag would start.
        if (offset > kVu1nMaxPacketBytes - 16u)
            return false;

        uint64_t tag = 0;
        std::memcpy(&tag, data + ((source + offset) & (kVu1nDataBytes - 1u)), sizeof(tag));
        const uint64_t nloop = tag & 0x7FFFu;
        const bool eop = ((tag >> 15) & 1u) != 0u;
        const uint32_t format = static_cast<uint32_t>((tag >> 58) & 3u);
        uint64_t nreg = (tag >> 60) & 0xFu;
        if (nreg == 0u)
            nreg = 16u;

        uint64_t tagBytes = 16u;
        if (format == 0u) // PACKED
            tagBytes += nloop * nreg * 16u;
        else if (format == 1u) // REGLIST
            tagBytes += ((nloop * nreg + 1u) & ~1ull) * 8u;
        else // IMAGE, and FLG 3 like it
            tagBytes += nloop * 16u;

        if (tagBytes > kVu1nMaxPacketBytes - offset)
            return false;
        offset += static_cast<uint32_t>(tagBytes);
        if (eop)
        {
            bytes = offset;
            return true;
        }
    }
}

void Vu1nHost::beginLaunch() noexcept
{
    arena.clear();
    kicks.clear();
    dropped = 0;
}

bool Vu1nHost::kick(const uint8_t *data, uint32_t qwordAddress) noexcept
{
    uint32_t bytes = 0;
    const size_t start = arena.size();
    if (!vu1nPacketBytes(data, qwordAddress, bytes) || start + bytes > kVu1nMaxLaunchPacketBytes)
    {
        ++dropped;
        return false;
    }

    const uint32_t source = vu1nQwordOffset(static_cast<int32_t>(qwordAddress));
    try
    {
        uint32_t copied = 0;
        while (copied < bytes)
        {
            const uint32_t at = (source + copied) & (kVu1nDataBytes - 1u);
            const uint32_t run = std::min(bytes - copied, kVu1nDataBytes - at);
            arena.insert(arena.end(), data + at, data + at + run);
            copied += run;
        }
        kicks.push_back({static_cast<uint32_t>(start), bytes});
    }
    catch (...)
    {
        // Out of memory. Nothing may leave a launch as an exception: the runtime turns one into a guest error.
        arena.resize(start);
        ++dropped;
        return false;
    }
    return true;
}

const Vu1nProgram *vu1nFindProgram(const uint8_t *microMemory)
{
    size_t count = 0;
    const Vu1nProgram *programs = vu1nPrograms(count);
    for (size_t index = 0; index < count; ++index)
    {
        if (std::memcmp(programs[index].image, microMemory, kVu1nCodeBytes) == 0)
            return &programs[index];
    }
    return nullptr;
}

Vu1nStatus vu1nRun(const Vu1nProgram &program, VU1State &state, uint8_t *data, Vu1nHost &host, uint32_t entryPc)
{
    if (!program.handles(entryPc))
        return Vu1nStatus::NotHandled;
    host.beginLaunch();
    const MxcsrScope scope(program.mxcsr);
    return program.run(state, data, host, entryPc);
}
