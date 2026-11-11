#include "vu_decode.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vu1gen
{
    namespace
    {
        constexpr uint8_t fieldDest(uint32_t word) { return static_cast<uint8_t>((word >> 21) & 0xFu); }
        constexpr uint8_t fieldFt(uint32_t word) { return static_cast<uint8_t>((word >> 16) & 0x1Fu); }
        constexpr uint8_t fieldFs(uint32_t word) { return static_cast<uint8_t>((word >> 11) & 0x1Fu); }
        constexpr uint8_t fieldFd(uint32_t word) { return static_cast<uint8_t>((word >> 6) & 0x1Fu); }
        constexpr uint8_t fieldIt(uint32_t word) { return static_cast<uint8_t>((word >> 16) & 0xFu); }
        constexpr uint8_t fieldIs(uint32_t word) { return static_cast<uint8_t>((word >> 11) & 0xFu); }
        constexpr uint8_t fieldId(uint32_t word) { return static_cast<uint8_t>((word >> 6) & 0xFu); }

        constexpr int32_t imm11(uint32_t word)
        {
            const int32_t value = static_cast<int32_t>(word & 0x7FFu);
            return (value & 0x400) != 0 ? value - 0x800 : value;
        }

        // IADDIU / ISUBIU as the interpreter builds it: bits 24..21 above bits 10..0, never negative.
        constexpr int32_t imm15(uint32_t word)
        {
            return static_cast<int32_t>((word & 0x7FFu) | ((word >> 10) & 0x7800u));
        }

        constexpr int32_t imm5(uint32_t word)
        {
            const int32_t value = static_cast<int32_t>((word >> 6) & 0x1Fu);
            return (value & 0x10) != 0 ? value - 32 : value;
        }

        constexpr int32_t imm12(uint32_t word)
        {
            return static_cast<int32_t>((((word >> 21) & 1u) << 11) | (word & 0x7FFu));
        }

        // VU1Interpreter::addVfRead: two registers at most, lanes of one register merged.
        void addVfRead(Instr &instr, uint8_t reg, uint8_t lanes)
        {
            if (lanes == 0u)
                return;
            for (uint32_t index = 0; index < instr.vfReadCount; ++index)
            {
                if (instr.vfRead[index].reg == reg)
                {
                    instr.vfRead[index].lanes |= lanes;
                    return;
                }
            }
            if (instr.vfReadCount < 2u)
                instr.vfRead[instr.vfReadCount++] = {reg, lanes};
        }

        // VU1Interpreter::addVfWrite: VF00 is never declared.
        void addVfWrite(Instr &instr, uint8_t reg, uint8_t lanes)
        {
            if (reg == 0u || lanes == 0u)
                return;
            if (instr.vfWrite.reg == 0u)
                instr.vfWrite = {reg, lanes};
            else if (instr.vfWrite.reg == reg)
                instr.vfWrite.lanes |= lanes;
        }

        void readVi(Instr &instr, uint8_t reg)
        {
            if (reg != 0u)
                instr.viRead |= static_cast<uint16_t>(1u << reg);
        }

        void writeVi(Instr &instr, uint8_t reg)
        {
            if (reg != 0u)
                instr.viWrite |= static_cast<uint16_t>(1u << reg);
        }

        uint8_t vfReadLanes(const Instr &instr, uint8_t reg)
        {
            for (uint32_t index = 0; index < instr.vfReadCount; ++index)
            {
                if (instr.vfRead[index].reg == reg)
                    return instr.vfRead[index].lanes;
            }
            return 0u;
        }

        void setArithmetic(Instr &instr, Op op, Operand operand)
        {
            instr.op = op;
            instr.operand = operand;
            instr.readsQ = operand == Operand::Q;
            instr.readsI = operand == Operand::I;
        }

        void setReserved(Instr &instr)
        {
            instr.op = Op::Reserved;
            instr.reserved = true;
        }

        void setMemory(Instr &instr, uint8_t base, int8_t step)
        {
            instr.memory = true;
            instr.memBase = base;
            instr.memStep = step;
        }

        void setImm(Instr &instr, int32_t value)
        {
            instr.hasImm = true;
            instr.imm = value;
        }

        // EFU latencies (decodeLowerUsage), 0 for a selector that is not an EFU instruction.
        uint8_t efuLatency(uint32_t special)
        {
            switch (special)
            {
            case 0x70:
                return 11u;
            case 0x71:
            case 0x72:
            case 0x77:
                return 18u;
            case 0x73:
                return 24u;
            case 0x74:
            case 0x75:
            case 0x7C:
                return 54u;
            case 0x76:
            case 0x78:
            case 0x7A:
                return 12u;
            case 0x79:
                return 29u;
            case 0x7D:
                return 44u;
            default:
                return 0u;
            }
        }

        Op lowerSpecialOp(uint32_t special)
        {
            switch (special)
            {
            case 0x30: return Op::Move;
            case 0x31: return Op::Mr32;
            case 0x34: return Op::Lqi;
            case 0x35: return Op::Sqi;
            case 0x36: return Op::Lqd;
            case 0x37: return Op::Sqd;
            case 0x38: return Op::Div;
            case 0x39: return Op::Sqrt;
            case 0x3A: return Op::Rsqrt;
            case 0x3B: return Op::Waitq;
            case 0x3C: return Op::Mtir;
            case 0x3D: return Op::Mfir;
            case 0x3E: return Op::Ilwr;
            case 0x3F: return Op::Iswr;
            case 0x40: return Op::Rnext;
            case 0x41: return Op::Rget;
            case 0x42: return Op::Rinit;
            case 0x43: return Op::Rxor;
            case 0x64: return Op::Mfp;
            case 0x68: return Op::Xtop;
            case 0x69: return Op::Xitop;
            case 0x6C: return Op::Xgkick;
            case 0x70: return Op::Esadd;
            case 0x71: return Op::Ersadd;
            case 0x72: return Op::Eleng;
            case 0x73: return Op::Erleng;
            case 0x74: return Op::Eatanxy;
            case 0x75: return Op::Eatanxz;
            case 0x76: return Op::Esum;
            case 0x77: return Op::Ersqrt;
            case 0x78: return Op::Esqrt;
            case 0x79: return Op::Esin;
            case 0x7A: return Op::Ercpr;
            case 0x7B: return Op::Waitp;
            case 0x7C: return Op::Eatan;
            case 0x7D: return Op::Eexp;
            default: return Op::Reserved;
            }
        }

        const char *lowerName(Op op)
        {
            switch (op)
            {
            case Op::Nop: return "NOP";
            case Op::Loi: return "LOI";
            case Op::Lq: return "LQ";
            case Op::Sq: return "SQ";
            case Op::Ilw: return "ILW";
            case Op::Isw: return "ISW";
            case Op::Iaddiu: return "IADDIU";
            case Op::Isubiu: return "ISUBIU";
            case Op::Fceq: return "FCEQ";
            case Op::Fcset: return "FCSET";
            case Op::Fcand: return "FCAND";
            case Op::Fcor: return "FCOR";
            case Op::Fseq: return "FSEQ";
            case Op::Fsset: return "FSSET";
            case Op::Fsand: return "FSAND";
            case Op::Fsor: return "FSOR";
            case Op::Fmeq: return "FMEQ";
            case Op::Fmand: return "FMAND";
            case Op::Fmor: return "FMOR";
            case Op::Fcget: return "FCGET";
            case Op::B: return "B";
            case Op::Bal: return "BAL";
            case Op::Jr: return "JR";
            case Op::Jalr: return "JALR";
            case Op::Ibeq: return "IBEQ";
            case Op::Ibne: return "IBNE";
            case Op::Ibltz: return "IBLTZ";
            case Op::Ibgtz: return "IBGTZ";
            case Op::Iblez: return "IBLEZ";
            case Op::Ibgez: return "IBGEZ";
            case Op::Iadd: return "IADD";
            case Op::Isub: return "ISUB";
            case Op::Iaddi: return "IADDI";
            case Op::Iand: return "IAND";
            case Op::Ior: return "IOR";
            case Op::Move: return "MOVE";
            case Op::Mr32: return "MR32";
            case Op::Lqi: return "LQI";
            case Op::Sqi: return "SQI";
            case Op::Lqd: return "LQD";
            case Op::Sqd: return "SQD";
            case Op::Div: return "DIV";
            case Op::Sqrt: return "SQRT";
            case Op::Rsqrt: return "RSQRT";
            case Op::Waitq: return "WAITQ";
            case Op::Mtir: return "MTIR";
            case Op::Mfir: return "MFIR";
            case Op::Ilwr: return "ILWR";
            case Op::Iswr: return "ISWR";
            case Op::Rnext: return "RNEXT";
            case Op::Rget: return "RGET";
            case Op::Rinit: return "RINIT";
            case Op::Rxor: return "RXOR";
            case Op::Mfp: return "MFP";
            case Op::Xtop: return "XTOP";
            case Op::Xitop: return "XITOP";
            case Op::Xgkick: return "XGKICK";
            case Op::Esadd: return "ESADD";
            case Op::Ersadd: return "ERSADD";
            case Op::Eleng: return "ELENG";
            case Op::Erleng: return "ERLENG";
            case Op::Eatanxy: return "EATANxy";
            case Op::Eatanxz: return "EATANxz";
            case Op::Esum: return "ESUM";
            case Op::Ersqrt: return "ERSQRT";
            case Op::Esqrt: return "ESQRT";
            case Op::Esin: return "ESIN";
            case Op::Ercpr: return "ERCPR";
            case Op::Waitp: return "WAITP";
            case Op::Eatan: return "EATAN";
            case Op::Eexp: return "EEXP";
            default: return "?";
            }
        }

        template <typename... Args>
        std::string format(const char *pattern, Args... args)
        {
            char buffer[256];
            const int length = std::snprintf(buffer, sizeof(buffer), pattern, args...);
            return std::string(buffer, length > 0 ? static_cast<size_t>(length) : 0u);
        }

        std::string vfName(uint8_t reg) { return format("vf%02u", static_cast<unsigned>(reg)); }
        std::string viName(uint8_t reg) { return format("vi%02u", static_cast<unsigned>(reg)); }

        std::string destSuffix(uint8_t dest)
        {
            return dest != 0u ? "." + lanesText(dest) : std::string();
        }

        const char kLaneNames[] = "xyzw";
    }

    Instr decodeUpper(uint32_t word)
    {
        Instr instr;
        instr.word = word;
        instr.upper = true;
        instr.pipeline = Pipeline::Fmac;
        instr.latency = 4u;
        instr.hasFields = true;
        instr.opcode = static_cast<uint8_t>(word & 0x3Fu);
        instr.dest = fieldDest(word);
        instr.fs = fieldFs(word);
        instr.ft = fieldFt(word);
        instr.fd = fieldFd(word);

        const uint8_t op = instr.opcode;
        const uint8_t dest = instr.dest;
        const uint8_t fs = instr.fs;
        const uint8_t ft = instr.ft;
        const uint8_t fd = instr.fd;

        if (op <= 0x2Fu)
        {
            addVfRead(instr, fs, dest);
            addVfWrite(instr, fd, dest);
            if (op <= 0x1Bu)
                addVfRead(instr, ft, laneOfComponent(op & 3u));
            else if (op >= 0x28u)
                addVfRead(instr, ft, op == 0x2Eu ? kLanesXyz : dest);
            if ((op >= 0x08u && op <= 0x0Fu) || op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
                op == 0x29u || op == 0x2Du || op == 0x2Eu)
            {
                instr.accRead = dest;
            }

            if (op <= 0x1Bu)
            {
                static constexpr Op kBroadcastOps[7] = {Op::Add, Op::Sub, Op::Madd, Op::Msub, Op::Max, Op::Mini, Op::Mul};
                setArithmetic(instr, kBroadcastOps[op >> 2], Operand::Broadcast);
                instr.bc = static_cast<uint8_t>(op & 3u);
            }
            else
            {
                switch (op)
                {
                case 0x1C: setArithmetic(instr, Op::Mul, Operand::Q); break;
                case 0x1D: setArithmetic(instr, Op::Max, Operand::I); break;
                case 0x1E: setArithmetic(instr, Op::Mul, Operand::I); break;
                case 0x1F: setArithmetic(instr, Op::Mini, Operand::I); break;
                case 0x20: setArithmetic(instr, Op::Add, Operand::Q); break;
                case 0x21: setArithmetic(instr, Op::Madd, Operand::Q); break;
                case 0x22: setArithmetic(instr, Op::Add, Operand::I); break;
                case 0x23: setArithmetic(instr, Op::Madd, Operand::I); break;
                case 0x24: setArithmetic(instr, Op::Sub, Operand::Q); break;
                case 0x25: setArithmetic(instr, Op::Msub, Operand::Q); break;
                case 0x26: setArithmetic(instr, Op::Sub, Operand::I); break;
                case 0x27: setArithmetic(instr, Op::Msub, Operand::I); break;
                case 0x28: setArithmetic(instr, Op::Add, Operand::Vector); break;
                case 0x29: setArithmetic(instr, Op::Madd, Operand::Vector); break;
                case 0x2A: setArithmetic(instr, Op::Mul, Operand::Vector); break;
                case 0x2B: setArithmetic(instr, Op::Max, Operand::Vector); break;
                case 0x2C: setArithmetic(instr, Op::Sub, Operand::Vector); break;
                case 0x2D: setArithmetic(instr, Op::Msub, Operand::Vector); break;
                case 0x2E: setArithmetic(instr, Op::Opmsub, Operand::Vector); break;
                default: setArithmetic(instr, Op::Mini, Operand::Vector); break; // 0x2F
                }
            }
            // MAX and MINI go through applyDest: no flags. Everything else here through applyFmacDest.
            instr.writesMacStatus = instr.op != Op::Max && instr.op != Op::Mini && dest != 0u;
            return instr;
        }

        if (op >= 0x3Cu)
        {
            const uint32_t special = (word & 3u) | ((word >> 4) & 0x7Cu);
            instr.special = static_cast<int16_t>(special);
            const bool writesAcc =
                special <= 0x0Fu ||
                (special >= 0x18u && special <= 0x1Cu) ||
                special == 0x1Eu ||
                (special >= 0x20u && special <= 0x2Au) ||
                (special >= 0x2Cu && special <= 0x2Eu);
            if (writesAcc)
            {
                addVfRead(instr, fs, dest);
                if (special <= 0x1Bu)
                    addVfRead(instr, ft, laneOfComponent(special & 3u));
                else if (special >= 0x28u && special <= 0x2Eu)
                    addVfRead(instr, ft, special == 0x2Eu ? kLanesXyz : dest);
                instr.accWrite = dest;
                if ((special >= 0x08u && special <= 0x0Fu) || special == 0x21u || special == 0x23u ||
                    special == 0x25u || special == 0x27u || special == 0x29u || special == 0x2Du)
                {
                    instr.accRead = dest;
                }

                instr.toAcc = true;
                if (special <= 0x1Bu)
                {
                    // 0x00 ADDAbc, 0x04 SUBAbc, 0x08 MADDAbc, 0x0C MSUBAbc, 0x18 MULAbc (0x10..0x17 are ITOF / FTOI).
                    static constexpr Op kBroadcastOps[7] = {Op::Add, Op::Sub, Op::Madd, Op::Msub, Op::Reserved, Op::Reserved, Op::Mul};
                    setArithmetic(instr, kBroadcastOps[special >> 2], Operand::Broadcast);
                    instr.bc = static_cast<uint8_t>(special & 3u);
                }
                else
                {
                    switch (special)
                    {
                    case 0x1C: setArithmetic(instr, Op::Mul, Operand::Q); break;
                    case 0x1E: setArithmetic(instr, Op::Mul, Operand::I); break;
                    case 0x20: setArithmetic(instr, Op::Add, Operand::Q); break;
                    case 0x21: setArithmetic(instr, Op::Madd, Operand::Q); break;
                    case 0x22: setArithmetic(instr, Op::Add, Operand::I); break;
                    case 0x23: setArithmetic(instr, Op::Madd, Operand::I); break;
                    case 0x24: setArithmetic(instr, Op::Sub, Operand::Q); break;
                    case 0x25: setArithmetic(instr, Op::Msub, Operand::Q); break;
                    case 0x26: setArithmetic(instr, Op::Sub, Operand::I); break;
                    case 0x27: setArithmetic(instr, Op::Msub, Operand::I); break;
                    case 0x28: setArithmetic(instr, Op::Add, Operand::Vector); break;
                    case 0x29: setArithmetic(instr, Op::Madd, Operand::Vector); break;
                    case 0x2A: setArithmetic(instr, Op::Mul, Operand::Vector); break;
                    case 0x2C: setArithmetic(instr, Op::Sub, Operand::Vector); break;
                    case 0x2D: setArithmetic(instr, Op::Msub, Operand::Vector); break;
                    default: setArithmetic(instr, Op::Opmula, Operand::Vector); break; // 0x2E
                    }
                }
                instr.writesMacStatus = dest != 0u;
            }
            else if ((special >= 0x10u && special <= 0x17u) || special == 0x1Du)
            {
                addVfRead(instr, fs, dest);
                addVfWrite(instr, ft, dest);
                if (special == 0x1Du)
                {
                    instr.op = Op::Abs;
                }
                else
                {
                    static constexpr uint8_t kFractionBits[4] = {0u, 4u, 12u, 15u};
                    instr.op = special <= 0x13u ? Op::Itof : Op::Ftoi;
                    instr.fixedPoint = kFractionBits[special & 3u];
                }
            }
            else if (special == 0x1Fu)
            {
                addVfRead(instr, fs, kLanesXyz);
                addVfRead(instr, ft, kLaneW);
                instr.writesClipUsage = true;
                instr.writesClip = true;
                instr.op = Op::Clip;
            }
            else if (special == 0x2Fu || special == 0x30u)
            {
                instr.op = Op::Nop;
                instr.nop = true;
            }
            else
            {
                setReserved(instr);
            }
            return instr;
        }

        setReserved(instr);
        return instr;
    }

    Instr decodeLower(uint32_t word)
    {
        Instr instr;
        instr.word = word;
        if (word == 0u || word == 0x8000033Cu)
        {
            instr.op = Op::Nop;
            instr.nop = true;
            return instr;
        }

        instr.hasFields = true;
        instr.opcode = static_cast<uint8_t>((word >> 25) & 0x7Fu);
        instr.dest = fieldDest(word);
        instr.fs = fieldFs(word);
        instr.ft = fieldFt(word);
        instr.is = fieldIs(word);
        instr.it = fieldIt(word);
        instr.id = fieldId(word);

        const uint8_t opHi = instr.opcode;
        const uint8_t dest = instr.dest;
        const uint8_t vfS = instr.fs;
        const uint8_t vfT = instr.ft;
        const uint8_t viS = instr.is;
        const uint8_t viT = instr.it;
        const uint8_t viD = instr.id;

        switch (opHi)
        {
        case 0x00:
            instr.op = Op::Lq;
            instr.pipeline = Pipeline::Lsu;
            instr.latency = 4u;
            readVi(instr, viS);
            addVfWrite(instr, vfT, dest);
            setImm(instr, imm11(word));
            setMemory(instr, viS, 0);
            return instr;
        case 0x01:
            instr.op = Op::Sq;
            instr.pipeline = Pipeline::Lsu;
            instr.latency = 1u;
            readVi(instr, viT);
            addVfRead(instr, vfS, dest);
            setImm(instr, imm11(word));
            setMemory(instr, viT, 0);
            return instr;
        case 0x04:
            instr.op = Op::Ilw;
            instr.pipeline = Pipeline::Lsu;
            instr.latency = 4u;
            readVi(instr, viS);
            writeVi(instr, viT);
            setImm(instr, imm11(word));
            setMemory(instr, viS, 0);
            return instr;
        case 0x05:
            instr.op = Op::Isw;
            instr.pipeline = Pipeline::Lsu;
            instr.latency = 1u;
            readVi(instr, viS);
            readVi(instr, viT);
            setImm(instr, imm11(word));
            setMemory(instr, viS, 0);
            return instr;
        case 0x08:
        case 0x09:
            instr.op = opHi == 0x08u ? Op::Iaddiu : Op::Isubiu;
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            instr.delaysNextBranchRead = true;
            readVi(instr, viS);
            writeVi(instr, viT);
            setImm(instr, imm15(word));
            return instr;
        case 0x10:
        case 0x12:
        case 0x13:
            instr.op = opHi == 0x10u ? Op::Fceq : (opHi == 0x12u ? Op::Fcand : Op::Fcor);
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            instr.readsClipUsage = true;
            instr.readsClip = true;
            writeVi(instr, 1u);
            setImm(instr, static_cast<int32_t>(word & 0xFFFFFFu));
            return instr;
        case 0x11:
            instr.op = Op::Fcset;
            instr.pipeline = Pipeline::Fmac;
            instr.latency = 4u;
            instr.writesClipUsage = true;
            instr.writesClip = true;
            setImm(instr, static_cast<int32_t>(word & 0xFFFFFFu));
            return instr;
        case 0x14:
        case 0x16:
        case 0x17:
            instr.op = opHi == 0x14u ? Op::Fseq : (opHi == 0x16u ? Op::Fsand : Op::Fsor);
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            writeVi(instr, viT);
            instr.readsStatus = true;
            setImm(instr, imm12(word));
            return instr;
        case 0x15:
            instr.op = Op::Fsset;
            instr.pipeline = Pipeline::Fmac;
            instr.latency = 4u;
            instr.writesSticky = true;
            setImm(instr, imm12(word));
            return instr;
        case 0x18:
        case 0x1A:
        case 0x1B:
            instr.op = opHi == 0x18u ? Op::Fmeq : (opHi == 0x1Au ? Op::Fmand : Op::Fmor);
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            readVi(instr, viS);
            writeVi(instr, viT);
            instr.readsMac = true;
            return instr;
        case 0x1C:
            instr.op = Op::Fcget;
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            instr.readsClipUsage = true;
            instr.readsClip = true;
            writeVi(instr, viT);
            return instr;
        case 0x20:
            instr.op = Op::B;
            instr.pipeline = Pipeline::Branch;
            instr.branch = true;
            instr.hasTarget = true;
            setImm(instr, imm11(word));
            return instr;
        case 0x21:
            instr.op = Op::Bal;
            instr.pipeline = Pipeline::Branch;
            instr.latency = 1u;
            instr.branch = true;
            instr.hasTarget = true;
            writeVi(instr, viT);
            instr.linkReg = static_cast<int8_t>(viT);
            setImm(instr, imm11(word));
            return instr;
        case 0x24:
            instr.op = Op::Jr;
            instr.pipeline = Pipeline::Branch;
            instr.branch = true;
            readVi(instr, viS);
            instr.jumpReg = static_cast<int8_t>(viS);
            return instr;
        case 0x25:
            instr.op = Op::Jalr;
            instr.pipeline = Pipeline::Branch;
            instr.latency = 1u;
            instr.branch = true;
            readVi(instr, viS);
            writeVi(instr, viT);
            instr.jumpReg = static_cast<int8_t>(viS);
            instr.linkReg = static_cast<int8_t>(viT);
            return instr;
        case 0x28:
        case 0x29:
            instr.op = opHi == 0x28u ? Op::Ibeq : Op::Ibne;
            instr.pipeline = Pipeline::Branch;
            instr.branch = true;
            instr.conditional = true;
            instr.hasTarget = true;
            readVi(instr, viS);
            readVi(instr, viT);
            setImm(instr, imm11(word));
            return instr;
        case 0x2C:
        case 0x2D:
        case 0x2E:
        case 0x2F:
        {
            static constexpr Op kSignBranches[4] = {Op::Ibltz, Op::Ibgtz, Op::Iblez, Op::Ibgez};
            instr.op = kSignBranches[opHi - 0x2Cu];
            instr.pipeline = Pipeline::Branch;
            instr.branch = true;
            instr.conditional = true;
            instr.hasTarget = true;
            readVi(instr, viS);
            setImm(instr, imm11(word));
            return instr;
        }
        case 0x40:
            break;
        default:
            setReserved(instr);
            return instr;
        }

        const uint32_t direct = word & 0x3Fu;
        if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
        {
            instr.op = direct == 0x30u ? Op::Iadd : (direct == 0x31u ? Op::Isub : (direct == 0x34u ? Op::Iand : Op::Ior));
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            instr.delaysNextBranchRead = true;
            readVi(instr, viS);
            readVi(instr, viT);
            writeVi(instr, viD);
            return instr;
        }
        if (direct == 0x32u)
        {
            instr.op = Op::Iaddi;
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            instr.delaysNextBranchRead = true;
            readVi(instr, viS);
            writeVi(instr, viT);
            setImm(instr, imm5(word));
            return instr;
        }
        if (direct < 0x3Cu)
        {
            setReserved(instr);
            return instr;
        }

        const uint32_t special = (word & 3u) | ((word >> 4) & 0x7Cu);
        instr.special = static_cast<int16_t>(special);
        instr.fsf = static_cast<uint8_t>((word >> 21) & 3u);
        instr.ftf = static_cast<uint8_t>((word >> 23) & 3u);
        instr.op = lowerSpecialOp(special);
        switch (instr.op)
        {
        case Op::Move:
        case Op::Mr32:
            instr.pipeline = Pipeline::Fmac;
            instr.latency = 4u;
            addVfRead(instr, vfS, instr.op == Op::Mr32 ? kLanesAll : dest);
            addVfWrite(instr, vfT, dest);
            if (instr.op == Op::Move && dest == 0u)
                instr.nop = true;
            break;
        case Op::Lqi:
        case Op::Lqd:
            instr.pipeline = Pipeline::Lsu;
            instr.latency = 4u;
            instr.viLatency = 1u;
            instr.delaysNextBranchRead = true;
            readVi(instr, viS);
            writeVi(instr, viS);
            addVfWrite(instr, vfT, dest);
            setMemory(instr, viS, instr.op == Op::Lqi ? int8_t{1} : int8_t{-1});
            break;
        case Op::Sqi:
        case Op::Sqd:
            instr.pipeline = Pipeline::Lsu;
            instr.latency = 1u;
            instr.delaysNextBranchRead = true;
            readVi(instr, viT);
            writeVi(instr, viT);
            addVfRead(instr, vfS, dest);
            setMemory(instr, viT, instr.op == Op::Sqi ? int8_t{1} : int8_t{-1});
            break;
        case Op::Div:
            instr.pipeline = Pipeline::Fdiv;
            instr.latency = 7u;
            addVfRead(instr, vfS, laneOfComponent(instr.fsf));
            addVfRead(instr, vfT, laneOfComponent(instr.ftf));
            instr.writesQ = 7u;
            break;
        case Op::Sqrt:
            instr.pipeline = Pipeline::Fdiv;
            instr.latency = 7u;
            addVfRead(instr, vfT, laneOfComponent(instr.ftf));
            instr.writesQ = 7u;
            break;
        case Op::Rsqrt:
            instr.pipeline = Pipeline::Fdiv;
            instr.latency = 13u;
            addVfRead(instr, vfS, laneOfComponent(instr.fsf));
            addVfRead(instr, vfT, laneOfComponent(instr.ftf));
            instr.writesQ = 13u;
            break;
        case Op::Waitq:
            instr.pipeline = Pipeline::Fdiv;
            instr.waitQ = true;
            break;
        case Op::Mtir:
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            instr.delaysNextBranchRead = true;
            addVfRead(instr, vfS, laneOfComponent(instr.fsf));
            writeVi(instr, viT);
            break;
        case Op::Mfir:
            instr.pipeline = Pipeline::Fmac;
            instr.latency = 4u;
            readVi(instr, viS);
            addVfWrite(instr, vfT, dest);
            break;
        case Op::Ilwr:
            instr.pipeline = Pipeline::Lsu;
            instr.latency = 4u;
            readVi(instr, viS);
            writeVi(instr, viT);
            setMemory(instr, viS, 0);
            break;
        case Op::Iswr:
            instr.pipeline = Pipeline::Lsu;
            instr.latency = 1u;
            readVi(instr, viS);
            readVi(instr, viT);
            setMemory(instr, viS, 0);
            break;
        case Op::Rnext:
        case Op::Rget:
            instr.pipeline = Pipeline::Fmac;
            instr.latency = 4u;
            addVfWrite(instr, vfT, dest);
            instr.readsR = true;
            instr.writesR = instr.op == Op::Rnext;
            break;
        case Op::Rinit:
        case Op::Rxor:
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            addVfRead(instr, vfS, laneOfComponent(instr.fsf));
            instr.writesR = true;
            instr.readsR = instr.op == Op::Rxor;
            break;
        case Op::Mfp:
            instr.pipeline = Pipeline::Fmac;
            instr.latency = 4u;
            addVfWrite(instr, vfT, dest);
            instr.readsP = true;
            break;
        case Op::Xtop:
        case Op::Xitop:
            instr.pipeline = Pipeline::Ialu;
            instr.latency = 1u;
            writeVi(instr, viT);
            instr.xtop = instr.op == Op::Xtop;
            instr.xitop = instr.op == Op::Xitop;
            break;
        case Op::Xgkick:
            instr.pipeline = Pipeline::Xgkick;
            instr.latency = 2u;
            readVi(instr, viS);
            instr.kickReg = static_cast<int8_t>(viS);
            break;
        case Op::Waitp:
            instr.pipeline = Pipeline::Efu;
            instr.waitP = true;
            break;
        case Op::Reserved:
            instr.reserved = true;
            break;
        default: // the EFU instructions
            instr.pipeline = Pipeline::Efu;
            instr.latency = efuLatency(special);
            instr.writesP = instr.latency;
            if (special >= 0x70u && special <= 0x73u)
                addVfRead(instr, vfS, kLanesXyz);
            else if (special == 0x74u)
                addVfRead(instr, vfS, kLaneX | kLaneY);
            else if (special == 0x75u)
                addVfRead(instr, vfS, kLaneX | kLaneZ);
            else if (special == 0x76u)
                addVfRead(instr, vfS, kLanesAll);
            else
                addVfRead(instr, vfS, laneOfComponent(instr.fsf));
            break;
        }
        return instr;
    }

    Pair decodePair(uint32_t pc, uint32_t lowerWord, uint32_t upperWord)
    {
        Pair pair;
        pair.pc = pc;
        pair.lowerWord = lowerWord;
        pair.upperWord = upperWord;
        pair.iBit = (upperWord & 0x80000000u) != 0u;
        pair.eBit = (upperWord & 0x40000000u) != 0u;
        pair.mBit = (upperWord & 0x20000000u) != 0u;
        pair.dBit = (upperWord & 0x10000000u) != 0u;
        pair.tBit = (upperWord & 0x08000000u) != 0u;
        pair.upper = decodeUpper(upperWord);
        if (pair.iBit)
        {
            // The lower word is the new I: the interpreter decodes no lower instruction and keeps the default usage.
            pair.lower.word = lowerWord;
            pair.lower.op = Op::Loi;
        }
        else
        {
            pair.lower = decodeLower(lowerWord);
        }

        const uint8_t upperWriteReg = pair.upper.vfWrite.reg;
        const uint8_t lowerWriteReg = pair.lower.vfWrite.reg;
        if (upperWriteReg != 0u && (vfReadLanes(pair.lower, upperWriteReg) != 0u || lowerWriteReg == upperWriteReg))
        {
            pair.upperVfShadowReg = upperWriteReg;
            if (lowerWriteReg == upperWriteReg)
                pair.suppressedLowerVf = upperWriteReg;
        }
        pair.reserved = pair.upper.reserved || pair.lower.reserved;
        return pair;
    }

    std::vector<Pair> decodeProgram(const uint8_t *code, size_t bytes)
    {
        std::vector<Pair> pairs;
        pairs.reserve(bytes / 8u);
        for (size_t pc = 0; pc + 8u <= bytes; pc += 8u)
        {
            const uint8_t *p = code + pc;
            const uint32_t lower = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            const uint32_t upper = static_cast<uint32_t>(p[4]) | (static_cast<uint32_t>(p[5]) << 8) |
                                   (static_cast<uint32_t>(p[6]) << 16) | (static_cast<uint32_t>(p[7]) << 24);
            pairs.push_back(decodePair(static_cast<uint32_t>(pc), lower, upper));
        }
        return pairs;
    }

    const char *pipelineName(Pipeline pipeline)
    {
        static constexpr const char *kNames[8] = {"none", "fmac", "lsu", "fdiv", "efu", "ialu", "branch", "xgkick"};
        return kNames[static_cast<uint32_t>(pipeline) & 7u];
    }

    std::string lanesText(uint8_t lanes)
    {
        std::string text;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lanes & laneOfComponent(component)) != 0u)
                text += kLaneNames[component];
        }
        return text;
    }

    std::string mnemonic(const Instr &instr)
    {
        if (instr.reserved)
            return "RESERVED";
        const char *base = nullptr;
        switch (instr.op)
        {
        case Op::Add: base = "ADD"; break;
        case Op::Sub: base = "SUB"; break;
        case Op::Mul: base = "MUL"; break;
        case Op::Madd: base = "MADD"; break;
        case Op::Msub: base = "MSUB"; break;
        case Op::Max: base = "MAX"; break;
        case Op::Mini: base = "MINI"; break;
        case Op::Opmula: return "OPMULA";
        case Op::Opmsub: return "OPMSUB";
        case Op::Itof: return format("ITOF%u", static_cast<unsigned>(instr.fixedPoint));
        case Op::Ftoi: return format("FTOI%u", static_cast<unsigned>(instr.fixedPoint));
        case Op::Abs: return "ABS";
        case Op::Clip: return "CLIP";
        default: return lowerName(instr.op);
        }
        std::string name = base;
        if (instr.toAcc)
            name += 'A';
        if (instr.operand == Operand::Broadcast)
            name += kLaneNames[instr.bc & 3u];
        else if (instr.operand == Operand::Q)
            name += 'q';
        else if (instr.operand == Operand::I)
            name += 'i';
        return name;
    }

    std::string floatRepr(double value)
    {
        if (std::isnan(value))
            return "nan";
        if (std::isinf(value))
            return value < 0.0 ? "-inf" : "inf";

        // The shortest digits that give the value back, as d[.ddd]e[+-]xx; then Python's choice of notation
        // (float_repr_style 'short': exponent form when the decimal point would be more than 16 digits right of the
        // first digit or more than 4 zeros left of it).
        char buffer[64];
        const std::to_chars_result result =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
        const std::string text(buffer, result.ptr);
        const size_t exponentAt = text.find('e');
        std::string digits;
        bool negative = false;
        for (size_t index = 0; index < exponentAt; ++index)
        {
            const char c = text[index];
            if (c == '-')
                negative = true;
            else if (c >= '0' && c <= '9')
                digits += c;
        }
        const int exponent = std::atoi(text.c_str() + exponentAt + 1u);
        const int pointAt = exponent + 1; // digits before the decimal point
        const int digitCount = static_cast<int>(digits.size());

        std::string out = negative ? "-" : "";
        if (pointAt <= -4 || pointAt > 16)
        {
            out += digits[0];
            if (digitCount > 1)
            {
                out += '.';
                out.append(digits, 1u, std::string::npos);
            }
            out += format("e%c%02d", exponent < 0 ? '-' : '+', std::abs(exponent));
        }
        else if (pointAt <= 0)
        {
            out += "0.";
            out.append(static_cast<size_t>(-pointAt), '0');
            out += digits;
        }
        else if (pointAt >= digitCount)
        {
            out += digits;
            out.append(static_cast<size_t>(pointAt - digitCount), '0');
            out += ".0";
        }
        else
        {
            out.append(digits, 0u, static_cast<size_t>(pointAt));
            out += '.';
            out.append(digits, static_cast<size_t>(pointAt), std::string::npos);
        }
        return out;
    }

    std::string instrText(const Instr &instr, uint32_t pc)
    {
        const std::string name = mnemonic(instr);
        const std::string d = destSuffix(instr.dest);
        const unsigned word = instr.word;

        if (instr.upper)
        {
            if (instr.reserved)
            {
                if (instr.special >= 0)
                    return format("RESERVED upper special 0x%02X (%08X)", static_cast<unsigned>(instr.special), word);
                return format("RESERVED upper op 0x%02X (%08X)", static_cast<unsigned>(instr.opcode), word);
            }
            switch (instr.op)
            {
            case Op::Nop:
                return "NOP";
            case Op::Itof:
            case Op::Ftoi:
            case Op::Abs:
                return name + d + " " + vfName(instr.ft) + ", " + vfName(instr.fs);
            case Op::Clip:
                return "CLIPw.xyz " + vfName(instr.fs) + ", " + vfName(instr.ft) + "w";
            default:
                break;
            }
            std::string text = name + d + " " + (instr.toAcc ? std::string("ACC") : vfName(instr.fd)) + ", " + vfName(instr.fs) + ", ";
            if (instr.operand == Operand::Q)
                text += "Q";
            else if (instr.operand == Operand::I)
                text += "I";
            else if (instr.operand == Operand::Broadcast)
                text += vfName(instr.ft) + kLaneNames[instr.bc & 3u];
            else
                text += vfName(instr.ft);
            return text;
        }

        if (instr.reserved)
        {
            if (instr.opcode != 0x40u)
                return format("RESERVED lower opHi 0x%02X (%08X)", static_cast<unsigned>(instr.opcode), word);
            if (instr.special < 0)
                return format("RESERVED lower1 funct 0x%02X (%08X)", word & 0x3Fu, word);
            return format("RESERVED lower special 0x%02X (%08X)", static_cast<unsigned>(instr.special), word);
        }

        const std::string target = format("0x%04X", (pc + 8u + static_cast<uint32_t>(instr.imm) * 8u) & kPcMask);
        const char fsfName = kLaneNames[instr.fsf & 3u];
        const char ftfName = kLaneNames[instr.ftf & 3u];
        switch (instr.op)
        {
        case Op::Nop:
            return word != 0u ? "NOP" : "NOP(0)";
        case Op::Loi:
        {
            float immediate = 0.0f;
            std::memcpy(&immediate, &instr.word, sizeof(immediate));
            return "LOI " + floatRepr(static_cast<double>(immediate)) + format(" (0x%08X)", word);
        }
        case Op::Lq:
            return name + d + " " + vfName(instr.ft) + format(", %d(", instr.imm) + viName(instr.is) + ")";
        case Op::Sq:
            return name + d + " " + vfName(instr.fs) + format(", %d(", instr.imm) + viName(instr.it) + ")";
        case Op::Ilw:
        case Op::Isw:
            return name + d + " " + viName(instr.it) + format(", %d(", instr.imm) + viName(instr.is) + ")";
        case Op::Iaddiu:
        case Op::Isubiu:
        {
            std::string text = name + " " + viName(instr.it) + ", " + viName(instr.is) + format(", %d", instr.imm);
            if (instr.imm >= 0x100)
                text += format(" (0x%X)", static_cast<unsigned>(instr.imm));
            return text;
        }
        case Op::Fceq:
        case Op::Fcand:
        case Op::Fcor:
            return name + format(" vi01, 0x%06X", static_cast<unsigned>(instr.imm));
        case Op::Fcset:
            return format("FCSET 0x%06X", static_cast<unsigned>(instr.imm));
        case Op::Fseq:
        case Op::Fsand:
        case Op::Fsor:
            return name + " " + viName(instr.it) + format(", 0x%03X", static_cast<unsigned>(instr.imm));
        case Op::Fsset:
            return format("FSSET 0x%03X", static_cast<unsigned>(instr.imm));
        case Op::Fmeq:
        case Op::Fmand:
        case Op::Fmor:
            return name + " " + viName(instr.it) + ", " + viName(instr.is);
        case Op::Fcget:
            return "FCGET " + viName(instr.it);
        case Op::B:
            return "B " + target;
        case Op::Bal:
            return "BAL " + viName(instr.it) + ", " + target;
        case Op::Jr:
            return "JR " + viName(instr.is);
        case Op::Jalr:
            return "JALR " + viName(instr.it) + ", " + viName(instr.is);
        case Op::Ibeq:
        case Op::Ibne:
            return name + " " + viName(instr.it) + ", " + viName(instr.is) + ", " + target;
        case Op::Ibltz:
        case Op::Ibgtz:
        case Op::Iblez:
        case Op::Ibgez:
            return name + " " + viName(instr.is) + ", " + target;
        case Op::Iadd:
        case Op::Isub:
        case Op::Iand:
        case Op::Ior:
            return name + " " + viName(instr.id) + ", " + viName(instr.is) + ", " + viName(instr.it);
        case Op::Iaddi:
            return "IADDI " + viName(instr.it) + ", " + viName(instr.is) + format(", %d", instr.imm);
        case Op::Move:
        case Op::Mr32:
            return name + d + " " + vfName(instr.ft) + ", " + vfName(instr.fs);
        case Op::Lqi:
            return "LQI" + d + " " + vfName(instr.ft) + ", (" + viName(instr.is) + "++)";
        case Op::Lqd:
            return "LQD" + d + " " + vfName(instr.ft) + ", (--" + viName(instr.is) + ")";
        case Op::Sqi:
            return "SQI" + d + " " + vfName(instr.fs) + ", (" + viName(instr.it) + "++)";
        case Op::Sqd:
            return "SQD" + d + " " + vfName(instr.fs) + ", (--" + viName(instr.it) + ")";
        case Op::Div:
        case Op::Rsqrt:
            return name + " Q, " + vfName(instr.fs) + fsfName + ", " + vfName(instr.ft) + ftfName;
        case Op::Sqrt:
            return "SQRT Q, " + vfName(instr.ft) + ftfName;
        case Op::Waitq:
            return "WAITQ";
        case Op::Mtir:
            return "MTIR " + viName(instr.it) + ", " + vfName(instr.fs) + fsfName;
        case Op::Mfir:
            return "MFIR" + d + " " + vfName(instr.ft) + ", " + viName(instr.is);
        case Op::Ilwr:
        case Op::Iswr:
            return name + d + " " + viName(instr.it) + ", (" + viName(instr.is) + ")";
        case Op::Rnext:
        case Op::Rget:
            return name + d + " " + vfName(instr.ft) + ", R";
        case Op::Rinit:
        case Op::Rxor:
            return name + " R, " + vfName(instr.fs) + fsfName;
        case Op::Mfp:
            return "MFP" + d + " " + vfName(instr.ft) + ", P";
        case Op::Xtop:
        case Op::Xitop:
            return name + " " + viName(instr.it);
        case Op::Xgkick:
            return "XGKICK " + viName(instr.is);
        case Op::Waitp:
            return "WAITP";
        case Op::Esadd:
        case Op::Ersadd:
        case Op::Eleng:
        case Op::Erleng:
        case Op::Eatanxy:
        case Op::Eatanxz:
        case Op::Esum:
            return name + " P, " + vfName(instr.fs);
        default: // the scalar EFU forms
            return name + " P, " + vfName(instr.fs) + fsfName;
        }
    }
}
