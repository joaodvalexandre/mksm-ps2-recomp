// The decoder against hand-encoded instructions. The expectations are written from the instruction formats and from
// the interpreter's usage rules; the comparison with the reference decoder over the game's images is the self-test
// described in PROGRESS.md.
#include "test.h"
#include "vu_decode.h"

#include <cstring>

using namespace vu1gen;

namespace
{
    uint32_t upperMain(uint32_t op, uint32_t dest, uint32_t ft, uint32_t fs, uint32_t fd)
    {
        return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op;
    }

    uint32_t upperSpecial(uint32_t special, uint32_t dest, uint32_t ft, uint32_t fs)
    {
        return (dest << 21) | (ft << 16) | (fs << 11) | ((special & 0x7Cu) << 4) | 0x3Cu | (special & 3u);
    }

    uint32_t lowerHi(uint32_t opHi, uint32_t dest, uint32_t t, uint32_t s, int32_t imm11)
    {
        return (opHi << 25) | (dest << 21) | (t << 16) | (s << 11) | (static_cast<uint32_t>(imm11) & 0x7FFu);
    }

    uint32_t lowerDirect(uint32_t funct, uint32_t t, uint32_t s, uint32_t d)
    {
        return 0x80000000u | (t << 16) | (s << 11) | (d << 6) | funct;
    }

    uint32_t lowerSpecial(uint32_t special, uint32_t dest, uint32_t t, uint32_t s)
    {
        return 0x80000000u | (dest << 21) | (t << 16) | (s << 11) | ((special & 0x7Cu) << 4) | 0x3Cu | (special & 3u);
    }

    constexpr uint32_t kUpperNop = 0x000002FFu;

    void checkVfRead(const Instr &instr, uint32_t index, uint32_t reg, uint32_t lanes)
    {
        CHECK(index < instr.vfReadCount);
        if (index < instr.vfReadCount)
        {
            CHECK_EQ(instr.vfRead[index].reg, reg);
            CHECK_EQ(instr.vfRead[index].lanes, lanes);
        }
    }

    std::string upperText(uint32_t word) { return instrText(decodeUpper(word), 0u); }
    std::string lowerText(uint32_t word, uint32_t pc = 0u) { return instrText(decodeLower(word), pc); }
}

TEST_CASE(upper_vector_and_broadcast_forms)
{
    const Instr add = decodeUpper(upperMain(0x28, 0xE, 2, 1, 3));
    CHECK_EQ(instrText(add, 0u), "ADD.xyz vf03, vf01, vf02");
    CHECK(add.upper);
    CHECK(add.op == Op::Add);
    CHECK(add.operand == Operand::Vector);
    CHECK(!add.toAcc);
    CHECK_EQ(add.vfReadCount, 2);
    checkVfRead(add, 0, 1, 0xE);
    checkVfRead(add, 1, 2, 0xE);
    CHECK_EQ(add.vfWrite.reg, 3);
    CHECK_EQ(add.vfWrite.lanes, 0xE);
    CHECK_EQ(add.accRead, 0);
    CHECK_EQ(add.accWrite, 0);
    CHECK(add.writesMacStatus);
    CHECK(add.pipeline == Pipeline::Fmac);
    CHECK_EQ(add.latency, 4);
    CHECK(!add.readsQ && !add.readsI && !add.reserved && !add.nop);

    const Instr addw = decodeUpper(upperMain(0x03, 0xC, 6, 5, 4));
    CHECK_EQ(instrText(addw, 0u), "ADDw.xy vf04, vf05, vf06w");
    CHECK(addw.operand == Operand::Broadcast);
    CHECK_EQ(addw.bc, 3);
    checkVfRead(addw, 0, 5, 0xC);
    checkVfRead(addw, 1, 6, 0x1);

    // The lanes of one register are merged into one declared read.
    const Instr mul = decodeUpper(upperMain(0x2A, 0xC, 2, 2, 1));
    CHECK_EQ(mul.vfReadCount, 1);
    checkVfRead(mul, 0, 2, 0xC);
    const Instr mulx = decodeUpper(upperMain(0x18, 0x4, 2, 2, 1));
    CHECK_EQ(instrText(mulx, 0u), "MULx.y vf01, vf02, vf02x");
    CHECK_EQ(mulx.vfReadCount, 1);
    checkVfRead(mulx, 0, 2, 0xC);

    // Without lanes nothing is read or written and no flag is set; a broadcast operand is still declared.
    const Instr none = decodeUpper(upperMain(0x28, 0x0, 3, 2, 1));
    CHECK_EQ(instrText(none, 0u), "ADD vf01, vf02, vf03");
    CHECK_EQ(none.vfReadCount, 0);
    CHECK_EQ(none.vfWrite.reg, 0);
    CHECK(!none.writesMacStatus);
    const Instr noneBc = decodeUpper(upperMain(0x01, 0x0, 3, 2, 1));
    CHECK_EQ(noneBc.vfReadCount, 1);
    checkVfRead(noneBc, 0, 3, 0x4);

    // A write to VF00 is not declared, its flags are.
    const Instr sub = decodeUpper(upperMain(0x2C, 0xF, 2, 1, 0));
    CHECK_EQ(instrText(sub, 0u), "SUB.xyzw vf00, vf01, vf02");
    CHECK_EQ(sub.vfWrite.reg, 0);
    CHECK(sub.writesMacStatus);
}

TEST_CASE(upper_accumulator_q_and_i_forms)
{
    const Instr madd = decodeUpper(upperMain(0x29, 0xF, 3, 2, 1));
    CHECK_EQ(instrText(madd, 0u), "MADD.xyzw vf01, vf02, vf03");
    CHECK(madd.op == Op::Madd);
    CHECK_EQ(madd.accRead, 0xF);
    CHECK_EQ(madd.accWrite, 0);

    const Instr maddz = decodeUpper(upperMain(0x0A, 0xE, 3, 2, 1));
    CHECK_EQ(instrText(maddz, 0u), "MADDz.xyz vf01, vf02, vf03z");
    CHECK_EQ(maddz.accRead, 0xE);

    const Instr msubq = decodeUpper(upperMain(0x25, 0x8, 0, 2, 1));
    CHECK_EQ(instrText(msubq, 0u), "MSUBq.x vf01, vf02, Q");
    CHECK(msubq.readsQ);
    CHECK(msubq.operand == Operand::Q);
    CHECK_EQ(msubq.accRead, 0x8);
    CHECK_EQ(msubq.vfReadCount, 1);
    checkVfRead(msubq, 0, 2, 0x8);

    CHECK_EQ(upperText(upperMain(0x1C, 0xF, 0, 2, 1)), "MULq.xyzw vf01, vf02, Q");
    CHECK_EQ(upperText(upperMain(0x20, 0xF, 0, 2, 1)), "ADDq.xyzw vf01, vf02, Q");
    CHECK_EQ(upperText(upperMain(0x21, 0xF, 0, 2, 1)), "MADDq.xyzw vf01, vf02, Q");
    CHECK_EQ(upperText(upperMain(0x24, 0xF, 0, 2, 1)), "SUBq.xyzw vf01, vf02, Q");
    CHECK_EQ(upperText(upperMain(0x1E, 0xF, 0, 2, 1)), "MULi.xyzw vf01, vf02, I");
    CHECK_EQ(upperText(upperMain(0x22, 0xF, 0, 2, 1)), "ADDi.xyzw vf01, vf02, I");
    CHECK_EQ(upperText(upperMain(0x23, 0xF, 0, 2, 1)), "MADDi.xyzw vf01, vf02, I");
    CHECK_EQ(upperText(upperMain(0x26, 0xF, 0, 2, 1)), "SUBi.xyzw vf01, vf02, I");
    CHECK_EQ(upperText(upperMain(0x27, 0xF, 0, 2, 1)), "MSUBi.xyzw vf01, vf02, I");
    CHECK(decodeUpper(upperMain(0x23, 0xF, 0, 2, 1)).readsI);
    CHECK_EQ(decodeUpper(upperMain(0x23, 0xF, 0, 2, 1)).accRead, 0xF);
    CHECK_EQ(decodeUpper(upperMain(0x22, 0xF, 0, 2, 1)).accRead, 0);

    // MAX and MINI set no flags, in any form.
    const Instr maxi = decodeUpper(upperMain(0x1D, 0xF, 0, 8, 7));
    CHECK_EQ(instrText(maxi, 0u), "MAXi.xyzw vf07, vf08, I");
    CHECK(maxi.readsI);
    CHECK(!maxi.writesMacStatus);
    CHECK_EQ(upperText(upperMain(0x1F, 0xF, 0, 8, 7)), "MINIi.xyzw vf07, vf08, I");
    const Instr mini = decodeUpper(upperMain(0x2F, 0xE, 3, 2, 1));
    CHECK_EQ(instrText(mini, 0u), "MINI.xyz vf01, vf02, vf03");
    CHECK(mini.op == Op::Mini);
    CHECK(!mini.writesMacStatus);
    CHECK_EQ(upperText(upperMain(0x2B, 0xF, 8, 8, 7)), "MAX.xyzw vf07, vf08, vf08");
    CHECK_EQ(upperText(upperMain(0x10, 0xF, 0, 25, 26)), "MAXx.xyzw vf26, vf25, vf00x");
    CHECK_EQ(upperText(upperMain(0x17, 0x8, 3, 2, 1)), "MINIw.x vf01, vf02, vf03w");
    CHECK(!decodeUpper(upperMain(0x10, 0xF, 0, 25, 26)).writesMacStatus);

    // OPMSUB declares fs by the mask and always ft.xyz.
    const Instr opmsub = decodeUpper(upperMain(0x2E, 0x8, 3, 2, 1));
    CHECK_EQ(instrText(opmsub, 0u), "OPMSUB.x vf01, vf02, vf03");
    CHECK(opmsub.op == Op::Opmsub);
    checkVfRead(opmsub, 0, 2, 0x8);
    checkVfRead(opmsub, 1, 3, 0xE);
    CHECK_EQ(opmsub.accRead, 0x8);
    CHECK(opmsub.writesMacStatus);
}

TEST_CASE(upper_special_group)
{
    const Instr addax = decodeUpper(upperSpecial(0x00, 0xF, 2, 1));
    CHECK_EQ(instrText(addax, 0u), "ADDAx.xyzw ACC, vf01, vf02x");
    CHECK(addax.op == Op::Add);
    CHECK(addax.toAcc);
    CHECK_EQ(addax.special, 0x00);
    CHECK_EQ(addax.accWrite, 0xF);
    CHECK_EQ(addax.accRead, 0);
    CHECK_EQ(addax.vfWrite.reg, 0);
    checkVfRead(addax, 0, 1, 0xF);
    checkVfRead(addax, 1, 2, 0x8);
    CHECK(addax.writesMacStatus);

    CHECK_EQ(upperText(upperSpecial(0x05, 0xF, 2, 1)), "SUBAy.xyzw ACC, vf01, vf02y");
    CHECK_EQ(upperText(upperSpecial(0x0A, 0xF, 2, 1)), "MADDAz.xyzw ACC, vf01, vf02z");
    CHECK_EQ(upperText(upperSpecial(0x0F, 0xF, 2, 1)), "MSUBAw.xyzw ACC, vf01, vf02w");
    CHECK_EQ(upperText(upperSpecial(0x18, 0xF, 2, 1)), "MULAx.xyzw ACC, vf01, vf02x");
    CHECK_EQ(decodeUpper(upperSpecial(0x0A, 0xE, 2, 1)).accRead, 0xE);
    CHECK_EQ(decodeUpper(upperSpecial(0x18, 0xE, 2, 1)).accRead, 0);

    const Instr maddaq = decodeUpper(upperSpecial(0x21, 0xE, 0, 5));
    CHECK_EQ(instrText(maddaq, 0u), "MADDAq.xyz ACC, vf05, Q");
    CHECK(maddaq.readsQ);
    CHECK_EQ(maddaq.accRead, 0xE);
    CHECK_EQ(maddaq.accWrite, 0xE);
    CHECK_EQ(maddaq.vfReadCount, 1);

    CHECK_EQ(upperText(upperSpecial(0x1C, 0xF, 0, 5)), "MULAq.xyzw ACC, vf05, Q");
    CHECK_EQ(upperText(upperSpecial(0x1E, 0x1, 0, 5)), "MULAi.w ACC, vf05, I");
    CHECK_EQ(upperText(upperSpecial(0x20, 0xF, 0, 5)), "ADDAq.xyzw ACC, vf05, Q");
    CHECK_EQ(upperText(upperSpecial(0x22, 0xF, 0, 5)), "ADDAi.xyzw ACC, vf05, I");
    CHECK_EQ(upperText(upperSpecial(0x23, 0xF, 0, 5)), "MADDAi.xyzw ACC, vf05, I");
    CHECK_EQ(upperText(upperSpecial(0x24, 0xF, 0, 5)), "SUBAq.xyzw ACC, vf05, Q");
    CHECK_EQ(upperText(upperSpecial(0x25, 0xF, 0, 5)), "MSUBAq.xyzw ACC, vf05, Q");
    CHECK_EQ(upperText(upperSpecial(0x26, 0xF, 0, 5)), "SUBAi.xyzw ACC, vf05, I");
    CHECK_EQ(upperText(upperSpecial(0x27, 0xF, 0, 5)), "MSUBAi.xyzw ACC, vf05, I");
    CHECK_EQ(upperText(upperSpecial(0x28, 0xF, 6, 5)), "ADDA.xyzw ACC, vf05, vf06");
    CHECK_EQ(upperText(upperSpecial(0x29, 0xF, 6, 5)), "MADDA.xyzw ACC, vf05, vf06");
    CHECK_EQ(upperText(upperSpecial(0x2A, 0xF, 6, 5)), "MULA.xyzw ACC, vf05, vf06");
    CHECK_EQ(upperText(upperSpecial(0x2C, 0xF, 6, 5)), "SUBA.xyzw ACC, vf05, vf06");
    CHECK_EQ(upperText(upperSpecial(0x2D, 0xF, 6, 5)), "MSUBA.xyzw ACC, vf05, vf06");

    const Instr opmula = decodeUpper(upperSpecial(0x2E, 0xE, 2, 1));
    CHECK_EQ(instrText(opmula, 0u), "OPMULA.xyz ACC, vf01, vf02");
    CHECK(opmula.op == Op::Opmula);
    CHECK(opmula.toAcc);
    checkVfRead(opmula, 0, 1, 0xE);
    checkVfRead(opmula, 1, 2, 0xE);
    CHECK_EQ(opmula.accWrite, 0xE);
    CHECK_EQ(opmula.accRead, 0);

    // ITOF, FTOI and ABS write vf[ft] and set no flags.
    const Instr itof = decodeUpper(upperSpecial(0x12, 0xE, 3, 4));
    CHECK_EQ(instrText(itof, 0u), "ITOF12.xyz vf03, vf04");
    CHECK(itof.op == Op::Itof);
    CHECK_EQ(itof.fixedPoint, 12);
    checkVfRead(itof, 0, 4, 0xE);
    CHECK_EQ(itof.vfWrite.reg, 3);
    CHECK_EQ(itof.vfWrite.lanes, 0xE);
    CHECK(!itof.writesMacStatus);
    CHECK_EQ(itof.accWrite, 0);
    CHECK_EQ(upperText(upperSpecial(0x10, 0xF, 3, 4)), "ITOF0.xyzw vf03, vf04");
    CHECK_EQ(upperText(upperSpecial(0x11, 0xF, 3, 4)), "ITOF4.xyzw vf03, vf04");
    CHECK_EQ(upperText(upperSpecial(0x13, 0xF, 3, 4)), "ITOF15.xyzw vf03, vf04");
    CHECK_EQ(upperText(upperSpecial(0x14, 0xF, 3, 4)), "FTOI0.xyzw vf03, vf04");
    CHECK_EQ(upperText(upperSpecial(0x15, 0xF, 3, 4)), "FTOI4.xyzw vf03, vf04");
    CHECK_EQ(upperText(upperSpecial(0x16, 0xF, 3, 4)), "FTOI12.xyzw vf03, vf04");
    CHECK_EQ(upperText(upperSpecial(0x17, 0xF, 3, 4)), "FTOI15.xyzw vf03, vf04");
    CHECK(decodeUpper(upperSpecial(0x15, 0xF, 3, 4)).op == Op::Ftoi);
    CHECK_EQ(decodeUpper(upperSpecial(0x15, 0xF, 3, 4)).fixedPoint, 4);

    const Instr abs = decodeUpper(upperSpecial(0x1D, 0x2, 0, 13));
    CHECK_EQ(instrText(abs, 0u), "ABS.z vf00, vf13");
    checkVfRead(abs, 0, 13, 0x2);
    CHECK_EQ(abs.vfWrite.reg, 0);

    const Instr clip = decodeUpper(upperSpecial(0x1F, 0xE, 6, 5));
    CHECK_EQ(instrText(clip, 0u), "CLIPw.xyz vf05, vf06w");
    CHECK(clip.op == Op::Clip);
    checkVfRead(clip, 0, 5, 0xE);
    checkVfRead(clip, 1, 6, 0x1);
    CHECK(clip.writesClip && clip.writesClipUsage);
    CHECK(!clip.writesMacStatus);
    CHECK_EQ(clip.vfWrite.reg, 0);

    CHECK_EQ(upperSpecial(0x2F, 0, 0, 0), kUpperNop);
    for (const uint32_t special : {0x2Fu, 0x30u})
    {
        const Instr nop = decodeUpper(upperSpecial(special, 0xF, 3, 4));
        CHECK_EQ(instrText(nop, 0u), "NOP");
        CHECK(nop.nop && !nop.reserved);
        CHECK_EQ(nop.vfReadCount, 0);
        CHECK_EQ(nop.vfWrite.reg, 0);
    }
}

TEST_CASE(upper_reserved_encodings)
{
    for (uint32_t op = 0x30; op <= 0x3B; ++op)
    {
        const Instr instr = decodeUpper(upperMain(op, 0xF, 1, 2, 3));
        CHECK(instr.reserved);
        CHECK_EQ(mnemonic(instr), "RESERVED");
    }
    CHECK_EQ(upperText(0x00000030u), "RESERVED upper op 0x30 (00000030)");
    unsigned reservedSpecials = 0;
    for (uint32_t special = 0; special < 0x80u; ++special)
    {
        const Instr instr = decodeUpper(upperSpecial(special, 0xF, 1, 2));
        const bool valid = special <= 0x2Au || (special >= 0x2Cu && special <= 0x30u);
        CHECK_EQ(instr.reserved, !valid);
        reservedSpecials += instr.reserved ? 1u : 0u;
    }
    CHECK_EQ(reservedSpecials, 0x80u - 0x30u);
    CHECK_EQ(upperText(upperSpecial(0x2B, 0, 0, 0)), "RESERVED upper special 0x2B (000002BF)");
    // The interpreter keeps the FMAC pipeline and latency on a reserved upper word.
    CHECK(decodeUpper(0x00000030u).pipeline == Pipeline::Fmac);
}

TEST_CASE(lower_memory_instructions)
{
    const Instr lq = decodeLower(lowerHi(0x00, 0xF, 5, 3, 7));
    CHECK_EQ(instrText(lq, 0u), "LQ.xyzw vf05, 7(vi03)");
    CHECK(lq.op == Op::Lq);
    CHECK(!lq.upper);
    CHECK(lq.pipeline == Pipeline::Lsu);
    CHECK_EQ(lq.latency, 4);
    CHECK_EQ(lq.viRead, 1u << 3);
    CHECK_EQ(lq.viWrite, 0);
    CHECK_EQ(lq.vfWrite.reg, 5);
    CHECK_EQ(lq.vfWrite.lanes, 0xF);
    CHECK(lq.memory);
    CHECK_EQ(lq.memBase, 3);
    CHECK_EQ(lq.imm, 7);
    CHECK_EQ(lq.memStep, 0);
    CHECK(!lq.delaysNextBranchRead);

    const Instr lqNegative = decodeLower(lowerHi(0x00, 0x8, 5, 0, -3));
    CHECK_EQ(instrText(lqNegative, 0u), "LQ.x vf05, -3(vi00)");
    CHECK_EQ(lqNegative.imm, -3);
    CHECK_EQ(lqNegative.viRead, 0);
    CHECK_EQ(decodeLower(lowerHi(0x00, 0xF, 5, 3, 0x3FF)).imm, 1023);
    CHECK_EQ(decodeLower(lowerHi(0x00, 0xF, 5, 3, 0x400)).imm, -1024);
    // VF00 as the destination is not declared.
    CHECK_EQ(decodeLower(lowerHi(0x00, 0xF, 0, 3, 0)).vfWrite.reg, 0);

    const Instr sq = decodeLower(lowerHi(0x01, 0xE, 4, 5, 12));
    CHECK_EQ(instrText(sq, 0u), "SQ.xyz vf05, 12(vi04)");
    CHECK_EQ(sq.latency, 1);
    CHECK_EQ(sq.viRead, 1u << 4);
    checkVfRead(sq, 0, 5, 0xE);
    CHECK_EQ(sq.vfWrite.reg, 0);
    CHECK_EQ(sq.memBase, 4);

    const Instr ilw = decodeLower(lowerHi(0x04, 0x8, 2, 1, 8));
    CHECK_EQ(instrText(ilw, 0u), "ILW.x vi02, 8(vi01)");
    CHECK_EQ(ilw.latency, 4);
    CHECK_EQ(ilw.viRead, 1u << 1);
    CHECK_EQ(ilw.viWrite, 1u << 2);
    CHECK(!ilw.delaysNextBranchRead);

    const Instr isw = decodeLower(lowerHi(0x05, 0x1, 2, 1, 8));
    CHECK_EQ(instrText(isw, 0u), "ISW.w vi02, 8(vi01)");
    CHECK_EQ(isw.latency, 1);
    CHECK_EQ(isw.viRead, (1u << 1) | (1u << 2));
    CHECK_EQ(isw.viWrite, 0);

    const Instr lqi = decodeLower(lowerSpecial(0x34, 0xF, 11, 6));
    CHECK_EQ(instrText(lqi, 0u), "LQI.xyzw vf11, (vi06++)");
    CHECK(lqi.op == Op::Lqi);
    CHECK_EQ(lqi.latency, 4);
    CHECK_EQ(lqi.viLatency, 1);
    CHECK_EQ(lqi.viRead, 1u << 6);
    CHECK_EQ(lqi.viWrite, 1u << 6);
    CHECK_EQ(lqi.vfWrite.reg, 11);
    CHECK_EQ(lqi.memStep, 1);
    CHECK_EQ(lqi.memBase, 6);
    CHECK(lqi.delaysNextBranchRead);
    CHECK(!lqi.hasImm);

    const Instr sqi = decodeLower(lowerSpecial(0x35, 0xF, 4, 10));
    CHECK_EQ(sqi.word, 0x81E4537Du);
    CHECK_EQ(instrText(sqi, 0u), "SQI.xyzw vf10, (vi04++)");
    CHECK_EQ(sqi.latency, 1);
    CHECK_EQ(sqi.viLatency, 0);
    CHECK_EQ(sqi.viRead, 1u << 4);
    CHECK_EQ(sqi.viWrite, 1u << 4);
    checkVfRead(sqi, 0, 10, 0xF);
    CHECK_EQ(sqi.memStep, 1);
    CHECK(sqi.delaysNextBranchRead);

    const Instr lqd = decodeLower(lowerSpecial(0x36, 0xF, 1, 2));
    CHECK_EQ(instrText(lqd, 0u), "LQD.xyzw vf01, (--vi02)");
    CHECK_EQ(lqd.memStep, -1);
    CHECK_EQ(lqd.viLatency, 1);
    const Instr sqd = decodeLower(lowerSpecial(0x37, 0x8, 2, 1));
    CHECK_EQ(instrText(sqd, 0u), "SQD.x vf01, (--vi02)");
    CHECK_EQ(sqd.memStep, -1);

    const Instr ilwr = decodeLower(lowerSpecial(0x3E, 0x8, 3, 2));
    CHECK_EQ(instrText(ilwr, 0u), "ILWR.x vi03, (vi02)");
    CHECK_EQ(ilwr.latency, 4);
    CHECK_EQ(ilwr.viRead, 1u << 2);
    CHECK_EQ(ilwr.viWrite, 1u << 3);
    CHECK(ilwr.memory);
    const Instr iswr = decodeLower(lowerSpecial(0x3F, 0x8, 3, 2));
    CHECK_EQ(instrText(iswr, 0u), "ISWR.x vi03, (vi02)");
    CHECK_EQ(iswr.latency, 1);
    CHECK_EQ(iswr.viRead, (1u << 2) | (1u << 3));
}

TEST_CASE(lower_integer_and_flag_instructions)
{
    const Instr iaddiu = decodeLower(lowerHi(0x08, 0, 3, 0, 9));
    CHECK_EQ(instrText(iaddiu, 0u), "IADDIU vi03, vi00, 9");
    CHECK(iaddiu.pipeline == Pipeline::Ialu);
    CHECK_EQ(iaddiu.latency, 1);
    CHECK_EQ(iaddiu.viRead, 0);
    CHECK_EQ(iaddiu.viWrite, 1u << 3);
    CHECK(iaddiu.delaysNextBranchRead);
    CHECK_EQ(iaddiu.imm, 9);

    // The immediate is 15 bits, never negative: bits 24..21 above bits 10..0.
    const Instr iaddiuMax = decodeLower(lowerHi(0x08, 0xF, 1, 1, 0x7FF));
    CHECK_EQ(instrText(iaddiuMax, 0u), "IADDIU vi01, vi01, 32767 (0x7FFF)");
    CHECK_EQ(iaddiuMax.imm, 0x7FFF);
    CHECK_EQ(lowerText(lowerHi(0x09, 0, 2, 3, 0x100)), "ISUBIU vi02, vi03, 256 (0x100)");
    CHECK_EQ(lowerText(lowerHi(0x09, 0, 2, 3, 0xFF)), "ISUBIU vi02, vi03, 255");
    CHECK_EQ(decodeLower(lowerHi(0x08, 0x1, 1, 1, 0)).imm, 0x800);

    const Instr iadd = decodeLower(lowerDirect(0x30, 2, 1, 3));
    CHECK_EQ(instrText(iadd, 0u), "IADD vi03, vi01, vi02");
    CHECK_EQ(iadd.viRead, (1u << 1) | (1u << 2));
    CHECK_EQ(iadd.viWrite, 1u << 3);
    CHECK(iadd.delaysNextBranchRead);
    CHECK_EQ(lowerText(lowerDirect(0x31, 2, 1, 3)), "ISUB vi03, vi01, vi02");
    CHECK_EQ(lowerText(lowerDirect(0x34, 2, 1, 3)), "IAND vi03, vi01, vi02");
    CHECK_EQ(lowerText(lowerDirect(0x35, 2, 1, 3)), "IOR vi03, vi01, vi02");
    // VI00 is never declared.
    CHECK_EQ(decodeLower(lowerDirect(0x30, 0, 0, 0)).viRead, 0);
    CHECK_EQ(decodeLower(lowerDirect(0x30, 0, 0, 0)).viWrite, 0);

    const Instr iaddi = decodeLower(lowerDirect(0x32, 2, 1, 0x1F));
    CHECK_EQ(instrText(iaddi, 0u), "IADDI vi02, vi01, -1");
    CHECK_EQ(iaddi.imm, -1);
    CHECK_EQ(iaddi.viWrite, 1u << 2);
    CHECK_EQ(decodeLower(lowerDirect(0x32, 2, 1, 0x0F)).imm, 15);
    CHECK_EQ(decodeLower(lowerDirect(0x32, 2, 1, 0x10)).imm, -16);

    const Instr fcand = decodeLower((0x12u << 25) | 0x03FFFFu);
    CHECK_EQ(instrText(fcand, 0u), "FCAND vi01, 0x03FFFF");
    CHECK_EQ(fcand.viWrite, 1u << 1);
    CHECK(fcand.readsClip && fcand.readsClipUsage);
    CHECK_EQ(fcand.latency, 1);
    CHECK_EQ(fcand.imm, 0x03FFFF);
    CHECK_EQ(lowerText((0x10u << 25) | 0xFFFFFFu), "FCEQ vi01, 0xFFFFFF");
    CHECK_EQ(lowerText((0x13u << 25) | 0x000001u), "FCOR vi01, 0x000001");

    const Instr fcset = decodeLower((0x11u << 25) | 0xABCDEFu);
    CHECK_EQ(instrText(fcset, 0u), "FCSET 0xABCDEF");
    CHECK(fcset.pipeline == Pipeline::Fmac);
    CHECK_EQ(fcset.latency, 4);
    CHECK(fcset.writesClip && fcset.writesClipUsage);
    CHECK_EQ(fcset.viWrite, 0);

    const Instr fsand = decodeLower(lowerHi(0x16, 0x1, 5, 0, 0x00F));
    CHECK_EQ(instrText(fsand, 0u), "FSAND vi05, 0x80F");
    CHECK(fsand.readsStatus);
    CHECK_EQ(fsand.viWrite, 1u << 5);
    CHECK_EQ(lowerText(lowerHi(0x14, 0, 5, 0, 0x001)), "FSEQ vi05, 0x001");
    CHECK_EQ(lowerText(lowerHi(0x17, 0, 5, 0, 0x7FF)), "FSOR vi05, 0x7FF");
    const Instr fsset = decodeLower(lowerHi(0x15, 0, 0, 0, 0x0C0));
    CHECK_EQ(instrText(fsset, 0u), "FSSET 0x0C0");
    CHECK(fsset.writesSticky);
    CHECK(fsset.pipeline == Pipeline::Fmac);

    const Instr fmand = decodeLower(lowerHi(0x1A, 0, 2, 3, 0));
    CHECK_EQ(instrText(fmand, 0u), "FMAND vi02, vi03");
    CHECK(fmand.readsMac);
    CHECK_EQ(fmand.viRead, 1u << 3);
    CHECK_EQ(fmand.viWrite, 1u << 2);
    CHECK_EQ(lowerText(lowerHi(0x18, 0, 2, 3, 0)), "FMEQ vi02, vi03");
    CHECK_EQ(lowerText(lowerHi(0x1B, 0, 2, 3, 0)), "FMOR vi02, vi03");
    const Instr fcget = decodeLower(lowerHi(0x1C, 0, 4, 0, 0));
    CHECK_EQ(instrText(fcget, 0u), "FCGET vi04");
    CHECK(fcget.readsClip);
    CHECK_EQ(fcget.viWrite, 1u << 4);

    const Instr mtir = decodeLower(lowerSpecial(0x3C, 0x3, 5, 20));
    CHECK_EQ(instrText(mtir, 0u), "MTIR vi05, vf20w");
    checkVfRead(mtir, 0, 20, 0x1);
    CHECK_EQ(mtir.viWrite, 1u << 5);
    CHECK(mtir.delaysNextBranchRead);
    const Instr mfir = decodeLower(lowerSpecial(0x3D, 0x1, 12, 1));
    CHECK_EQ(instrText(mfir, 0u), "MFIR.w vf12, vi01");
    CHECK_EQ(mfir.viRead, 1u << 1);
    CHECK_EQ(mfir.vfWrite.reg, 12);
    CHECK_EQ(mfir.latency, 4);
    CHECK(!mfir.delaysNextBranchRead);

    const Instr xtop = decodeLower(lowerSpecial(0x68, 0, 2, 0));
    CHECK_EQ(instrText(xtop, 0u), "XTOP vi02");
    CHECK(xtop.xtop && !xtop.xitop);
    CHECK_EQ(xtop.viWrite, 1u << 2);
    CHECK(!xtop.delaysNextBranchRead);
    CHECK_EQ(lowerText(lowerSpecial(0x69, 0, 3, 0)), "XITOP vi03");
    CHECK(decodeLower(lowerSpecial(0x69, 0, 3, 0)).xitop);
}

TEST_CASE(lower_branches)
{
    const Instr b = decodeLower(lowerHi(0x20, 0, 0, 0, 1));
    CHECK_EQ(instrText(b, 0x40u), "B 0x0050");
    CHECK(b.branch && !b.conditional && b.hasTarget);
    CHECK(b.pipeline == Pipeline::Branch);
    CHECK_EQ(b.latency, 0);
    CHECK_EQ(lowerText(lowerHi(0x20, 0, 0, 0, -3), 0x10u), "B 0x0000");
    CHECK_EQ(lowerText(lowerHi(0x20, 0, 0, 0, -2), 0x0u), "B 0x3FF8");
    CHECK_EQ(lowerText(lowerHi(0x20, 0, 0, 0, 0), 0x3FF8u), "B 0x0000");

    const Instr bal = decodeLower(lowerHi(0x21, 0, 15, 0, 31));
    CHECK_EQ(instrText(bal, 0u), "BAL vi15, 0x0100");
    CHECK_EQ(bal.linkReg, 15);
    CHECK_EQ(bal.viWrite, 1u << 15);
    CHECK_EQ(bal.latency, 1);
    CHECK(!bal.delaysNextBranchRead);

    const Instr jr = decodeLower(lowerHi(0x24, 0, 0, 15, 0));
    CHECK_EQ(instrText(jr, 0u), "JR vi15");
    CHECK_EQ(jr.jumpReg, 15);
    CHECK_EQ(jr.linkReg, -1);
    CHECK_EQ(jr.viRead, 1u << 15);
    CHECK(jr.branch && !jr.hasTarget);

    const Instr jalr = decodeLower(lowerHi(0x25, 0, 15, 14, 0));
    CHECK_EQ(instrText(jalr, 0u), "JALR vi15, vi14");
    CHECK_EQ(jalr.jumpReg, 14);
    CHECK_EQ(jalr.linkReg, 15);
    CHECK_EQ(jalr.viRead, 1u << 14);
    CHECK_EQ(jalr.viWrite, 1u << 15);
    CHECK_EQ(jalr.latency, 1);

    const Instr ibne = decodeLower(lowerHi(0x29, 0, 4, 3, -4));
    CHECK_EQ(instrText(ibne, 0x20u), "IBNE vi04, vi03, 0x0008");
    CHECK(ibne.conditional && ibne.hasTarget);
    CHECK_EQ(ibne.viRead, (1u << 3) | (1u << 4));
    CHECK_EQ(ibne.viWrite, 0);
    CHECK_EQ(ibne.imm, -4);
    CHECK_EQ(lowerText(lowerHi(0x28, 0, 14, 0, 2), 0x0u), "IBEQ vi14, vi00, 0x0018");

    CHECK_EQ(lowerText(lowerHi(0x2C, 0, 0, 5, 2), 0x0u), "IBLTZ vi05, 0x0018");
    CHECK_EQ(lowerText(lowerHi(0x2D, 0, 0, 5, 2), 0x0u), "IBGTZ vi05, 0x0018");
    CHECK_EQ(lowerText(lowerHi(0x2E, 0, 0, 5, 2), 0x0u), "IBLEZ vi05, 0x0018");
    const Instr ibgez = decodeLower(lowerHi(0x2F, 0, 0, 5, 2));
    CHECK_EQ(instrText(ibgez, 0u), "IBGEZ vi05, 0x0018");
    CHECK_EQ(ibgez.viRead, 1u << 5);
    CHECK(ibgez.conditional);
}

TEST_CASE(lower_divider_efu_and_the_rest)
{
    const Instr div = decodeLower(lowerSpecial(0x38, 0xF, 5, 0));
    CHECK_EQ(instrText(div, 0u), "DIV Q, vf00w, vf05w");
    CHECK(div.pipeline == Pipeline::Fdiv);
    CHECK_EQ(div.latency, 7);
    CHECK_EQ(div.writesQ, 7);
    checkVfRead(div, 0, 0, 0x1);
    checkVfRead(div, 1, 5, 0x1);
    CHECK_EQ(div.fsf, 3);
    CHECK_EQ(div.ftf, 3);

    const Instr sqrt = decodeLower(lowerSpecial(0x39, 0x4, 3, 0));
    CHECK_EQ(instrText(sqrt, 0u), "SQRT Q, vf03y");
    CHECK_EQ(sqrt.vfReadCount, 1);
    checkVfRead(sqrt, 0, 3, 0x4);
    CHECK_EQ(sqrt.writesQ, 7);

    const Instr rsqrt = decodeLower(lowerSpecial(0x3A, 0x8, 2, 1));
    CHECK_EQ(instrText(rsqrt, 0u), "RSQRT Q, vf01x, vf02z");
    CHECK_EQ(rsqrt.latency, 13);
    CHECK_EQ(rsqrt.writesQ, 13);
    checkVfRead(rsqrt, 0, 1, 0x8);
    checkVfRead(rsqrt, 1, 2, 0x2);

    const Instr waitq = decodeLower(lowerSpecial(0x3B, 0, 0, 0));
    CHECK_EQ(instrText(waitq, 0u), "WAITQ");
    CHECK(waitq.waitQ);
    CHECK(waitq.pipeline == Pipeline::Fdiv);
    CHECK_EQ(waitq.latency, 0);
    CHECK_EQ(waitq.writesQ, 0);

    struct Efu
    {
        uint32_t special;
        const char *text;
        uint32_t lanes;
        uint32_t latency;
    };
    // fs = vf05, fsf = w.
    const Efu efu[] = {
        {0x70, "ESADD P, vf05", 0xE, 11},
        {0x71, "ERSADD P, vf05", 0xE, 18},
        {0x72, "ELENG P, vf05", 0xE, 18},
        {0x73, "ERLENG P, vf05", 0xE, 24},
        {0x74, "EATANxy P, vf05", 0xC, 54},
        {0x75, "EATANxz P, vf05", 0xA, 54},
        {0x76, "ESUM P, vf05", 0xF, 12},
        {0x77, "ERSQRT P, vf05w", 0x1, 18},
        {0x78, "ESQRT P, vf05w", 0x1, 12},
        {0x79, "ESIN P, vf05w", 0x1, 29},
        {0x7A, "ERCPR P, vf05w", 0x1, 12},
        {0x7C, "EATAN P, vf05w", 0x1, 54},
        {0x7D, "EEXP P, vf05w", 0x1, 44},
    };
    for (const Efu &expected : efu)
    {
        const Instr instr = decodeLower(lowerSpecial(expected.special, 0x3, 0, 5));
        CHECK_EQ(instrText(instr, 0u), expected.text);
        CHECK(instr.pipeline == Pipeline::Efu);
        CHECK_EQ(instr.latency, expected.latency);
        CHECK_EQ(instr.writesP, expected.latency);
        checkVfRead(instr, 0, 5, expected.lanes);
        CHECK(!instr.waitP && !instr.reserved);
    }
    const Instr waitp = decodeLower(lowerSpecial(0x7B, 0, 0, 0));
    CHECK_EQ(instrText(waitp, 0u), "WAITP");
    CHECK(waitp.waitP);
    CHECK(waitp.pipeline == Pipeline::Efu);
    CHECK_EQ(waitp.writesP, 0);

    const Instr mfp = decodeLower(lowerSpecial(0x64, 0x1, 12, 0));
    CHECK_EQ(instrText(mfp, 0u), "MFP.w vf12, P");
    CHECK(mfp.readsP);
    CHECK_EQ(mfp.vfWrite.reg, 12);
    CHECK_EQ(mfp.latency, 4);

    const Instr xgkick = decodeLower(lowerSpecial(0x6C, 0, 0, 6));
    CHECK_EQ(xgkick.word, 0x800036FCu);
    CHECK_EQ(instrText(xgkick, 0u), "XGKICK vi06");
    CHECK(xgkick.pipeline == Pipeline::Xgkick);
    CHECK_EQ(xgkick.latency, 2);
    CHECK_EQ(xgkick.kickReg, 6);
    CHECK_EQ(xgkick.viRead, 1u << 6);

    const Instr move = decodeLower(lowerSpecial(0x30, 0xF, 2, 1));
    CHECK_EQ(instrText(move, 0u), "MOVE.xyzw vf02, vf01");
    checkVfRead(move, 0, 1, 0xF);
    CHECK_EQ(move.vfWrite.reg, 2);
    CHECK(!move.nop);
    const Instr moveNothing = decodeLower(lowerSpecial(0x30, 0x0, 2, 1));
    CHECK_EQ(instrText(moveNothing, 0u), "MOVE vf02, vf01");
    CHECK(moveNothing.nop);
    const Instr mr32 = decodeLower(lowerSpecial(0x31, 0x3, 26, 0));
    CHECK_EQ(instrText(mr32, 0u), "MR32.zw vf26, vf00");
    checkVfRead(mr32, 0, 0, 0xF);
    CHECK_EQ(mr32.vfWrite.lanes, 0x3);

    const Instr rnext = decodeLower(lowerSpecial(0x40, 0x8, 1, 0));
    CHECK_EQ(instrText(rnext, 0u), "RNEXT.x vf01, R");
    CHECK(rnext.readsR && rnext.writesR);
    const Instr rget = decodeLower(lowerSpecial(0x41, 0x8, 1, 0));
    CHECK_EQ(instrText(rget, 0u), "RGET.x vf01, R");
    CHECK(rget.readsR && !rget.writesR);
    const Instr rinit = decodeLower(lowerSpecial(0x42, 0x0, 0, 1));
    CHECK_EQ(instrText(rinit, 0u), "RINIT R, vf01x");
    CHECK(!rinit.readsR && rinit.writesR);
    const Instr rxor = decodeLower(lowerSpecial(0x43, 0x1, 0, 2));
    CHECK_EQ(instrText(rxor, 0u), "RXOR R, vf02y");
    CHECK(rxor.readsR && rxor.writesR);
    checkVfRead(rxor, 0, 2, 0x4);
}

TEST_CASE(lower_skipped_and_reserved_words)
{
    for (const uint32_t word : {0x00000000u, 0x8000033Cu})
    {
        const Instr nop = decodeLower(word);
        CHECK(nop.nop && !nop.reserved);
        CHECK(nop.op == Op::Nop);
        CHECK(nop.pipeline == Pipeline::None);
        CHECK(!nop.hasFields);
    }
    CHECK_EQ(lowerText(0x00000000u), "NOP(0)");
    CHECK_EQ(lowerText(0x8000033Cu), "NOP");
    CHECK_EQ(lowerSpecial(0x30, 0, 0, 0), 0x8000033Cu);

    CHECK_EQ(lowerText(lowerHi(0x02, 0, 0, 0, 0)), "RESERVED lower opHi 0x02 (04000000)");
    CHECK_EQ(lowerText(0x70000000u), "RESERVED lower opHi 0x38 (70000000)");
    CHECK_EQ(lowerText(lowerDirect(0x33, 0, 0, 0)), "RESERVED lower1 funct 0x33 (80000033)");
    CHECK_EQ(lowerText(lowerSpecial(0x33, 0, 0, 0)), "RESERVED lower special 0x33 (8000033F)");

    unsigned validOpHi = 0;
    for (uint32_t opHi = 0; opHi < 0x80u; ++opHi)
    {
        if (opHi == 0x40u)
            continue;
        validOpHi += decodeLower((opHi << 25) | 0x00010800u).reserved ? 0u : 1u;
    }
    CHECK_EQ(validOpHi, 28u);
    unsigned validDirect = 0;
    for (uint32_t funct = 0; funct < 0x3Cu; ++funct)
        validDirect += decodeLower(lowerDirect(funct, 1, 1, 1)).reserved ? 0u : 1u;
    CHECK_EQ(validDirect, 5u);
    unsigned validSpecial = 0;
    for (uint32_t special = 0; special < 0x80u; ++special)
        validSpecial += decodeLower(lowerSpecial(special, 0xF, 1, 1)).reserved ? 0u : 1u;
    CHECK_EQ(validSpecial, 36u);
}

TEST_CASE(pair_bits_and_the_shadow_register)
{
    const Pair loi = decodePair(0x100u, 0x4B0000FFu, 0x80000000u | kUpperNop);
    CHECK(loi.iBit && !loi.eBit && !loi.mBit && !loi.dBit && !loi.tBit);
    CHECK(loi.lower.op == Op::Loi);
    CHECK_EQ(instrText(loi.lower, loi.pc), "LOI 8388863.0 (0x4B0000FF)");
    CHECK(loi.lower.pipeline == Pipeline::None);
    CHECK_EQ(loi.lower.viWrite, 0);
    CHECK(!loi.lower.reserved && !loi.reserved);
    CHECK_EQ(loi.index(), 0x20u);

    // With the I bit the lower word is never an instruction, whatever it looks like.
    const Pair loiReserved = decodePair(0u, 0x70000000u, 0x80000000u | kUpperNop);
    CHECK(!loiReserved.reserved);

    const Pair bits = decodePair(0u, 0u, 0x78000000u | kUpperNop);
    CHECK(!bits.iBit && bits.eBit && bits.mBit && bits.dBit && bits.tBit);
    CHECK(bits.upper.nop);

    // The lower reads the register the upper writes: it runs on the old value.
    const Pair reads = decodePair(0u, lowerHi(0x01, 0xF, 4, 5, 0), upperMain(0x28, 0xF, 2, 1, 5));
    CHECK_EQ(reads.upperVfShadowReg, 5);
    CHECK_EQ(reads.suppressedLowerVf, 0);
    // The lower writes it too: its write is dropped.
    const Pair writes = decodePair(0u, lowerHi(0x00, 0x8, 5, 4, 0), upperMain(0x28, 0x1, 2, 1, 5));
    CHECK_EQ(writes.upperVfShadowReg, 5);
    CHECK_EQ(writes.suppressedLowerVf, 5);
    // Different registers, or an upper write to VF00: nothing.
    const Pair apart = decodePair(0u, lowerHi(0x01, 0xF, 4, 6, 0), upperMain(0x28, 0xF, 2, 1, 5));
    CHECK_EQ(apart.upperVfShadowReg, 0);
    const Pair zero = decodePair(0u, lowerSpecial(0x38, 0xF, 5, 0), upperMain(0x2C, 0x1, 2, 1, 0));
    CHECK_EQ(zero.upperVfShadowReg, 0);
    CHECK_EQ(zero.suppressedLowerVf, 0);

    const Pair reserved = decodePair(0u, 0x70000000u, kUpperNop);
    CHECK(reserved.reserved && reserved.lower.reserved && !reserved.upper.reserved);
    CHECK(decodePair(0u, 0u, 0x00000030u).reserved);
}

TEST_CASE(program_is_little_endian_pairs)
{
    const uint8_t code[20] = {0x09, 0x00, 0x03, 0x10, 0xFF, 0x02, 0x00, 0x00,
                              0x3C, 0x03, 0x00, 0x80, 0xFF, 0x02, 0x00, 0x40,
                              0xAA, 0xBB, 0xCC, 0xDD};
    const std::vector<Pair> pairs = decodeProgram(code, sizeof(code));
    CHECK_EQ(pairs.size(), 2u);
    CHECK_EQ(pairs[0].pc, 0u);
    CHECK_EQ(pairs[0].lowerWord, 0x10030009u);
    CHECK_EQ(pairs[0].upperWord, 0x000002FFu);
    CHECK_EQ(instrText(pairs[0].lower, pairs[0].pc), "IADDIU vi03, vi00, 9");
    CHECK_EQ(pairs[1].pc, 8u);
    CHECK(pairs[1].eBit);
    CHECK(pairs[1].lower.nop);
}

TEST_CASE(float_notation_is_pythons)
{
    struct Expected
    {
        uint32_t bits;
        const char *text;
    };
    // repr() of the float32 with these bits, from Python 3.
    const Expected expected[] = {
        {0x00000000u, "0.0"},
        {0x80000000u, "-0.0"},
        {0x3F800000u, "1.0"},
        {0xBF800000u, "-1.0"},
        {0x4B0000FFu, "8388863.0"},
        {0x3DCCCCCDu, "0.10000000149011612"},
        {0x3E99999Au, "0.30000001192092896"},
        {0x00000001u, "1.401298464324817e-45"},
        {0x80000001u, "-1.401298464324817e-45"},
        {0x007FFFFFu, "1.1754942106924411e-38"},
        {0x00800000u, "1.1754943508222875e-38"},
        {0x7F7FFFFFu, "3.4028234663852886e+38"},
        {0xFF7FFFFFu, "-3.4028234663852886e+38"},
        {0x7F800000u, "inf"},
        {0xFF800000u, "-inf"},
        {0x7FC00000u, "nan"},
        {0xFFC00001u, "nan"},
        {0x7F800001u, "nan"},
        {0x461C4000u, "10000.0"},
        {0x501502F9u, "10000000000.0"},
        {0x5A0E1BCAu, "1.0000000272564224e+16"},
        {0x5D5E0B6Bu, "9.999999843067494e+17"},
        {0x38D1B717u, "9.999999747378752e-05"},
        {0x3A83126Fu, "0.0010000000474974513"},
        {0x3727C5ACu, "9.999999747378752e-06"},
        {0x358637BDu, "9.999999974752427e-07"},
        {0x42F6E979u, "123.45600128173828"},
        {0x40490FDBu, "3.1415927410125732"},
        {0x322BCC77u, "9.99999993922529e-09"},
        {0x6258D727u, "1.0000000200408773e+21"},
        {0x1E3CE508u, "9.999999682655225e-21"},
        {0xC2C80000u, "-100.0"},
        {0x4E6E6B28u, "1000000000.0"},
        {0x58635FA9u, "999999986991104.0"},
        {0x5A635FA9u, "1.5999999791857664e+16"},
        {0x59B1A2BCu, "6249999901917184.0"},
        {0x5BB1A2BCu, "9.999999843067494e+16"},
        {0x5C0E1BCAu, "1.600000043610276e+17"},
        {0x3C23D70Au, "0.009999999776482582"},
        {0x3951B717u, "0.00019999999494757503"},
        {0x391D4952u, "0.0001500000071246177"},
    };
    for (const Expected &entry : expected)
    {
        float value = 0.0f;
        std::memcpy(&value, &entry.bits, sizeof(value));
        CHECK_EQ(floatRepr(static_cast<double>(value)), entry.text);
    }
    CHECK_EQ(floatRepr(0.1), "0.1");
    CHECK_EQ(floatRepr(1e16), "1e+16");
    CHECK_EQ(floatRepr(1e15), "1000000000000000.0");
    CHECK_EQ(floatRepr(0.0001), "0.0001");
    CHECK_EQ(floatRepr(0.00001), "1e-05");
    CHECK_EQ(floatRepr(-2.5e-7), "-2.5e-07");
    CHECK_EQ(floatRepr(1.5e300), "1.5e+300");
}

TEST_CASE(names_of_lanes_and_pipelines)
{
    CHECK_EQ(lanesText(0xF), "xyzw");
    CHECK_EQ(lanesText(0xA), "xz");
    CHECK_EQ(lanesText(0x1), "w");
    CHECK_EQ(lanesText(0x0), "");
    CHECK_EQ(laneOfComponent(0), 8);
    CHECK_EQ(laneOfComponent(3), 1);
    CHECK_EQ(std::string(pipelineName(Pipeline::None)), "none");
    CHECK_EQ(std::string(pipelineName(Pipeline::Xgkick)), "xgkick");
}
