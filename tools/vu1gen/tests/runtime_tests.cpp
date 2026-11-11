// The runtime part of the translated programs: the XGKICK packet walk, the launch wrapper and its MXCSR guard, the
// program table. (The packet walk is also compared with the interpreter's transfer by vu1difftest --kick-check.)
#include "test.h"
#include "vu1n.h"

#include <cstring>
#include <stdexcept>
#include <vector>
#include <xmmintrin.h>

// The runtime only passes the state through; these tests need no real one.
struct VU1State
{
    uint32_t marker;
};

namespace
{
    struct RunLog
    {
        unsigned calls = 0;
        uint32_t entryPc = 0;
        unsigned mxcsrInside = 0;
        VU1State *state = nullptr;
        uint8_t *data = nullptr;
        Vu1nHost *host = nullptr;
        size_t kicksAtEntry = 0;
        size_t arenaAtEntry = 0;
        uint32_t droppedAtEntry = 0;
    };
    RunLog g_log;

    uint8_t g_imageA[kVu1nCodeBytes];
    uint8_t g_imageB[kVu1nCodeBytes];
    uint8_t g_flags[kVu1nCodeBytes / 8u];

    Vu1nStatus recordingRun(VU1State &state, uint8_t *data, Vu1nHost &host, uint32_t entryPc)
    {
        ++g_log.calls;
        g_log.entryPc = entryPc;
        g_log.mxcsrInside = _mm_getcsr();
        g_log.state = &state;
        g_log.data = data;
        g_log.host = &host;
        g_log.kicksAtEntry = host.kicks.size();
        g_log.arenaAtEntry = host.arena.size();
        g_log.droppedAtEntry = host.dropped;
        if (entryPc == 0x20u)
            throw std::runtime_error("a translated program never throws; the guard must hold anyway");
        host.kick(data, 5u);
        return entryPc == 0x18u ? Vu1nStatus::UnknownTarget : Vu1nStatus::Done;
    }

    const Vu1nProgram g_programs[2] = {
        {"a", g_imageA, g_flags, 0x7F80u, recordingRun},
        {"b", g_imageB, g_flags, 0xFFC0u, recordingRun},
    };

    void setUpPrograms()
    {
        for (uint32_t index = 0; index < kVu1nCodeBytes; ++index)
        {
            g_imageA[index] = static_cast<uint8_t>(index * 13u + 1u);
            g_imageB[index] = static_cast<uint8_t>(index * 13u + 1u);
        }
        g_imageB[kVu1nCodeBytes - 1u] ^= 0x80u; // the images differ in their last byte only
        std::memset(g_flags, 0, sizeof(g_flags));
        g_flags[0] = kVu1nEntry;
        g_flags[3] = kVu1nEntry | kVu1nEntrySnapshot; // pc 0x18
        g_flags[4] = kVu1nEntry;                      // pc 0x20: the run function throws
        g_log = RunLog();
    }

    // VU1 data memory with a pattern in every byte, so that a copy from the wrong place shows.
    std::vector<uint8_t> patternedData()
    {
        std::vector<uint8_t> data(kVu1nDataBytes);
        for (uint32_t index = 0; index < kVu1nDataBytes; ++index)
            data[index] = static_cast<uint8_t>((index * 31u + 7u) ^ (index >> 8));
        return data;
    }

    void putTag(std::vector<uint8_t> &data, uint32_t qword, uint32_t nloop, uint32_t format, uint32_t nreg, bool eop)
    {
        uint64_t tag = nloop & 0x7FFFu;
        if (eop)
            tag |= 1ull << 15;
        tag |= static_cast<uint64_t>(format & 3u) << 58;
        tag |= static_cast<uint64_t>(nreg & 0xFu) << 60;
        std::memcpy(data.data() + (qword & 0x3FFu) * 16u, &tag, sizeof(tag));
    }

    // The bytes a packet of this size holds: read one by one with the wrap of the interpreter's transfer.
    std::vector<uint8_t> expectedPacket(const std::vector<uint8_t> &data, uint32_t qword, uint32_t bytes)
    {
        std::vector<uint8_t> packet(bytes);
        for (uint32_t index = 0; index < bytes; ++index)
            packet[index] = data[((qword & 0x3FFu) * 16u + index) % kVu1nDataBytes];
        return packet;
    }

    uint32_t packetBytes(const std::vector<uint8_t> &data, uint32_t qword)
    {
        uint32_t bytes = 0xFFFFFFFFu;
        return vu1nPacketBytes(data.data(), qword, bytes) ? bytes : 0u;
    }
}

const Vu1nProgram *vu1nPrograms(size_t &count)
{
    count = 2u;
    return g_programs;
}

TEST_CASE(packet_size_follows_the_gif_tags)
{
    std::vector<uint8_t> data = patternedData();
    // PACKED: nloop * nreg qwords of data.
    putTag(data, 5u, 3u, 0u, 2u, true);
    CHECK_EQ(packetBytes(data, 5u), 16u + 3u * 2u * 16u);
    putTag(data, 5u, 0u, 0u, 2u, true);
    CHECK_EQ(packetBytes(data, 5u), 16u);
    putTag(data, 5u, 4u, 0u, 0u, true); // nreg 0 means 16 registers
    CHECK_EQ(packetBytes(data, 5u), 16u + 4u * 16u * 16u);
    // REGLIST: nloop * nreg doublewords, rounded up to a qword.
    putTag(data, 5u, 3u, 1u, 3u, true);
    CHECK_EQ(packetBytes(data, 5u), 16u + 10u * 8u);
    putTag(data, 5u, 2u, 1u, 2u, true);
    CHECK_EQ(packetBytes(data, 5u), 16u + 4u * 8u);
    putTag(data, 5u, 1u, 1u, 0u, true);
    CHECK_EQ(packetBytes(data, 5u), 16u + 16u * 8u);
    // IMAGE, and FLG 3 like it: nloop qwords.
    putTag(data, 5u, 5u, 2u, 7u, true);
    CHECK_EQ(packetBytes(data, 5u), 16u + 5u * 16u);
    putTag(data, 5u, 5u, 3u, 7u, true);
    CHECK_EQ(packetBytes(data, 5u), 16u + 5u * 16u);

    // Tags follow each other up to the first one with EOP: 1 + 2 qwords, then 1 + 3, then the tag with EOP alone.
    putTag(data, 100u, 2u, 0u, 1u, false);
    putTag(data, 103u, 3u, 2u, 0u, false);
    putTag(data, 107u, 0u, 0u, 1u, true);
    CHECK_EQ(packetBytes(data, 100u), (3u + 4u + 1u) * 16u);

    // The address is taken modulo 1024 qwords.
    CHECK_EQ(packetBytes(data, 100u + 0x400u), 8u * 16u);
    CHECK_EQ(packetBytes(data, 100u + 0xFC00u), 8u * 16u);
}

TEST_CASE(packet_wraps_at_the_end_of_data_memory)
{
    std::vector<uint8_t> data = patternedData();
    putTag(data, 0x3FEu, 4u, 0u, 1u, true); // the tag, qword 0x3FF, then qwords 0, 1, 2
    CHECK_EQ(packetBytes(data, 0x3FEu), 5u * 16u);
    // A tag can sit after the wrap too.
    putTag(data, 0x3FFu, 0u, 0u, 1u, false);
    putTag(data, 0x000u, 1u, 0u, 1u, true);
    CHECK_EQ(packetBytes(data, 0x3FFu), 3u * 16u);

    Vu1nHost host;
    putTag(data, 0x3FEu, 4u, 0u, 1u, true);
    CHECK(host.kick(data.data(), 0x3FEu));
    CHECK_EQ(host.kicks.size(), 1u);
    CHECK_EQ(host.kicks[0].offset, 0u);
    CHECK_EQ(host.kicks[0].bytes, 80u);
    CHECK(host.arena == expectedPacket(data, 0x3FEu, 80u));
    CHECK(std::memcmp(host.packet(0), data.data() + 0x3FE0u, 32u) == 0);
    CHECK(std::memcmp(host.packet(0) + 32u, data.data(), 48u) == 0);
}

TEST_CASE(packet_limit_is_the_interpreters_64_kb)
{
    std::vector<uint8_t> data = patternedData();
    // Exactly 64 KB: allowed. The transfer goes four times around the 16 KB.
    putTag(data, 7u, 4095u, 0u, 1u, true);
    CHECK_EQ(packetBytes(data, 7u), 0x10000u);
    Vu1nHost host;
    CHECK(host.kick(data.data(), 7u));
    CHECK_EQ(host.dropped, 0u);
    CHECK(host.arena == expectedPacket(data, 7u, 0x10000u));

    // One qword more: the interpreter reports an error and sends nothing.
    putTag(data, 7u, 4096u, 0u, 1u, true);
    CHECK_EQ(packetBytes(data, 7u), 0u);
    CHECK(!host.kick(data.data(), 7u));
    CHECK_EQ(host.dropped, 1u);
    CHECK_EQ(host.kicks.size(), 1u);
    CHECK_EQ(host.arena.size(), static_cast<size_t>(0x10000u));
    putTag(data, 7u, 0x7FFFu, 0u, 0u, true);
    CHECK_EQ(packetBytes(data, 7u), 0u);

    // A second tag that does not fit behind the first.
    putTag(data, 7u, 4000u, 2u, 0u, false);
    putTag(data, 7u + 4001u, 94u, 2u, 0u, true);
    CHECK_EQ(packetBytes(data, 7u), 0x10000u);
    putTag(data, 7u + 4001u, 95u, 2u, 0u, true);
    CHECK_EQ(packetBytes(data, 7u), 0u);

    // Tags that fill the 64 KB without an EOP: the next tag would start past the buffer.
    putTag(data, 7u, 4095u, 0u, 1u, false);
    CHECK_EQ(packetBytes(data, 7u), 0u);

    // Memory without any EOP tag: zeros are 4096 empty tags.
    const std::vector<uint8_t> zeros(kVu1nDataBytes, 0u);
    CHECK_EQ(packetBytes(zeros, 0u), 0u);
    CHECK(!host.kick(zeros.data(), 0u));
    CHECK_EQ(host.dropped, 2u);
}

TEST_CASE(host_records_packets_in_order)
{
    std::vector<uint8_t> data = patternedData();
    putTag(data, 10u, 2u, 0u, 1u, true);  // 48 bytes
    putTag(data, 20u, 1u, 1u, 2u, true);  // 32 bytes
    putTag(data, 30u, 0u, 2u, 0u, true);  // 16 bytes
    Vu1nHost host;
    CHECK(host.kick(data.data(), 10u));
    CHECK(host.kick(data.data(), 20u));
    data[10u * 16u + 20u] ^= 0xFFu; // a store after the kick does not reach the recorded packet
    CHECK(host.kick(data.data(), 30u));
    CHECK(host.kick(data.data(), 10u));
    CHECK_EQ(host.kicks.size(), 4u);
    CHECK_EQ(host.kicks[0].offset, 0u);
    CHECK_EQ(host.kicks[0].bytes, 48u);
    CHECK_EQ(host.kicks[1].offset, 48u);
    CHECK_EQ(host.kicks[1].bytes, 32u);
    CHECK_EQ(host.kicks[2].offset, 80u);
    CHECK_EQ(host.kicks[2].bytes, 16u);
    CHECK_EQ(host.kicks[3].offset, 96u);
    CHECK_EQ(host.arena.size(), 144u);
    CHECK(std::memcmp(host.packet(1), data.data() + 20u * 16u, 32u) == 0);
    CHECK(std::memcmp(host.packet(3), data.data() + 10u * 16u, 48u) == 0);
    CHECK(std::memcmp(host.packet(0), host.packet(3), 48u) != 0);
    CHECK_EQ(host.packet(0)[20], static_cast<uint8_t>(host.packet(3)[20] ^ 0xFFu));

    host.dropped = 3u;
    host.beginLaunch();
    CHECK_EQ(host.kicks.size(), 0u);
    CHECK_EQ(host.arena.size(), 0u);
    CHECK_EQ(host.dropped, 0u);
}

TEST_CASE(host_bounds_what_a_launch_records)
{
    std::vector<uint8_t> data = patternedData();
    putTag(data, 0u, 4095u, 0u, 1u, true); // 64 KB
    Vu1nHost host;
    const uint32_t fit = kVu1nMaxLaunchPacketBytes / kVu1nMaxPacketBytes;
    for (uint32_t index = 0; index < fit; ++index)
        CHECK(host.kick(data.data(), 0u));
    CHECK_EQ(host.arena.size(), static_cast<size_t>(kVu1nMaxLaunchPacketBytes));
    CHECK(!host.kick(data.data(), 0u));
    CHECK_EQ(host.kicks.size(), static_cast<size_t>(fit));
    CHECK_EQ(host.dropped, 1u);
    CHECK_EQ(host.kicks[fit - 1u].offset, (fit - 1u) * kVu1nMaxPacketBytes);
}

TEST_CASE(qword_offset_masks_like_the_interpreter)
{
    CHECK_EQ(vu1nQwordOffset(0), 0u);
    CHECK_EQ(vu1nQwordOffset(1), 16u);
    CHECK_EQ(vu1nQwordOffset(0x3FF), 0x3FF0u);
    CHECK_EQ(vu1nQwordOffset(0x400), 0u);
    CHECK_EQ(vu1nQwordOffset(-1), 0x3FF0u);
    CHECK_EQ(vu1nQwordOffset(-0x100), 0x3000u);
    CHECK_EQ(vu1nQwordOffset(0xF00 + 0x20C), 0x10C0u);
    // The interpreter's two forms, for every 16-bit register value and a few immediates.
    for (int32_t vi = -32768; vi <= 32767; ++vi)
    {
        const uint32_t registerForm = (static_cast<uint32_t>(static_cast<uint16_t>(vi)) * 16u) & 0x3FFFu;
        if (vu1nQwordOffset(vi) != registerForm)
            CHECK_EQ(vu1nQwordOffset(vi), registerForm);
        for (const int32_t imm : {-1024, -1, 0, 1, 1023})
        {
            const uint32_t immediateForm = (static_cast<uint32_t>(vi + imm) * 16u) & 0x3FFFu;
            if (vu1nQwordOffset(vi + imm) != immediateForm)
                CHECK_EQ(vu1nQwordOffset(vi + imm), immediateForm);
        }
    }
    CHECK(true);
}

TEST_CASE(program_entry_flags)
{
    setUpPrograms();
    const Vu1nProgram &program = g_programs[0];
    CHECK(program.handles(0x0000u));
    CHECK(!program.needsSnapshot(0x0000u));
    CHECK(program.handles(0x0018u));
    CHECK(program.needsSnapshot(0x0018u));
    CHECK(!program.handles(0x0008u));
    CHECK(!program.needsSnapshot(0x0008u));
    // Not the address of a pair, or outside micro memory: never an entry.
    CHECK(!program.handles(0x0004u));
    CHECK(!program.handles(0x0019u));
    CHECK(!program.handles(0x4000u));
    CHECK(!program.handles(0x4018u));
    CHECK(!program.handles(0xFFFFFFF8u));
    g_flags[kVu1nCodeBytes / 8u - 1u] = kVu1nEntry;
    CHECK(program.handles(0x3FF8u));
}

TEST_CASE(programs_are_found_by_the_whole_image)
{
    setUpPrograms();
    std::vector<uint8_t> memory(g_imageA, g_imageA + kVu1nCodeBytes);
    CHECK(vu1nFindProgram(memory.data()) == &g_programs[0]);
    memory[kVu1nCodeBytes - 1u] ^= 0x80u;
    CHECK(vu1nFindProgram(memory.data()) == &g_programs[1]);
    memory[kVu1nCodeBytes - 1u] ^= 0x01u;
    CHECK(vu1nFindProgram(memory.data()) == nullptr);
    memory.assign(kVu1nCodeBytes, 0u);
    CHECK(vu1nFindProgram(memory.data()) == nullptr);
}

TEST_CASE(run_sets_and_restores_mxcsr)
{
    setUpPrograms();
    std::vector<uint8_t> data = patternedData();
    putTag(data, 5u, 1u, 0u, 1u, true);
    VU1State state{0x12345678u};
    Vu1nHost host;
    const unsigned callerMxcsr = _mm_getcsr();

    // A caller state that is not the default: rounding toward +infinity and a sticky flag. It must come back as it
    // was, not as a constant.
    const unsigned unusual = 0x1F80u | 0x4000u | 0x0020u;
    _mm_setcsr(unusual);
    Vu1nStatus status = vu1nRun(g_programs[0], state, data.data(), host, 0x0000u);
    unsigned after = _mm_getcsr();
    _mm_setcsr(callerMxcsr);
    CHECK(status == Vu1nStatus::Done);
    CHECK_EQ(g_log.calls, 1u);
    CHECK_EQ(g_log.mxcsrInside, 0x7F80u);
    CHECK_EQ(after, unusual);
    CHECK(g_log.state == &state);
    CHECK(g_log.data == data.data());
    CHECK(g_log.host == &host);
    CHECK_EQ(g_log.entryPc, 0u);

    _mm_setcsr(0x1F80u);
    status = vu1nRun(g_programs[1], state, data.data(), host, 0x0018u);
    after = _mm_getcsr();
    _mm_setcsr(callerMxcsr);
    CHECK(status == Vu1nStatus::UnknownTarget);
    CHECK_EQ(g_log.mxcsrInside, 0xFFC0u);
    CHECK_EQ(after, 0x1F80u);

    // Also when the program leaves by an exception.
    _mm_setcsr(0x1F80u);
    bool thrown = false;
    try
    {
        vu1nRun(g_programs[1], state, data.data(), host, 0x0020u);
    }
    catch (const std::runtime_error &)
    {
        thrown = true;
    }
    after = _mm_getcsr();
    _mm_setcsr(callerMxcsr);
    CHECK(thrown);
    CHECK_EQ(g_log.mxcsrInside, 0xFFC0u);
    CHECK_EQ(after, 0x1F80u);
    CHECK_EQ(state.marker, 0x12345678u);
}

TEST_CASE(run_starts_each_launch_with_an_empty_host)
{
    setUpPrograms();
    std::vector<uint8_t> data = patternedData();
    putTag(data, 5u, 1u, 0u, 1u, true);
    VU1State state{0u};
    Vu1nHost host;
    host.kick(data.data(), 5u);
    host.kick(data.data(), 5u);
    host.dropped = 7u;

    // An entry the program does not handle: nothing is called, nothing is touched, the host's lists included.
    const unsigned callerMxcsr = _mm_getcsr();
    CHECK(vu1nRun(g_programs[0], state, data.data(), host, 0x0008u) == Vu1nStatus::NotHandled);
    CHECK(vu1nRun(g_programs[0], state, data.data(), host, 0x4000u) == Vu1nStatus::NotHandled);
    CHECK_EQ(_mm_getcsr(), callerMxcsr);
    CHECK_EQ(g_log.calls, 0u);
    CHECK_EQ(host.kicks.size(), 2u);
    CHECK_EQ(host.dropped, 7u);

    CHECK(vu1nRun(g_programs[0], state, data.data(), host, 0x0000u) == Vu1nStatus::Done);
    CHECK_EQ(g_log.calls, 1u);
    CHECK_EQ(g_log.kicksAtEntry, 0u);
    CHECK_EQ(g_log.arenaAtEntry, 0u);
    CHECK_EQ(g_log.droppedAtEntry, 0u);
    CHECK_EQ(host.kicks.size(), 1u); // the one the program recorded
    CHECK_EQ(host.kicks[0].offset, 0u);
    CHECK_EQ(host.kicks[0].bytes, 32u);
}

TEST_CASE(mxcsr_values_of_the_two_arithmetic_builds)
{
    // Exceptions masked and round toward zero in both; flush to zero and denormals are zero in the fast one only.
#if defined(VU1N_MATH_REFERENCE)
    CHECK_EQ(kVu1nMxcsr, 0x7F80u);
#else
    CHECK_EQ(kVu1nMxcsr, 0xFFC0u);
#endif
    CHECK_EQ(kVu1nMxcsr & 0x1F80u, 0x1F80u);
    CHECK_EQ(kVu1nMxcsr & 0x6000u, 0x6000u);
    CHECK_EQ(kVu1nMxcsr & 0x003Fu, 0u);
}
