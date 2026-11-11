#include "listing.h"

#include <cstdarg>
#include <cstdio>

namespace vu1gen
{
    namespace
    {
        void appendFormat(std::string &out, const char *pattern, ...)
        {
            char buffer[512];
            va_list arguments;
            va_start(arguments, pattern);
            const int length = std::vsnprintf(buffer, sizeof(buffer), pattern, arguments);
            va_end(arguments);
            if (length > 0)
                out.append(buffer, static_cast<size_t>(length) < sizeof(buffer) ? static_cast<size_t>(length) : sizeof(buffer) - 1u);
        }

        std::string bitsText(const Pair &pair)
        {
            std::string text;
            text += pair.iBit ? 'I' : '-';
            text += pair.eBit ? 'E' : '-';
            text += pair.mBit ? 'M' : '-';
            text += pair.dBit ? 'D' : '-';
            text += pair.tBit ? 'T' : '-';
            return text;
        }

        std::string optional(bool present, int value)
        {
            return present ? std::to_string(value) : std::string("-");
        }

        // The instructions whose immediate the reference decoder lists among the raw fields (the others carry it in
        // the memory operand or in the branch target).
        bool listsRawImmediate(Op op)
        {
            switch (op)
            {
            case Op::Iaddiu:
            case Op::Isubiu:
            case Op::Fceq:
            case Op::Fcand:
            case Op::Fcor:
            case Op::Fcset:
            case Op::Fseq:
            case Op::Fsand:
            case Op::Fsor:
            case Op::Fsset:
            case Op::Iaddi:
                return true;
            default:
                return false;
            }
        }

        void appendInstr(std::string &out, char tag, const Instr &instr)
        {
            std::string vfReads;
            for (uint32_t index = 0; index < instr.vfReadCount; ++index)
            {
                if (index != 0u)
                    vfReads += ',';
                appendFormat(vfReads, "%u:%X", static_cast<unsigned>(instr.vfRead[index].reg), static_cast<unsigned>(instr.vfRead[index].lanes));
            }
            std::string vfWrite = "-";
            if (instr.vfWrite.reg != 0u)
            {
                vfWrite.clear();
                appendFormat(vfWrite, "%u:%X", static_cast<unsigned>(instr.vfWrite.reg), static_cast<unsigned>(instr.vfWrite.lanes));
            }
            std::string memory = "-";
            if (instr.memory)
            {
                memory.clear();
                appendFormat(memory, "%s,%u,%d,%X,%d", mnemonic(instr).c_str(), static_cast<unsigned>(instr.memBase),
                             instr.hasImm ? instr.imm : 0, static_cast<unsigned>(instr.dest), static_cast<int>(instr.memStep));
            }
            std::string raw;
            if (instr.upper)
            {
                appendFormat(raw, "dest=%u,fd=%u,fs=%u,ft=%u,op=%u", static_cast<unsigned>(instr.dest), static_cast<unsigned>(instr.fd),
                             static_cast<unsigned>(instr.fs), static_cast<unsigned>(instr.ft), static_cast<unsigned>(instr.opcode));
                if (instr.special >= 0)
                    appendFormat(raw, ",special=%d", static_cast<int>(instr.special));
            }
            else if (instr.hasFields)
            {
                appendFormat(raw, "dest=%u", static_cast<unsigned>(instr.dest));
                if (listsRawImmediate(instr.op))
                    appendFormat(raw, ",imm=%d", instr.imm);
                appendFormat(raw, ",op_hi=%u", static_cast<unsigned>(instr.opcode));
                if (instr.special >= 0)
                    appendFormat(raw, ",special=%d", static_cast<int>(instr.special));
                appendFormat(raw, ",vfS=%u,vfT=%u,viD=%u,viS=%u,viT=%u", static_cast<unsigned>(instr.fs), static_cast<unsigned>(instr.ft),
                             static_cast<unsigned>(instr.id), static_cast<unsigned>(instr.is), static_cast<unsigned>(instr.it));
            }

            appendFormat(out, "  %c %s reserved=%d nop=%d", tag, mnemonic(instr).c_str(), instr.reserved ? 1 : 0, instr.nop ? 1 : 0);
            appendFormat(out, " | vfr=[%s] vfw=%s vir=%04X viw=%04X accr=%X accw=%X pipe=%s lat=%u vflat=%u vilat=%u waitq=%d waitp=%d"
                              " rclipu=%d wclipu=%d delay=%d",
                         vfReads.c_str(), vfWrite.c_str(), static_cast<unsigned>(instr.viRead), static_cast<unsigned>(instr.viWrite),
                         static_cast<unsigned>(instr.accRead), static_cast<unsigned>(instr.accWrite), pipelineName(instr.pipeline),
                         static_cast<unsigned>(instr.latency), static_cast<unsigned>(instr.vfLatency), static_cast<unsigned>(instr.viLatency),
                         instr.waitQ ? 1 : 0, instr.waitP ? 1 : 0, instr.readsClipUsage ? 1 : 0, instr.writesClipUsage ? 1 : 0,
                         instr.delaysNextBranchRead ? 1 : 0);
            appendFormat(out, " | dest=%X rq=%d rp=%d ri=%d rr=%d wr=%d wq=%u wp=%u rstatus=%d rmac=%d rclip=%d wflags=%d wclip=%d"
                              " wsticky=%d",
                         static_cast<unsigned>(instr.dest), instr.readsQ ? 1 : 0, instr.readsP ? 1 : 0, instr.readsI ? 1 : 0,
                         instr.readsR ? 1 : 0, instr.writesR ? 1 : 0, static_cast<unsigned>(instr.writesQ), static_cast<unsigned>(instr.writesP),
                         instr.readsStatus ? 1 : 0, instr.readsMac ? 1 : 0, instr.readsClip ? 1 : 0, instr.writesMacStatus ? 1 : 0,
                         instr.writesClip ? 1 : 0, instr.writesSticky ? 1 : 0);
            appendFormat(out, " branch=%s rel=%s link=%s cond=%d jr=%s mem=%s kick=%s xtop=%d xitop=%d",
                         instr.branch ? mnemonic(instr).c_str() : "-", optional(instr.hasTarget, instr.imm).c_str(),
                         optional(instr.linkReg >= 0, instr.linkReg).c_str(), instr.conditional ? 1 : 0,
                         optional(instr.jumpReg >= 0, instr.jumpReg).c_str(), memory.c_str(),
                         optional(instr.kickReg >= 0, instr.kickReg).c_str(), instr.xtop ? 1 : 0, instr.xitop ? 1 : 0);
            appendFormat(out, " | raw=[%s]\n", raw.c_str());
        }
    }

    std::string rawListing(const ImageConfig &config, const std::vector<Pair> &pairs)
    {
        std::string out;
        out.reserve(pairs.size() * 112u);
        appendFormat(out, "; VU1 program '%s': ELF 0x%X, image of %u pairs, %u pairs before the next image\n", config.name.c_str(),
                     config.elfAddress, static_cast<unsigned>(pairs.size()), config.ownPairs);
        out += "; addr  pair  lower    upper    bits  | upper instruction                         | lower instruction\n";
        for (const Pair &pair : pairs)
        {
            std::string mark;
            if (pair.reserved)
                mark += " <RESERVED>";
            if (pair.upperVfShadowReg != 0u)
            {
                appendFormat(mark, " <shadow vf%02u%s>", static_cast<unsigned>(pair.upperVfShadowReg),
                             pair.suppressedLowerVf != 0u ? " lower-write-dropped" : "");
            }
            appendFormat(out, "%04X  %4u  %08X %08X %s | %-40s | %s%s\n", pair.pc, pair.index(), pair.lowerWord, pair.upperWord,
                         bitsText(pair).c_str(), instrText(pair.upper, pair.pc).c_str(), instrText(pair.lower, pair.pc).c_str(),
                         mark.c_str());
            if (config.ownPairs != 0u && pair.index() == config.ownPairs - 1u)
                out += "; ---- end of the program's own pairs (the next image starts here in the ELF) ----\n";
        }
        return out;
    }

    std::string usageListing(const std::vector<Pair> &pairs)
    {
        std::string out;
        out.reserve(pairs.size() * 720u);
        for (const Pair &pair : pairs)
        {
            appendFormat(out, "%04X %08X %08X %s shadow=%u suppressed=%u reserved=%d\n", pair.pc, pair.lowerWord, pair.upperWord,
                         bitsText(pair).c_str(), static_cast<unsigned>(pair.upperVfShadowReg),
                         static_cast<unsigned>(pair.suppressedLowerVf), pair.reserved ? 1 : 0);
            appendInstr(out, 'U', pair.upper);
            appendInstr(out, 'L', pair.lower);
        }
        return out;
    }
}
