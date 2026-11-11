// Decoder and usage tables of VU1 instruction pairs.
//
// The decoding is the one of upstream PS2Recomp's VU1 interpreter, which is the oracle of the translation:
// execUpper (ps2_vu1_upper.cpp), execLower (ps2_vu1_lower.cpp) and decodeUpperUsage / decodeLowerUsage /
// decodeInstructionPair (ps2_vu1_core.cpp). The "usage" fields are exactly what the interpreter's scheduler
// declares for an instruction (they decide its stalls); the other fields say what the instruction does.
#ifndef MKSM_VU1GEN_VU_DECODE_H
#define MKSM_VU1GEN_VU_DECODE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vu1gen
{
    constexpr uint32_t kCodeBytes = 0x4000u; // VU1 micro memory: 2048 pairs of 8 bytes (lower word, upper word)
    constexpr uint32_t kCodePairs = kCodeBytes / 8u;
    constexpr uint32_t kPcMask = kCodeBytes - 1u;

    // Lane masks as the interpreter's: x = 8, y = 4, z = 2, w = 1.
    constexpr uint8_t kLaneX = 8u;
    constexpr uint8_t kLaneY = 4u;
    constexpr uint8_t kLaneZ = 2u;
    constexpr uint8_t kLaneW = 1u;
    constexpr uint8_t kLanesXyz = 0xEu;
    constexpr uint8_t kLanesAll = 0xFu;

    constexpr uint8_t laneOfComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - (component & 3u)));
    }

    // The interpreter's pipelines, in its order.
    enum class Pipeline : uint8_t
    {
        None,
        Fmac,
        Lsu,
        Fdiv,
        Efu,
        Ialu,
        Branch,
        Xgkick
    };

    enum class Op : uint8_t
    {
        Reserved,
        Nop, // upper NOP, or a lower word the interpreter skips (0x00000000, 0x8000033C)

        // Upper. The arithmetic family is one Op per operation; Instr::operand and Instr::toAcc give the form
        // (ADD, ADDbc, ADDq, ADDi and their ACC forms ADDA, ADDAbc, ADDAq, ADDAi are all Op::Add).
        Add,
        Sub,
        Mul,
        Madd,
        Msub,
        Max,
        Mini,
        Opmula,
        Opmsub,
        Itof,
        Ftoi,
        Abs,
        Clip,

        // Lower.
        Loi, // the lower word of a pair with the I bit: the new I, not an instruction
        Lq,
        Sq,
        Ilw,
        Isw,
        Iaddiu,
        Isubiu,
        Fceq,
        Fcset,
        Fcand,
        Fcor,
        Fseq,
        Fsset,
        Fsand,
        Fsor,
        Fmeq,
        Fmand,
        Fmor,
        Fcget,
        B,
        Bal,
        Jr,
        Jalr,
        Ibeq,
        Ibne,
        Ibltz,
        Ibgtz,
        Iblez,
        Ibgez,
        Iadd,
        Isub,
        Iaddi,
        Iand,
        Ior,
        Move,
        Mr32,
        Lqi,
        Sqi,
        Lqd,
        Sqd,
        Div,
        Sqrt,
        Rsqrt,
        Waitq,
        Mtir,
        Mfir,
        Ilwr,
        Iswr,
        Rnext,
        Rget,
        Rinit,
        Rxor,
        Mfp,
        Xtop,
        Xitop,
        Xgkick,
        Esadd,
        Ersadd,
        Eleng,
        Erleng,
        Eatanxy,
        Eatanxz,
        Esum,
        Ersqrt,
        Esqrt,
        Esin,
        Ercpr,
        Waitp,
        Eatan,
        Eexp,
    };

    // Where the second operand of an upper arithmetic instruction comes from.
    enum class Operand : uint8_t
    {
        None,
        Vector,    // vf[ft], lane by lane
        Broadcast, // vf[ft][bc] for every lane
        Q,
        I
    };

    struct VfAccess
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct Instr
    {
        uint32_t word = 0;
        bool upper = false;
        Op op = Op::Reserved;
        bool reserved = false; // the interpreter stops the launch at this instruction
        bool nop = false;      // no effect at all

        // ---- usage: what decodeUpperUsage / decodeLowerUsage declare ----
        VfAccess vfRead[2];
        uint8_t vfReadCount = 0;
        VfAccess vfWrite;   // reg 0: none (a write to VF00 is not declared)
        uint16_t viRead = 0;  // one bit per VI register; VI00 is never declared
        uint16_t viWrite = 0;
        uint8_t accRead = 0;  // lanes
        uint8_t accWrite = 0; // lanes
        Pipeline pipeline = Pipeline::None;
        uint8_t latency = 0;
        uint8_t vfLatency = 0; // 0: latency
        uint8_t viLatency = 0; // 0: latency
        bool waitQ = false;
        bool waitP = false;
        bool readsClipUsage = false;  // declared by the interpreter, not used by its scheduler
        bool writesClipUsage = false; // same
        bool delaysNextBranchRead = false;

        // ---- encoding fields ----
        uint8_t opcode = 0;   // upper: bits 5..0; lower: bits 31..25
        int16_t special = -1; // selector of the special groups, -1 outside them
        bool hasFields = false; // false for a skipped lower word and for LOI: no field means anything
        uint8_t dest = 0;     // lane mask, bits 24..21
        uint8_t fs = 0;       // VF register, bits 15..11
        uint8_t ft = 0;       // VF register, bits 20..16
        uint8_t fd = 0;       // VF register, bits 10..6 (upper)
        uint8_t is = 0;       // VI register, bits 14..11 (lower)
        uint8_t it = 0;       // VI register, bits 19..16 (lower)
        uint8_t id = 0;       // VI register, bits 9..6 (lower)
        uint8_t fsf = 0;      // lane index of the fs operand of DIV, RSQRT, MTIR, RINIT, RXOR, the scalar EFU forms
        uint8_t ftf = 0;      // lane index of the ft operand of DIV, SQRT, RSQRT
        bool hasImm = false;
        int32_t imm = 0;      // LQ, SQ, ILW, ISW and the relative branches: 11 bits, signed; IADDIU, ISUBIU: 15 bits,
                              // unsigned; IADDI: 5 bits, signed; FCEQ, FCAND, FCOR, FCSET: 24 bits; FSEQ, FSAND, FSOR,
                              // FSSET: 12 bits

        // ---- semantics ----
        Operand operand = Operand::None; // upper arithmetic (Add .. Mini)
        bool toAcc = false;              // upper arithmetic and OPMULA: the result goes to ACC
        uint8_t bc = 0;                  // lane index of a Broadcast operand
        uint8_t fixedPoint = 0;          // ITOF, FTOI: fraction bits (0, 4, 12, 15)
        bool readsQ = false;
        bool readsP = false;
        bool readsI = false;
        bool readsR = false;
        bool writesR = false;
        uint8_t writesQ = 0;          // DIV, SQRT, RSQRT: cycles until Q changes
        uint8_t writesP = 0;          // EFU: cycles until P changes
        bool readsStatus = false;
        bool readsMac = false;
        bool readsClip = false;
        bool writesMacStatus = false; // MAC and status flags of an FMAC instruction, visible 4 cycles later
        bool writesClip = false;      // CLIP, FCSET
        bool writesSticky = false;    // FSSET
        bool branch = false;
        bool conditional = false;
        bool hasTarget = false; // target = (pc + 8 + imm * 8) & kPcMask
        int8_t linkReg = -1;    // BAL, JALR: the VI register that receives (pc + 16) / 8
        int8_t jumpReg = -1;    // JR, JALR: the VI register with the target (in pairs)
        bool memory = false;    // LQ, SQ, ILW, ISW (address vi[memBase] + imm) and LQI, SQI, LQD, SQD, ILWR, ISWR
        uint8_t memBase = 0;
        int8_t memStep = 0;     // +1 after the access (LQI, SQI), -1 before it (LQD, SQD)
        int8_t kickReg = -1;    // XGKICK: the VI register with the packet's qword address
        bool xtop = false;
        bool xitop = false;
    };

    struct Pair
    {
        uint32_t pc = 0; // byte address in micro memory
        uint32_t lowerWord = 0;
        uint32_t upperWord = 0;
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        Instr upper;
        Instr lower; // Op::Loi with the I bit
        // decodeInstructionPair: when the lower reads or writes the VF register the upper writes, the lower runs on
        // the register's value from before the pair, and its own write to that register is dropped.
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;
        bool reserved = false;

        uint32_t index() const { return pc / 8u; }
    };

    Instr decodeUpper(uint32_t word);
    Instr decodeLower(uint32_t word);
    Pair decodePair(uint32_t pc, uint32_t lowerWord, uint32_t upperWord);
    // Decodes bytes / 8 pairs from little-endian code.
    std::vector<Pair> decodeProgram(const uint8_t *code, size_t bytes);

    const char *pipelineName(Pipeline pipeline);
    // "ADDAx", "MULq", "FTOI4", "LQI", "LOI", "RESERVED" ...
    std::string mnemonic(const Instr &instr);
    // Assembly text of one instruction; pc is the address of its pair (for branch targets).
    std::string instrText(const Instr &instr, uint32_t pc);
    // "xz" for 0xA.
    std::string lanesText(uint8_t lanes);
    // Python's repr() of the value: the text of an immediate in the listings.
    std::string floatRepr(double value);
}

#endif
