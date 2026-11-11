// Translated VU1 programs: the interface between the code vu1gen generates and whoever runs it (the glue in the
// game's override module, vu1difftest).
//
// The unit is a launch: one MSCAL or MSCNT, from an entry pc to the end of the pair after the next E bit. A launch
// is translated or interpreted as a whole. Before a launch the caller identifies the program by the content of the
// 16 KB micro memory and prepares VU1State as upstream's execute() does on entry, for an MSCNT too (its entry pc is
// state.pc): pc set, ebit, haltAfterDelaySlot, stoppedByD/T and the pending branch cleared, TOP, ITOP, VF00; the D/T
// enables come from FBRST as in upstream's callbacks. After a launch that returned Done it submits the recorded
// PATH1 packets, in order, under its own MXCSR.
#ifndef MKSM_VU1N_API_H
#define MKSM_VU1N_API_H

#include <cstddef>
#include <cstdint>
#include <vector>

struct VU1State; // upstream PS2Recomp: runtime/ps2_vu1.h

constexpr uint32_t kVu1nCodeBytes = 0x4000u;       // VU1 micro memory
constexpr uint32_t kVu1nDataBytes = 0x4000u;       // VU1 data memory: 1024 qwords
constexpr uint32_t kVu1nMaxPacketBytes = 0x10000u; // the interpreter's XGKICK buffer: a longer packet is an error
// What a launch may record in all. The interpreter transfers a qword every two cycles and a launch has 65536
// cycles, so it cannot send more than 512 KB; twice that bounds the arena of a launch that runs away.
constexpr uint32_t kVu1nMaxLaunchPacketBytes = 0x100000u;

enum class Vu1nStatus : uint32_t
{
    Done,          // the launch ran to its end: state, data memory and the host's kicks hold the result
    NotHandled,    // entryPc is not an entry point of the program: nothing was touched
    UnknownTarget, // an indirect jump left the translated targets. State and data memory are part-way through the
                   // launch: the caller restores the snapshot it took (Vu1nProgram::needsSnapshot) and interprets
};

// One PATH1 packet of the launch: bytes at arena[offset].
struct Vu1nKick
{
    uint32_t offset;
    uint32_t bytes;
};

// What a launch gives back besides the state and the data memory: the packets of its XGKICKs, copied at the
// instruction, in order. Nothing is sent to the GS from inside a launch. One host serves any number of launches,
// its buffers keep their capacity.
struct Vu1nHost
{
    std::vector<uint8_t> arena;  // the packets, back to back
    std::vector<Vu1nKick> kicks; // in XGKICK order
    uint32_t dropped = 0;        // XGKICKs of the launch that recorded nothing: see kick()

    // Empties the lists; vu1nRun calls it.
    void beginLaunch() noexcept;

    // XGKICK: records the packet that starts at a qword of VU1 data memory (any value: the address is taken
    // modulo 1024, as the interpreter's). The packet is every GIF tag with its data up to the first tag with EOP,
    // read with wrap from qword 0x3FF to qword 0. Returns false, counts the XGKICK in dropped and records nothing
    // where the interpreter reports an error and sends nothing: a packet that would pass 64 KB (also when no tag
    // has EOP); and when the launch already recorded kVu1nMaxLaunchPacketBytes.
    bool kick(const uint8_t *data, uint32_t qwordAddress) noexcept;

    const uint8_t *packet(size_t index) const { return arena.data() + kicks[index].offset; }
};

// The size in bytes of the packet kick() would record, by the same tag walk; false where kick() would drop it for
// its length.
bool vu1nPacketBytes(const uint8_t *data, uint32_t qwordAddress, uint32_t &bytes) noexcept;

enum : uint8_t
{
    kVu1nEntry = 1u,         // a launch may start at this pair: an MSCAL entry or the continuation of an E bit
    kVu1nEntrySnapshot = 2u, // a launch from this entry can reach an indirect jump (it may end with UnknownTarget)
};

// One translated image.
struct Vu1nProgram
{
    const char *name;          // the image's name in config/vu1_images.txt
    const uint8_t *image;      // kVu1nCodeBytes: the micro memory this code was translated from
    const uint8_t *entryFlags; // kVu1nCodeBytes / 8: kVu1nEntry... of each pair
    uint32_t mxcsr;            // the MXCSR the program's arithmetic was compiled for (vu1n.h: kVu1nMxcsr)
    // The translated code. Called through vu1nRun.
    Vu1nStatus (*run)(VU1State &state, uint8_t *data, Vu1nHost &host, uint32_t entryPc);

    uint8_t flagsAt(uint32_t entryPc) const
    {
        return (entryPc & 7u) == 0u && entryPc < kVu1nCodeBytes ? entryFlags[entryPc >> 3] : uint8_t{0};
    }
    bool handles(uint32_t entryPc) const { return (flagsAt(entryPc) & kVu1nEntry) != 0u; }
    bool needsSnapshot(uint32_t entryPc) const { return (flagsAt(entryPc) & kVu1nEntrySnapshot) != 0u; }
};

// The translated images of this build. Defined by the table vu1gen generates.
const Vu1nProgram *vu1nPrograms(size_t &count);

// The program whose image equals the 16 KB of micro memory, or null.
const Vu1nProgram *vu1nFindProgram(const uint8_t *microMemory);

// Runs one launch: NotHandled, with nothing touched, unless the program handles entryPc; otherwise empties the
// host's lists, sets the MXCSR the program needs, runs it and puts the caller's MXCSR back.
Vu1nStatus vu1nRun(const Vu1nProgram &program, VU1State &state, uint8_t *data, Vu1nHost &host, uint32_t entryPc);

#endif
