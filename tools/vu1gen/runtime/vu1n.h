// What the code vu1gen generates includes (after upstream's runtime/ps2_vu1.h, for VU1State).
//
// The interface (vu1n_api.h), the MXCSR the helpers are written for, the registers of a launch (Vu1nRegs), and the
// helpers every emitted instruction goes through: arithmetic, data memory, the rings of timed values.
//
// Arithmetic, in two implementations behind one set of names, selected when the generated library is compiled (one
// implementation for all of its translation units):
//   VU1N_MATH_REFERENCE  scalar, lane by lane, written straight from upstream's interpreter (ps2_vu1_upper.cpp,
//                        ps2_vu1_lower.cpp, ps2_vu1_core.cpp): operands normalised (exponent 0 -> signed zero,
//                        exponent 255 -> signed 0x7F7FFFFF), float32 operations rounded toward zero, a product-sum as
//                        two rounded operations, the result fixed up from the same expression in binary64. Needs round
//                        toward zero only (kVu1nMxcsr below).
//   default              SSE without FMA, under round toward zero + flush to zero + denormals are zero, with
//                        explicit code for what those modes do not give. NOT WRITTEN YET (the next stage, PROGRESS.md):
//                        generated code (VU1N_GENERATED) must be compiled with VU1N_MATH_REFERENCE until then.
//
// Rules for a translation unit with generated arithmetic: compiled /fp:precise (never /fp:fast), no contraction of
// a multiply and an add, no arithmetic on two literals (the compiler would fold it under the default rounding), and
// it never changes MXCSR itself: vu1nRun sets Vu1nProgram::mxcsr around the whole launch.
#ifndef MKSM_VU1N_H
#define MKSM_VU1N_H

#include "vu1n_api.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(VU1N_GENERATED) && !defined(VU1N_MATH_REFERENCE)
#error "vu1n.h: only the reference arithmetic exists so far; compile the generated code with VU1N_MATH_REFERENCE"
#endif

// MXCSR during a launch, exceptions masked. The table vu1gen generates stores it in Vu1nProgram::mxcsr.
#if defined(VU1N_MATH_REFERENCE)
constexpr uint32_t kVu1nMxcsr = 0x7F80u; // round toward zero; denormals as IEEE, which is the interpreter's environment
#else
constexpr uint32_t kVu1nMxcsr = 0xFFC0u; // round toward zero, flush to zero, denormals are zero
#endif

#if defined(_MSC_VER)
#define VU1N_INLINE __forceinline
#else
#define VU1N_INLINE inline __attribute__((always_inline))
#endif

// Byte offset of a qword of VU1 data memory. The interpreter computes ((vi + imm) * 16) & 0x3FFF for LQ, SQ, ILW and
// ISW and ((uint16_t)vi * 16) & 0x3FFF for the register forms and XGKICK: both are this, and nothing else bounds an
// access.
VU1N_INLINE constexpr uint32_t vu1nQwordOffset(int32_t qwordAddress)
{
    return (static_cast<uint32_t>(qwordAddress) & 0x3FFu) * 16u;
}

// A launch that goes this many times through backward branches is stopped (the interpreter's budget is 65536 cycles
// and every pair takes at least one, so the interpreter would have stopped it before): the state is stored with pc at
// the branch target, which is not an entry, so the next MSCNT goes to the interpreter.
constexpr uint32_t kVu1nLoopBudget = 0x10000u;

struct Vu1nVec
{
    float v[4];
};

// The registers of a launch. The rings hold the values the timed reads choose from: q[0] is the result of the last
// division executed, q[1] the one before; the same for p (EFU results), mac (the MAC flags of the flag-writing
// instructions) and clip (clip[0] is the working clip value, clip[k] the value k clip writers ago).
struct Vu1nRegs
{
    float vf[32][4];
    float acc[4];
    int32_t vi[16];
    float q[2];
    float p[3];
    uint32_t mac[8];
    uint32_t clip[8];
    float i;
    uint32_t r;
    uint32_t branchTarget;
};

VU1N_INLINE uint32_t vu1nBits(float value) { return std::bit_cast<uint32_t>(value); }
VU1N_INLINE float vu1nFloat(uint32_t bits) { return std::bit_cast<float>(bits); }
VU1N_INLINE int32_t vu1nS16(int32_t value) { return static_cast<int32_t>(static_cast<int16_t>(value)); }
// std::signbit, without the call MSVC makes for it.
VU1N_INLINE bool vu1nSignbit(float value) { return (vu1nBits(value) >> 31) != 0u; }
VU1N_INLINE bool vu1nSignbit(double value) { return (std::bit_cast<uint64_t>(value) >> 63) != 0u; }

VU1N_INLINE Vu1nVec vu1nSplat(float value) { return Vu1nVec{{value, value, value, value}}; }

VU1N_INLINE void vu1nApply(float *dst, const float *src, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = src[0];
    if (dest & 0x4u)
        dst[1] = src[1];
    if (dest & 0x2u)
        dst[2] = src[2];
    if (dest & 0x1u)
        dst[3] = src[3];
}

template <int Depth, class T>
VU1N_INLINE void vu1nPush(T *ring, T value)
{
    for (int index = Depth - 1; index > 0; --index)
        ring[index] = ring[index - 1];
    ring[0] = value;
}

// ---- state ------------------------------------------------------------------------------------------------------

template <class State>
VU1N_INLINE void vu1nEnter(Vu1nRegs &regs, const State &state)
{
    std::memcpy(regs.vf, state.vf, sizeof(regs.vf));
    regs.vf[0][0] = 0.0f;
    regs.vf[0][1] = 0.0f;
    regs.vf[0][2] = 0.0f;
    regs.vf[0][3] = 1.0f;
    std::memcpy(regs.acc, state.acc, sizeof(regs.acc));
    std::memcpy(regs.vi, state.vi, sizeof(regs.vi));
    regs.vi[0] = 0;
    for (float &value : regs.q)
        value = state.q;
    for (float &value : regs.p)
        value = state.p;
    for (uint32_t &value : regs.mac)
        value = state.mac;
    for (uint32_t &value : regs.clip)
        value = state.clip;
    regs.i = state.i;
    regs.r = state.r;
    regs.branchTarget = state.branchTarget;
}

// The end of a launch: every pending value committed (flushPipelines), pc at the continuation.
template <bool KeepsMac, class State>
VU1N_INLINE void vu1nLeave(State &state, const Vu1nRegs &regs, uint32_t pc)
{
    std::memcpy(state.vf[1], regs.vf[1], sizeof(float) * 4u * 31u);
    std::memcpy(state.acc, regs.acc, sizeof(regs.acc));
    std::memcpy(&state.vi[1], &regs.vi[1], sizeof(int32_t) * 15u);
    state.q = regs.q[0];
    state.p = regs.p[0];
    if constexpr (KeepsMac)
        state.mac = regs.mac[0];
    state.clip = regs.clip[0];
    state.i = regs.i;
    state.r = regs.r;
    state.pc = pc;
    state.branchPending = false;
    state.branchTarget = regs.branchTarget;
    state.branchDelay = 0u;
}

// ---- operands and results (ps2_vu1_core.cpp: normalizeOperand, normalizeResult, normalizeFmacExactResult) --------

VU1N_INLINE float vu1nN(float value)
{
    uint32_t bits = vu1nBits(value);
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
        bits &= 0x80000000u;
    else if (exponent == 0xFFu)
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    return vu1nFloat(bits);
}

// normalizeResult without the flags: what queueQ and queueP store.
VU1N_INLINE float vu1nNormalizeResult(float value)
{
    uint32_t bits = vu1nBits(value);
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (magnitude != 0u && exponent == 0u)
        bits = sign;
    else if (exponent == 0xFFu)
        bits = sign | 0x7F7FFFFFu;
    return vu1nFloat(bits);
}

// The fix-up of one FMAC lane from the exact (binary64) value of its expression. Returns the lane's flags: Z 1, S 2,
// U 4, O 8.
VU1N_INLINE uint32_t vu1nFix(float &value, double exact)
{
    const bool negative = vu1nSignbit(exact);
    const double magnitude = std::fabs(exact);
    uint32_t flags = negative ? 0x2u : 0u;
    const uint32_t sign = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0)
    {
        flags |= 0x1u;
        value = vu1nFloat(sign);
    }
    else if (magnitude > static_cast<double>(std::numeric_limits<float>::max()))
    {
        flags |= 0x8u;
        value = vu1nFloat(sign | 0x7F7FFFFFu);
    }
    else if (magnitude < static_cast<double>(std::numeric_limits<float>::min()))
    {
        flags |= 0x5u;
        value = vu1nFloat(sign);
    }
    return flags;
}

VU1N_INLINE uint32_t vu1nMacBits(uint32_t flags, uint32_t lane)
{
    uint32_t mac = 0u;
    if (flags & 0x1u)
        mac |= lane;
    if (flags & 0x2u)
        mac |= lane << 4;
    if (flags & 0x4u)
        mac |= lane << 8;
    if (flags & 0x8u)
        mac |= lane << 12;
    return mac;
}

// ---- upper instructions -----------------------------------------------------------------------------------------

enum : int
{
    kVu1nAdd,
    kVu1nSub,
    kVu1nMul,
    kVu1nMadd,
    kVu1nMsub,
};

// ADD, SUB, MUL, MADD, MSUB in every form: t is the second operand per lane (a broadcast, Q or I is passed splatted).
// Writes the destination lanes of out and returns the MAC flags.
template <int Op>
VU1N_INLINE uint32_t vu1nFmac(uint8_t dest, const float *s, const float *t, const float *acc, float *out)
{
    uint32_t mac = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint32_t lane = 8u >> component;
        if ((dest & lane) == 0u)
            continue;
        const float a = vu1nN(s[component]);
        const float b = vu1nN(t[component]);
        float result;
        double exact;
        if constexpr (Op == kVu1nAdd)
        {
            result = a + b;
            exact = static_cast<double>(a) + static_cast<double>(b);
        }
        else if constexpr (Op == kVu1nSub)
        {
            result = a - b;
            exact = static_cast<double>(a) - static_cast<double>(b);
        }
        else if constexpr (Op == kVu1nMul)
        {
            result = a * b;
            exact = static_cast<double>(a) * static_cast<double>(b);
        }
        else if constexpr (Op == kVu1nMadd)
        {
            const float z = vu1nN(acc[component]);
            const float product = a * b;
            result = z + product;
            exact = static_cast<double>(z) + static_cast<double>(a) * static_cast<double>(b);
        }
        else
        {
            const float z = vu1nN(acc[component]);
            const float product = a * b;
            result = z - product;
            exact = static_cast<double>(z) - static_cast<double>(a) * static_cast<double>(b);
        }
        const uint32_t flags = vu1nFix(result, exact);
        out[component] = result;
        mac |= vu1nMacBits(flags, lane);
    }
    return mac;
}

// OPMULA (Sub false) and OPMSUB (Sub true): lane c of the product is s[(1,2,0)[c]] * t[(2,0,1)[c]]; lane w is 0.
template <bool Sub>
VU1N_INLINE uint32_t vu1nOpm(uint8_t dest, const float *s, const float *t, const float *acc, float *out)
{
    static constexpr uint32_t kLeft[3] = {1u, 2u, 0u};
    static constexpr uint32_t kRight[3] = {2u, 0u, 1u};
    uint32_t mac = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint32_t lane = 8u >> component;
        if ((dest & lane) == 0u)
            continue;
        float result;
        double exact;
        if (component == 3u)
        {
            result = 0.0f;
            exact = 0.0;
        }
        else
        {
            const float a = vu1nN(s[kLeft[component]]);
            const float b = vu1nN(t[kRight[component]]);
            if constexpr (Sub)
            {
                const float z = vu1nN(acc[component]);
                const float product = a * b;
                result = z - product;
                exact = static_cast<double>(z) - static_cast<double>(a) * static_cast<double>(b);
            }
            else
            {
                result = a * b;
                exact = static_cast<double>(a) * static_cast<double>(b);
            }
        }
        const uint32_t flags = vu1nFix(result, exact);
        out[component] = result;
        mac |= vu1nMacBits(flags, lane);
    }
    return mac;
}

// MAX (Max true) and MINI: the normalised operands compared as floats, no flags.
template <bool Max>
VU1N_INLINE void vu1nMinMax(uint8_t dest, const float *s, const float *t, float *out)
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & (8u >> component)) == 0u)
            continue;
        const float a = vu1nN(s[component]);
        const float b = vu1nN(t[component]);
        if constexpr (Max)
            out[component] = a > b ? a : b;
        else
            out[component] = a < b ? a : b;
    }
}

VU1N_INLINE void vu1nAbs(uint8_t dest, const float *s, float *out)
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if (dest & (8u >> component))
            out[component] = std::fabs(vu1nN(s[component]));
    }
}

// ITOF0/4/12/15: the raw word as int32, converted (rounded toward zero), then divided.
template <int Fraction>
VU1N_INLINE void vu1nItof(uint8_t dest, const float *s, float *out)
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & (8u >> component)) == 0u)
            continue;
        const float value = static_cast<float>(static_cast<int32_t>(vu1nBits(s[component])));
        if constexpr (Fraction == 0)
            out[component] = value;
        else if constexpr (Fraction == 4)
            out[component] = value / 16.0f;
        else if constexpr (Fraction == 12)
            out[component] = value / 4096.0f;
        else
            out[component] = value / 32768.0f;
    }
}

// FTOI0/4/12/15 (vuFloatToInt): the normalised operand times the scale in binary64, saturated, truncated.
template <int Fraction>
VU1N_INLINE void vu1nFtoi(uint8_t dest, const float *s, float *out)
{
    constexpr float kScale = Fraction == 0 ? 1.0f : (Fraction == 4 ? 16.0f : (Fraction == 12 ? 4096.0f : 32768.0f));
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & (8u >> component)) == 0u)
            continue;
        const double scaled = static_cast<double>(vu1nN(s[component])) * static_cast<double>(kScale);
        int32_t value;
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            value = std::numeric_limits<int32_t>::max();
        else if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            value = std::numeric_limits<int32_t>::min();
        else
            value = static_cast<int32_t>(scaled);
        out[component] = vu1nFloat(static_cast<uint32_t>(value));
    }
}

// CLIP: the six judgement bits (+x -x +y -y +z -z) on raw bit patterns.
VU1N_INLINE uint32_t vu1nClip(const float *s, float w)
{
    const uint32_t wBits = vu1nBits(w);
    const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
    uint32_t flags = 0u;
    for (uint32_t component = 0; component < 3u; ++component)
    {
        const uint32_t bits = vu1nBits(s[component]);
        if (static_cast<int32_t>(bits) > limit)
            flags |= 1u << (component * 2u);
        if (static_cast<int32_t>(bits ^ 0x80000000u) > limit)
            flags |= 2u << (component * 2u);
    }
    return flags;
}

VU1N_INLINE Vu1nVec vu1nMr32(const float *s)
{
    return Vu1nVec{{s[1], s[2], s[3], s[0]}};
}

// ---- lower instructions: Q and P ----------------------------------------------------------------------------------

VU1N_INLINE float vu1nDiv(float fs, float ft)
{
    const float num = vu1nN(fs);
    const float den = vu1nN(ft);
    float result;
    if (den == 0.0f)
        result = vu1nSignbit(num) != vu1nSignbit(den) ? -std::numeric_limits<float>::max()
                                                        : std::numeric_limits<float>::max();
    else
        result = num / den;
    return vu1nNormalizeResult(result);
}

VU1N_INLINE float vu1nSqrt(float ft)
{
    const float value = vu1nN(ft);
    return vu1nNormalizeResult(std::sqrt(std::fabs(value)));
}

VU1N_INLINE float vu1nRsqrt(float fs, float ft)
{
    const float num = vu1nN(fs);
    const float radicand = vu1nN(ft);
    const float den = std::sqrt(std::fabs(radicand));
    float result;
    if (den != 0.0f)
        result = num / den;
    else
        result = vu1nSignbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    return vu1nNormalizeResult(result);
}

VU1N_INLINE float vu1nEatanPoly(float value)
{
    constexpr float coefficients[] = {0.999999344348907f, -0.333298563957214f, 0.199465364217758f,
                                      -0.13085337519646f, 0.096420042216778f, -0.055909886956215f,
                                      0.021861229091883f, -0.004054057877511f};
    constexpr float quarterPi = 0.785398185253143f;
    const float squared = value * value;
    float polynomial = coefficients[7];
    for (int index = 6; index >= 0; --index)
        polynomial = coefficients[index] + squared * polynomial;
    return quarterPi + value * polynomial;
}

VU1N_INLINE float vu1nEsinPoly(float value)
{
    constexpr float coefficients[] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f,
                                      0.000002601886990f};
    const float squared = value * value;
    float polynomial = coefficients[4];
    for (int index = 3; index >= 0; --index)
        polynomial = coefficients[index] + squared * polynomial;
    return value * polynomial;
}

VU1N_INLINE float vu1nEexpPoly(float value)
{
    constexpr float coefficients[] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
                                      0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
    float polynomial = coefficients[5];
    for (int index = 4; index >= 0; --index)
        polynomial = coefficients[index] + value * polynomial;
    polynomial = 1.0f + value * polynomial;
    polynomial *= polynomial;
    polynomial *= polynomial;
    return polynomial != 0.0f ? 1.0f / polynomial : std::numeric_limits<float>::max();
}

VU1N_INLINE float vu1nSquareSum(const float *s)
{
    const float x = vu1nN(s[0]);
    const float y = vu1nN(s[1]);
    const float z = vu1nN(s[2]);
    return x * x + y * y + z * z;
}

VU1N_INLINE float vu1nEsadd(const float *s) { return vu1nNormalizeResult(vu1nSquareSum(s)); }

VU1N_INLINE float vu1nErsadd(const float *s)
{
    const float sum = vu1nSquareSum(s);
    return vu1nNormalizeResult(sum != 0.0f ? 1.0f / sum : sum);
}

VU1N_INLINE float vu1nEleng(const float *s) { return vu1nNormalizeResult(std::sqrt(vu1nSquareSum(s))); }

VU1N_INLINE float vu1nErleng(const float *s)
{
    const float length = std::sqrt(vu1nSquareSum(s));
    return vu1nNormalizeResult(length != 0.0f ? 1.0f / length : length);
}

VU1N_INLINE float vu1nEatanxy(const float *s)
{
    const float x = vu1nN(s[0]);
    const float y = vu1nN(s[1]);
    return vu1nNormalizeResult(x != 0.0f ? vu1nEatanPoly(y / x) : 0.0f);
}

VU1N_INLINE float vu1nEatanxz(const float *s)
{
    const float x = vu1nN(s[0]);
    const float z = vu1nN(s[2]);
    return vu1nNormalizeResult(x != 0.0f ? vu1nEatanPoly(z / x) : 0.0f);
}

VU1N_INLINE float vu1nEsum(const float *s)
{
    float sum = 0.0f;
    for (uint32_t component = 0; component < 4u; ++component)
        sum += vu1nN(s[component]);
    return vu1nNormalizeResult(sum);
}

VU1N_INLINE float vu1nErsqrt(float fs)
{
    float result = vu1nN(fs);
    if (result >= 0.0f)
    {
        result = std::sqrt(result);
        if (result != 0.0f)
            result = 1.0f / result;
    }
    return vu1nNormalizeResult(result);
}

VU1N_INLINE float vu1nEsqrt(float fs)
{
    const float value = vu1nN(fs);
    return vu1nNormalizeResult(value >= 0.0f ? std::sqrt(value) : value);
}

VU1N_INLINE float vu1nEsin(float fs) { return vu1nNormalizeResult(vu1nEsinPoly(vu1nN(fs))); }

VU1N_INLINE float vu1nErcpr(float fs)
{
    const float value = vu1nN(fs);
    return vu1nNormalizeResult(value != 0.0f ? 1.0f / value : value);
}

VU1N_INLINE float vu1nEatan(float fs) { return vu1nNormalizeResult(vu1nEatanPoly(vu1nN(fs))); }
VU1N_INLINE float vu1nEexp(float fs) { return vu1nNormalizeResult(vu1nEexpPoly(vu1nN(fs))); }

// ---- lower instructions: R ----------------------------------------------------------------------------------------

VU1N_INLINE float vu1nRnext(uint32_t &r)
{
    const uint32_t x = (r >> 4) & 1u;
    const uint32_t y = (r >> 22) & 1u;
    r = ((r << 1) ^ x ^ y) & 0x007FFFFFu;
    r |= 0x3F800000u;
    return vu1nFloat(r);
}

// ---- data memory ----------------------------------------------------------------------------------------------------

VU1N_INLINE void vu1nLq(float *dst, const uint8_t *data, int32_t qword, uint8_t dest)
{
    float value[4];
    std::memcpy(value, data + vu1nQwordOffset(qword), sizeof(value));
    vu1nApply(dst, value, dest);
}

VU1N_INLINE void vu1nSq(uint8_t *data, int32_t qword, const float *src, uint8_t dest)
{
    uint8_t *at = data + vu1nQwordOffset(qword);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if (dest & (8u >> component))
            std::memcpy(at + component * 4u, &src[component], 4u);
    }
}

// ILW, ILWR: the low 16 bits of the first selected lane, sign-extended.
VU1N_INLINE int32_t vu1nIlw(const uint8_t *data, int32_t qword, uint8_t dest)
{
    const uint32_t component = (dest & 0x8u) ? 0u : (dest & 0x4u) ? 1u : (dest & 0x2u) ? 2u : 3u;
    uint32_t word;
    std::memcpy(&word, data + vu1nQwordOffset(qword) + component * 4u, sizeof(word));
    return vu1nS16(static_cast<int32_t>(word & 0xFFFFu));
}

// ISW, ISWR: the VI value zero-extended from 16 bits in each selected lane.
VU1N_INLINE void vu1nIsw(uint8_t *data, int32_t qword, int32_t value, uint8_t dest)
{
    const uint32_t word = static_cast<uint32_t>(static_cast<uint16_t>(value & 0xFFFF));
    uint8_t *at = data + vu1nQwordOffset(qword);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if (dest & (8u >> component))
            std::memcpy(at + component * 4u, &word, 4u);
    }
}

#endif
