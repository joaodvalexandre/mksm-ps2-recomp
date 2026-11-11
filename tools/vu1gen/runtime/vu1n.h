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
//                        two rounded operations, the result fixed up from the same expression in binary64. These are
//                        the vu1nRef* functions.
//   default              SSE4.1 for the upper instructions (see "the public names of the upper helpers"), exact by
//                        construction, with the reference taking the rare lanes it must; the lower instructions are
//                        the same scalar code in both.
// Both run under kVu1nMxcsr.
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

#include <smmintrin.h> // SSE4.1, which the runtime already requires

// MXCSR during a launch, exceptions masked: round toward zero, denormals as IEEE (no flush to zero, no denormals
// are zero), which is the interpreter's own environment. Both implementations use it: with the operands normalised
// explicitly, the SSE operations then round exactly as the interpreter's float code does, and only the binary64
// fix-up of a result needs handling (see the fast helpers). The table vu1gen generates stores it in
// Vu1nProgram::mxcsr.
constexpr uint32_t kVu1nMxcsr = 0x7F80u;

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
VU1N_INLINE uint32_t vu1nRefFmac(uint8_t dest, const float *s, const float *t, const float *acc, float *out)
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
VU1N_INLINE uint32_t vu1nRefOpm(uint8_t dest, const float *s, const float *t, const float *acc, float *out)
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
VU1N_INLINE void vu1nRefMinMax(uint8_t dest, const float *s, const float *t, float *out)
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

VU1N_INLINE void vu1nRefAbs(uint8_t dest, const float *s, float *out)
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if (dest & (8u >> component))
            out[component] = std::fabs(vu1nN(s[component]));
    }
}

// ITOF0/4/12/15: the raw word as int32, converted (rounded toward zero), then divided.
template <int Fraction>
VU1N_INLINE void vu1nRefItof(uint8_t dest, const float *s, float *out)
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
VU1N_INLINE void vu1nRefFtoi(uint8_t dest, const float *s, float *out)
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

// ---- the public names of the upper helpers -------------------------------------------------------------------
//
// Dest is the instruction's lane mask (x = 8, y = 4, z = 2, w = 1). out receives every lane of the result (only the
// destination lanes are meaningful); vu1nCommit<Dest> writes them to the register. The "S" forms take a scalar
// second operand (a broadcast lane, Q or I).
//
// Fast forms (default build), under kVu1nMxcsr (round toward zero, denormals as IEEE):
// - Operands are normalised with integer operations (exponent 0 -> signed zero, exponent 255 -> signed 0x7F7FFFFF),
//   so the SSE operation sees the interpreter's operands and rounds as its float code does.
// - The interpreter then replaces a lane's result from the binary64 value e of the same expression: a zero when e
//   is 0 or below FLT_MIN, 0x7F7FFFFF when |e| exceeds FLT_MAX. For ADD, SUB and MUL of normalised operands that is
//   exactly "a result below FLT_MIN (a denormal or a zero) becomes the zero of its sign": round toward zero already
//   clamps an overflow to 0x7F7FFFFF; an exact value below FLT_MIN gives a denormal or zero result with the exact
//   value's sign on one side and the fix-up's zero of that sign on the other; and FLT_MIN <= |e| means the float
//   result is normal and neither side touches it.
// - For MADD and MSUB (r = z +- p, p = the float product a*b rounded; e = z +- a*b in binary64) the same holds in
//   every lane that is not "slow". With a*b = 0 both sides do the same operation on the same values. With
//   |a*b| >= 2^-79 the exponents of a and b add up to at least -126, so a*b and p are multiples of 2^-126, and a z
//   that cancels them to below 2^-79 is itself at least 2^-79, a multiple of 2^-102: z +- a*b and z +- p are then
//   multiples of 2^-126, so neither can be nonzero and below FLT_MIN, and r is a denormal in no case. And e can
//   exceed FLT_MAX while r does not only when p or r is at or above 2^127 (p below 2^127 is within 2^103 of a*b).
//   Slow lanes, which take the reference: a nonzero product below 2^-79, or a product or result at or above 2^127.
//   The bound 2^-79 is tight: MADD of 0xABFFFFFF and 0x2B7FFFFF on 0x17FFFFFE has a product just below it, r = +0
//   and e = -2^-127, so the interpreter's result is -0 (tests/math_tests.cpp has the case).
namespace vu1n_detail
{
    VU1N_INLINE __m128i iv(int32_t value) { return _mm_set1_epi32(value); }

    // normalizeOperand on four lanes.
    VU1N_INLINE __m128 normalize(__m128 value)
    {
        const __m128i bits = _mm_castps_si128(value);
        const __m128i sign = _mm_and_si128(bits, iv(static_cast<int32_t>(0x80000000u)));
        __m128i magnitude = _mm_and_si128(bits, iv(0x7FFFFFFF));
        magnitude = _mm_min_epi32(magnitude, iv(0x7F7FFFFF));
        magnitude = _mm_and_si128(magnitude, _mm_cmpgt_epi32(magnitude, iv(0x007FFFFF)));
        return _mm_castsi128_ps(_mm_or_si128(magnitude, sign));
    }

    // A result below FLT_MIN (a denormal or a zero) becomes the zero of its sign.
    VU1N_INLINE __m128 flushTiny(__m128 value)
    {
        const __m128i bits = _mm_castps_si128(value);
        const __m128i normal = _mm_cmpgt_epi32(_mm_and_si128(bits, iv(0x7FFFFFFF)), iv(0x007FFFFF));
        return _mm_castsi128_ps(_mm_and_si128(bits, _mm_or_si128(normal, iv(static_cast<int32_t>(0x80000000u)))));
    }

    // Lane i of an SSE register is component i: x is bit 0 of a blend or movemask, and bit 3 of a VU lane mask.
    constexpr int laneBits(int dest)
    {
        return ((dest >> 3) & 1) | (((dest >> 2) & 1) << 1) | (((dest >> 1) & 1) << 2) | ((dest & 1) << 3);
    }

    // The lanes of a product-sum the binary64 fix-up may change (see above).
    VU1N_INLINE int productSumSlowLanes(__m128 a, __m128 b, __m128 p, __m128 r)
    {
        const __m128 magnitudeMask = _mm_castsi128_ps(iv(0x7FFFFFFF));
        const __m128 zero = _mm_setzero_ps();
        const __m128 pMagnitude = _mm_and_ps(p, magnitudeMask);
        const __m128 rMagnitude = _mm_and_ps(r, magnitudeMask);
        const __m128 tiny = _mm_and_ps(_mm_cmplt_ps(pMagnitude, _mm_castsi128_ps(iv(0x18000000))), // 2^-79
                                       _mm_and_ps(_mm_cmpneq_ps(a, zero), _mm_cmpneq_ps(b, zero)));
        const __m128 top = _mm_castsi128_ps(iv(0x7F000000)); // 2^127
        const __m128 huge = _mm_or_ps(_mm_cmpge_ps(pMagnitude, top), _mm_cmpge_ps(rMagnitude, top));
        return _mm_movemask_ps(_mm_or_ps(tiny, huge));
    }

    template <int Op, int Dest>
    VU1N_INLINE void fmac(const float *s, __m128 t, const float *acc, float *out)
    {
        const __m128 a = normalize(_mm_loadu_ps(s));
        const __m128 b = normalize(t);
        __m128 r;
        if constexpr (Op == kVu1nAdd)
            r = _mm_add_ps(a, b);
        else if constexpr (Op == kVu1nSub)
            r = _mm_sub_ps(a, b);
        else if constexpr (Op == kVu1nMul)
            r = _mm_mul_ps(a, b);
        else
        {
            const __m128 z = normalize(_mm_loadu_ps(acc));
            const __m128 p = _mm_mul_ps(a, b);
            r = Op == kVu1nMadd ? _mm_add_ps(z, p) : _mm_sub_ps(z, p);
            if (productSumSlowLanes(a, b, p, r) & laneBits(Dest)) [[unlikely]]
            {
                alignas(16) float tv[4];
                _mm_store_ps(tv, t);
                (void)vu1nRefFmac<Op>(static_cast<uint8_t>(Dest), s, tv, acc, out);
                return;
            }
        }
        _mm_storeu_ps(out, flushTiny(r));
    }

    template <bool Max, int Dest>
    VU1N_INLINE void minMax(const float *s, __m128 t, float *out)
    {
        const __m128 a = normalize(_mm_loadu_ps(s));
        const __m128 b = normalize(t);
        // MAXPS / MINPS return the second operand unless the first compares greater / less: vs > vt ? vs : vt.
        _mm_storeu_ps(out, Max ? _mm_max_ps(a, b) : _mm_min_ps(a, b));
    }
}

template <int Dest>
VU1N_INLINE void vu1nCommit(float *dst, const float *src)
{
    if constexpr (Dest == 0xF)
        std::memcpy(dst, src, 16u);
    else
    {
        const __m128 value = _mm_blend_ps(_mm_loadu_ps(dst), _mm_loadu_ps(src), vu1n_detail::laneBits(Dest));
        _mm_storeu_ps(dst, value);
    }
}

#if defined(VU1N_MATH_REFERENCE)

template <int Op, int Dest>
VU1N_INLINE void vu1nFmac(const float *s, const float *t, const float *acc, float *out)
{
    (void)vu1nRefFmac<Op>(static_cast<uint8_t>(Dest), s, t, acc, out);
}
template <int Op, int Dest>
VU1N_INLINE void vu1nFmacS(const float *s, float t, const float *acc, float *out)
{
    (void)vu1nRefFmac<Op>(static_cast<uint8_t>(Dest), s, vu1nSplat(t).v, acc, out);
}
template <bool Sub, int Dest>
VU1N_INLINE void vu1nOpmFast(const float *s, const float *t, const float *acc, float *out)
{
    (void)vu1nRefOpm<Sub>(static_cast<uint8_t>(Dest), s, t, acc, out);
}
template <bool Max, int Dest>
VU1N_INLINE void vu1nMinMax(const float *s, const float *t, float *out)
{
    vu1nRefMinMax<Max>(static_cast<uint8_t>(Dest), s, t, out);
}
template <bool Max, int Dest>
VU1N_INLINE void vu1nMinMaxS(const float *s, float t, float *out)
{
    vu1nRefMinMax<Max>(static_cast<uint8_t>(Dest), s, vu1nSplat(t).v, out);
}
template <int Dest>
VU1N_INLINE void vu1nAbs(const float *s, float *out)
{
    vu1nRefAbs(static_cast<uint8_t>(Dest), s, out);
}
template <int Fraction, int Dest>
VU1N_INLINE void vu1nItof(const float *s, float *out)
{
    vu1nRefItof<Fraction>(static_cast<uint8_t>(Dest), s, out);
}
template <int Fraction, int Dest>
VU1N_INLINE void vu1nFtoi(const float *s, float *out)
{
    vu1nRefFtoi<Fraction>(static_cast<uint8_t>(Dest), s, out);
}

#else

template <int Op, int Dest>
VU1N_INLINE void vu1nFmac(const float *s, const float *t, const float *acc, float *out)
{
    vu1n_detail::fmac<Op, Dest>(s, _mm_loadu_ps(t), acc, out);
}
template <int Op, int Dest>
VU1N_INLINE void vu1nFmacS(const float *s, float t, const float *acc, float *out)
{
    vu1n_detail::fmac<Op, Dest>(s, _mm_set1_ps(t), acc, out);
}
// OPMULA / OPMSUB with the xyz mask (vu1gen refuses any other): the product-sum of the permuted operands.
template <bool Sub, int Dest>
VU1N_INLINE void vu1nOpmFast(const float *s, const float *t, const float *acc, float *out)
{
    static_assert((Dest & 1) == 0, "OPMULA / OPMSUB with the w lane are refused by vu1gen");
    const __m128 sv = _mm_loadu_ps(s);
    const __m128 tv = _mm_loadu_ps(t);
    alignas(16) float sp[4];
    _mm_store_ps(sp, _mm_shuffle_ps(sv, sv, _MM_SHUFFLE(3, 0, 2, 1))); // (y, z, x, w)
    const __m128 tp = _mm_shuffle_ps(tv, tv, _MM_SHUFFLE(3, 1, 0, 2)); // (z, x, y, w)
    if constexpr (Sub)
    {
        const __m128 a = vu1n_detail::normalize(_mm_load_ps(sp));
        const __m128 b = vu1n_detail::normalize(tp);
        const __m128 z = vu1n_detail::normalize(_mm_loadu_ps(acc));
        const __m128 p = _mm_mul_ps(a, b);
        const __m128 r = _mm_sub_ps(z, p);
        if (vu1n_detail::productSumSlowLanes(a, b, p, r) & vu1n_detail::laneBits(Dest)) [[unlikely]]
        {
            (void)vu1nRefOpm<true>(static_cast<uint8_t>(Dest), s, t, acc, out);
            return;
        }
        _mm_storeu_ps(out, vu1n_detail::flushTiny(r));
    }
    else
        vu1n_detail::fmac<kVu1nMul, Dest>(sp, tp, acc, out);
}
template <bool Max, int Dest>
VU1N_INLINE void vu1nMinMax(const float *s, const float *t, float *out)
{
    vu1n_detail::minMax<Max, Dest>(s, _mm_loadu_ps(t), out);
}
template <bool Max, int Dest>
VU1N_INLINE void vu1nMinMaxS(const float *s, float t, float *out)
{
    vu1n_detail::minMax<Max, Dest>(s, _mm_set1_ps(t), out);
}
template <int Dest>
VU1N_INLINE void vu1nAbs(const float *s, float *out)
{
    const __m128 value = vu1n_detail::normalize(_mm_loadu_ps(s));
    _mm_storeu_ps(out, _mm_and_ps(value, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))));
}
// ITOF: the conversion rounds under kVu1nMxcsr as the interpreter's cast does; the division by a power of two is
// exact (no result is below 2^-15), so it is a multiplication.
template <int Fraction, int Dest>
VU1N_INLINE void vu1nItof(const float *s, float *out)
{
    __m128 value = _mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i *>(s)));
    if constexpr (Fraction != 0)
        value = _mm_mul_ps(value, _mm_set1_ps(1.0f / static_cast<float>(1 << Fraction)));
    _mm_storeu_ps(out, value);
}
// FTOI: the float product by a power of two equals the binary64 one wherever the truncation can tell (an overflow
// clamps to FLT_MAX, past both limits); CVTTPS2DQ gives 0x80000000 out of range, which the interpreter's
// saturation matches below -2^31 and which is replaced by 0x7FFFFFFF from 2^31 up.
template <int Fraction, int Dest>
VU1N_INLINE void vu1nFtoi(const float *s, float *out)
{
    __m128 value = vu1n_detail::normalize(_mm_loadu_ps(s));
    if constexpr (Fraction != 0)
        value = _mm_mul_ps(value, _mm_set1_ps(static_cast<float>(1 << Fraction)));
    __m128i converted = _mm_cvttps_epi32(value);
    const __m128 high = _mm_cmpge_ps(value, _mm_set1_ps(2147483648.0f));
    converted = _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(converted), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), high));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(out), converted);
}

#endif

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
