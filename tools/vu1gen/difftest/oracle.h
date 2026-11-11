// The oracle: upstream's VU1 interpreter, built with the flags of the build tree, on a PS2Memory and GS fixture as
// upstream's own VU1 tests use. Its PATH1 packets are captured with PS2Memory::setGifPacketCallback: nothing
// reaches the GS.
#ifndef MKSM_VU1DIFF_ORACLE_H
#define MKSM_VU1DIFF_ORACLE_H

#include "captures.h"
#include "vu1n_api.h"

#include "runtime/ps2_vu1.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vu1diff
{
    constexpr uint32_t kCycleBudget = 65536u; // what upstream's callbacks give a VU1 launch

    enum class OracleEnd : uint8_t
    {
        Ended,  // the program ended (E bit) and its pipelines were flushed
        Budget, // the cycle budget ran out first: the state is in the middle of the program
        Error,  // the interpreter reported an error (a reserved instruction, an XGKICK packet over 64 KB) and stopped
    };

    // How the interpreter is entered.
    enum class OracleEntry : uint8_t
    {
        // execute(startPC): an MSCAL; also how the glue enters the interpreter for an MSCNT (startPC = state.pc).
        Execute,
        // resume(): an MSCNT as upstream's runtime runs it, on the interpreter's private state as it is.
        Resume,
        // An MSCNT on a state that was put into the interpreter from outside: execute() with a budget of zero
        // cycles (it resets the scheduler and runs nothing), the branch and E state execute() cleared put back,
        // then resume().
        ResumeAfterReset,
    };

    // One interpreter. Its clock counts on from launch to launch, as in the runtime.
    struct OracleMachine
    {
        std::unique_ptr<VU1Interpreter> vu = std::make_unique<VU1Interpreter>();
        uint64_t clock = 0;

        VU1State &state() { return vu->state(); }
    };

    struct OracleRun
    {
        OracleEnd end = OracleEnd::Ended;
        uint64_t cycles = 0; // VU cycles of the launch, its flush included
        uint64_t ticks = 0;  // time stamp counter ticks spent inside execute() or resume()
    };

    class Oracle
    {
    public:
        Oracle();
        ~Oracle();
        Oracle(const Oracle &) = delete;
        Oracle &operator=(const Oracle &) = delete;

        bool initialize(std::string &error);

        // Loads 16 KB into VU1 micro memory, as an upload does.
        void setCode(const uint8_t *code);
        const uint8_t *code() const;

        // Runs one launch of the machine's state on data (16 KB, changed in place). startPc is used by Execute only.
        // The packets are appended to packets.
        OracleRun run(OracleMachine &machine, OracleEntry entry, uint32_t startPc, uint32_t top, uint32_t itop,
                      uint8_t *data, Vu1nHost &packets);

        // The same launch as run() with OracleEntry::Execute, one pair at a time: execute() and then resume() with a
        // budget of one cycle a call, until the program ends. Sets executed[pc / 8] for every pair that ran
        // (kCodeBytes / 8 flags, not cleared here) and returns their number in pairs. The interpreter keeps a launch
        // cut by its budget exactly, so the result must be the one of run(): the caller can check that.
        OracleRun runStepping(OracleMachine &machine, uint32_t startPc, uint32_t top, uint32_t itop, uint8_t *data,
                              Vu1nHost &packets, uint8_t *executed, uint64_t &pairs);

    private:
        struct Fixture;
        std::unique_ptr<Fixture> m_fixture;
    };

    // Time stamp counter ticks, and their length (measured once, against the steady clock).
    uint64_t ticksNow();
    double nanosecondsPerTick();
}

#endif
