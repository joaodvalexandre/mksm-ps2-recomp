#include "oracle.h"

#include "ps2_log.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#include <chrono>
#include <cstring>
#include <thread>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace vu1diff
{
    struct Oracle::Fixture
    {
        PS2Memory memory;
        GS gs;
        Vu1nHost *sink = nullptr;
    };

    Oracle::Oracle() = default;
    Oracle::~Oracle() = default;

    bool Oracle::initialize(std::string &error)
    {
        m_fixture = std::make_unique<Fixture>();
        Fixture &fixture = *m_fixture;
        if (!fixture.memory.initialize() || !fixture.memory.getVU1Code() || !fixture.memory.getVU1Data())
        {
            error = "PS2Memory::initialize failed";
            m_fixture.reset();
            return false;
        }
        fixture.gs.init(fixture.memory.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &fixture.memory.gs());
        std::memset(fixture.memory.getVU1Code(), 0, PS2_VU1_CODE_SIZE);
        std::memset(fixture.memory.getVU1Data(), 0, PS2_VU1_DATA_SIZE);
        // Without a GIF arbiter PS2Memory::submitGifPacket hands every packet to this callback.
        fixture.memory.setGifPacketCallback([&fixture](const uint8_t *bytes, uint32_t size)
        {
            if (!fixture.sink)
                return;
            Vu1nHost &sink = *fixture.sink;
            sink.kicks.push_back({static_cast<uint32_t>(sink.arena.size()), size});
            sink.arena.insert(sink.arena.end(), bytes, bytes + size);
        });
        return true;
    }

    void Oracle::setCode(const uint8_t *code)
    {
        std::memcpy(m_fixture->memory.getVU1Code(), code, PS2_VU1_CODE_SIZE);
        // The interpreters' decode caches are keyed on this counter.
        m_fixture->memory.markVU1CodeModified();
    }

    const uint8_t *Oracle::code() const
    {
        return m_fixture->memory.getVU1Code();
    }

    OracleRun Oracle::run(OracleMachine &machine, OracleEntry entry, uint32_t startPc, uint32_t top, uint32_t itop,
                          uint8_t *data, Vu1nHost &packets)
    {
        Fixture &fixture = *m_fixture;
        VU1Interpreter &vu = *machine.vu;
        uint8_t *code = fixture.memory.getVU1Code();
        fixture.sink = &packets;
        const uint64_t errorsBefore = ps2_log::runtime_log_next_seq();

        OracleRun result;
        if (entry == OracleEntry::Execute)
        {
            const uint64_t start = ticksNow();
            vu.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, fixture.gs, &fixture.memory, startPc, top, itop, kCycleBudget);
            result.ticks = ticksNow() - start;
        }
        else
        {
            if (entry == OracleEntry::ResumeAfterReset)
            {
                const VU1State before = vu.state();
                vu.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, fixture.gs, &fixture.memory, before.pc, top, itop, 0u);
                VU1State &state = vu.state();
                state.ebit = before.ebit;
                state.haltAfterDelaySlot = before.haltAfterDelaySlot;
                state.branchPending = before.branchPending;
                state.branchTarget = before.branchTarget;
                state.branchDelay = before.branchDelay;
            }
            const uint64_t start = ticksNow();
            vu.resume(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, fixture.gs, &fixture.memory, top, itop, kCycleBudget);
            result.ticks = ticksNow() - start;
        }
        fixture.sink = nullptr;

        const uint64_t clock = vu.state().cycles;
        result.cycles = clock - machine.clock;
        machine.clock = clock;
        if (ps2_log::runtime_log_next_seq() != errorsBefore)
            result.end = OracleEnd::Error;
        else if (result.cycles >= kCycleBudget)
            result.end = OracleEnd::Budget;
        return result;
    }

    uint64_t ticksNow()
    {
        return __rdtsc();
    }

    double nanosecondsPerTick()
    {
        static const double value = []
        {
            using Clock = std::chrono::steady_clock;
            const Clock::time_point startTime = Clock::now();
            const uint64_t startTicks = ticksNow();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const uint64_t ticks = ticksNow() - startTicks;
            const double nanoseconds = std::chrono::duration<double, std::nano>(Clock::now() - startTime).count();
            return ticks != 0u ? nanoseconds / static_cast<double>(ticks) : 0.0;
        }();
        return value;
    }
}
