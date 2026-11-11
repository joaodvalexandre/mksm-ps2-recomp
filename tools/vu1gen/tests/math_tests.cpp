// The fast arithmetic of vu1n.h against the reference (vu1nRef*), bit for bit, on operands chosen where the two could
// part: exponent 0 and 255 operands, tiny and huge exponents, products near 2^-79, FLT_MIN and FLT_MAX, product-sums
// that cancel to within a few units in the last place, integers used as floats, FTOI near its limits.
//
// The reference is the interpreter's code transcribed (it is what the translated programs are compared with on the
// captured launches); this test covers the operand values the captures never show. VU1N_MATH_FUZZ=<n> sets the
// number of vectors per operation (default 200000).
#include "test.h"

#include "vu1n.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    struct Rng
    {
        uint64_t state;
        explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1u) {}
        uint64_t next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }
        uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
    };

    uint32_t withExponent(Rng &rng, uint32_t exponent)
    {
        return (static_cast<uint32_t>(rng.next()) & 0x807FFFFFu) | (exponent << 23);
    }

    // One operand word from a mix of classes.
    uint32_t operand(Rng &rng)
    {
        switch (rng.below(10))
        {
        case 0: return static_cast<uint32_t>(rng.next());
        case 1: return withExponent(rng, rng.below(2) ? 0u : 255u);
        case 2: return withExponent(rng, 1u + rng.below(40));
        case 3: return withExponent(rng, 215u + rng.below(40));
        case 4: return withExponent(rng, 100u + rng.below(55));
        case 5: return rng.below(2) ? 0x80000000u : 0u;
        case 6: return rng.below(0x01000000u);                 // small integers: denormal patterns
        case 7: return 0x00800000u + rng.below(0x00800000u);   // integers that are tiny normal floats
        case 8: return withExponent(rng, 150u + rng.below(12)); // around the FTOI limits
        default: return withExponent(rng, 127u + rng.below(3) - 1u);
        }
    }

    float f(uint32_t bits) { return vu1nFloat(bits); }

    struct Mismatches
    {
        uint64_t count = 0;
        uint64_t compared = 0;
        std::string first;

        void compare(const char *what, const uint32_t *in, size_t inWords, const float *fast, const float *ref, uint8_t dest)
        {
            ++compared;
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((dest & (8u >> component)) == 0u || vu1nBits(fast[component]) == vu1nBits(ref[component]))
                    continue;
                if (count++ < 8u)
                {
                    char text[256];
                    std::snprintf(text, sizeof(text), "%s lane %u: fast %08X, reference %08X; inputs", what, component,
                                  vu1nBits(fast[component]), vu1nBits(ref[component]));
                    first += text;
                    for (size_t index = 0; index < inWords; ++index)
                    {
                        std::snprintf(text, sizeof(text), " %08X", in[index]);
                        first += text;
                    }
                    first += "\n";
                }
                return;
            }
        }
    };

    uint64_t vectorsPerOperation()
    {
        const char *value = std::getenv("VU1N_MATH_FUZZ");
        return value ? std::strtoull(value, nullptr, 10) : 200000u;
    }

    // Runs body under the launch MXCSR.
    template <class Body>
    void underLaunchMxcsr(Body body)
    {
        const unsigned saved = _mm_getcsr();
        _mm_setcsr(kVu1nMxcsr);
        body();
        _mm_setcsr(saved);
    }

    // Twelve operand words: s, t, acc. For the product-sums some vectors are built to cancel: acc set to the
    // product rounded (or its negation), moved by a few units in the last place.
    void makeVectors(Rng &rng, uint32_t *w, bool cancel, bool subtract)
    {
        for (int index = 0; index < 12; ++index)
            w[index] = operand(rng);
        if (rng.below(4) == 0)
        {
            // Products whose exponent lands near 2^-79, 2^-126 or 2^127.
            static constexpr int kTargets[3] = {-79, -126, 127};
            for (int lane = 0; lane < 4; ++lane)
            {
                const int target = kTargets[rng.below(3)] + static_cast<int>(rng.below(5)) - 2;
                const int ea = 1 + static_cast<int>(rng.below(253));
                int eb = target - (ea - 127) + 127;
                eb = eb < 1 ? 1 : (eb > 254 ? 254 : eb);
                w[lane] = withExponent(rng, static_cast<uint32_t>(ea));
                w[4 + lane] = withExponent(rng, static_cast<uint32_t>(eb));
            }
        }
        if (cancel && rng.below(2) == 0)
        {
            for (int lane = 0; lane < 4; ++lane)
            {
                const float a = vu1nN(f(w[lane]));
                const float b = vu1nN(f(w[4 + lane]));
                const float p = a * b; // under the launch MXCSR
                uint32_t z = vu1nBits(subtract ? p : -p);
                z += static_cast<uint32_t>(static_cast<int>(rng.below(7)) - 3);
                w[8 + lane] = z;
            }
        }
    }

    template <int Op, int Dest>
    void fuzzFmac(Mismatches &m, uint64_t seed, const char *name)
    {
        Rng rng(seed);
        const uint64_t n = vectorsPerOperation();
        underLaunchMxcsr([&] {
            for (uint64_t index = 0; index < n; ++index)
            {
                uint32_t w[12];
                makeVectors(rng, w, Op == kVu1nMadd || Op == kVu1nMsub, Op == kVu1nMsub);
                float s[4], t[4], acc[4], fast[4], ref[4];
                std::memcpy(s, w, 16);
                std::memcpy(t, w + 4, 16);
                std::memcpy(acc, w + 8, 16);
                vu1nFmac<Op, Dest>(s, t, acc, fast);
                (void)vu1nRefFmac<Op>(Dest, s, t, acc, ref);
                m.compare(name, w, 12, fast, ref, Dest);
                // The scalar form with lane y of t.
                vu1nFmacS<Op, Dest>(s, t[1], acc, fast);
                const float ts[4] = {t[1], t[1], t[1], t[1]};
                (void)vu1nRefFmac<Op>(Dest, s, ts, acc, ref);
                m.compare(name, w, 12, fast, ref, Dest);
            }
        });
    }

    template <bool Sub>
    void fuzzOpm(Mismatches &m, uint64_t seed)
    {
        Rng rng(seed);
        const uint64_t n = vectorsPerOperation();
        underLaunchMxcsr([&] {
            for (uint64_t index = 0; index < n; ++index)
            {
                uint32_t w[12];
                makeVectors(rng, w, false, false);
                float s[4], t[4], acc[4], fast[4], ref[4];
                std::memcpy(s, w, 16);
                std::memcpy(t, w + 4, 16);
                std::memcpy(acc, w + 8, 16);
                if (Sub && rng.below(2) == 0)
                {
                    // OPMSUB cancelling: acc lane c = product of the permuted lanes, moved a few units.
                    static constexpr int kLeft[3] = {1, 2, 0};
                    static constexpr int kRight[3] = {2, 0, 1};
                    for (int lane = 0; lane < 3; ++lane)
                    {
                        const float p = vu1nN(s[kLeft[lane]]) * vu1nN(t[kRight[lane]]);
                        acc[lane] = vu1nFloat(vu1nBits(p) + static_cast<uint32_t>(static_cast<int>(rng.below(7)) - 3));
                    }
                    std::memcpy(w + 8, acc, 16);
                }
                vu1nOpmFast<Sub, 0xE>(s, t, acc, fast);
                (void)vu1nRefOpm<Sub>(0xE, s, t, acc, ref);
                m.compare(Sub ? "OPMSUB" : "OPMULA", w, 12, fast, ref, 0xE);
            }
        });
    }

    template <class Fast, class Ref>
    void fuzzUnary(Mismatches &m, uint64_t seed, const char *name, Fast fast, Ref ref)
    {
        Rng rng(seed);
        const uint64_t n = vectorsPerOperation();
        underLaunchMxcsr([&] {
            for (uint64_t index = 0; index < n; ++index)
            {
                uint32_t w[8];
                for (uint32_t &word : w)
                    word = operand(rng);
                float s[4], t[4], outFast[4], outRef[4];
                std::memcpy(s, w, 16);
                std::memcpy(t, w + 4, 16);
                fast(s, t, outFast);
                ref(s, t, outRef);
                m.compare(name, w, 8, outFast, outRef, 0xF);
            }
        });
    }

    void report(const Mismatches &m)
    {
        if (m.count != 0u)
            std::fprintf(stderr, "%s", m.first.c_str());
        CHECK_EQ(m.count, 0u);
    }
}

TEST_CASE(fast_fmac_equals_reference)
{
    Mismatches m;
    fuzzFmac<kVu1nAdd, 0xF>(m, 1, "ADD");
    fuzzFmac<kVu1nSub, 0xF>(m, 2, "SUB");
    fuzzFmac<kVu1nMul, 0xF>(m, 3, "MUL");
    fuzzFmac<kVu1nMadd, 0xF>(m, 4, "MADD");
    fuzzFmac<kVu1nMsub, 0xF>(m, 5, "MSUB");
    fuzzFmac<kVu1nMadd, 0xA>(m, 6, "MADD.xz");
    fuzzFmac<kVu1nMsub, 0x1>(m, 7, "MSUB.w");
    report(m);
    CHECK(m.compared >= 14u * vectorsPerOperation());
}

TEST_CASE(fast_opm_equals_reference)
{
    Mismatches m;
    fuzzOpm<false>(m, 11);
    fuzzOpm<true>(m, 12);
    report(m);
}

TEST_CASE(fast_minmax_abs_itof_ftoi_equal_reference)
{
    Mismatches m;
    fuzzUnary(m, 21, "MAX", [](const float *s, const float *t, float *o) { vu1nMinMax<true, 0xF>(s, t, o); },
              [](const float *s, const float *t, float *o) { vu1nRefMinMax<true>(0xF, s, t, o); });
    fuzzUnary(m, 22, "MINI", [](const float *s, const float *t, float *o) { vu1nMinMax<false, 0xF>(s, t, o); },
              [](const float *s, const float *t, float *o) { vu1nRefMinMax<false>(0xF, s, t, o); });
    fuzzUnary(m, 23, "MAXbc", [](const float *s, const float *t, float *o) { vu1nMinMaxS<true, 0xF>(s, t[2], o); },
              [](const float *s, const float *t, float *o) {
                  const float b[4] = {t[2], t[2], t[2], t[2]};
                  vu1nRefMinMax<true>(0xF, s, b, o);
              });
    fuzzUnary(m, 24, "ABS", [](const float *s, const float *, float *o) { vu1nAbs<0xF>(s, o); },
              [](const float *s, const float *, float *o) { vu1nRefAbs(0xF, s, o); });
    fuzzUnary(m, 25, "ITOF0", [](const float *s, const float *, float *o) { vu1nItof<0, 0xF>(s, o); },
              [](const float *s, const float *, float *o) { vu1nRefItof<0>(0xF, s, o); });
    fuzzUnary(m, 26, "ITOF4", [](const float *s, const float *, float *o) { vu1nItof<4, 0xF>(s, o); },
              [](const float *s, const float *, float *o) { vu1nRefItof<4>(0xF, s, o); });
    fuzzUnary(m, 27, "ITOF12", [](const float *s, const float *, float *o) { vu1nItof<12, 0xF>(s, o); },
              [](const float *s, const float *, float *o) { vu1nRefItof<12>(0xF, s, o); });
    fuzzUnary(m, 28, "ITOF15", [](const float *s, const float *, float *o) { vu1nItof<15, 0xF>(s, o); },
              [](const float *s, const float *, float *o) { vu1nRefItof<15>(0xF, s, o); });
    fuzzUnary(m, 29, "FTOI0", [](const float *s, const float *, float *o) { vu1nFtoi<0, 0xF>(s, o); },
              [](const float *s, const float *, float *o) { vu1nRefFtoi<0>(0xF, s, o); });
    fuzzUnary(m, 30, "FTOI4", [](const float *s, const float *, float *o) { vu1nFtoi<4, 0xF>(s, o); },
              [](const float *s, const float *, float *o) { vu1nRefFtoi<4>(0xF, s, o); });
    fuzzUnary(m, 31, "FTOI12", [](const float *s, const float *, float *o) { vu1nFtoi<12, 0xF>(s, o); },
              [](const float *s, const float *, float *o) { vu1nRefFtoi<12>(0xF, s, o); });
    fuzzUnary(m, 32, "FTOI15", [](const float *s, const float *, float *o) { vu1nFtoi<15, 0xF>(s, o); },
              [](const float *s, const float *, float *o) { vu1nRefFtoi<15>(0xF, s, o); });
    report(m);
}

TEST_CASE(fast_helpers_on_hand_picked_values)
{
    // Cases the argument in vu1n.h names, written out.
    underLaunchMxcsr([] {
        const auto madd = [](uint32_t a, uint32_t b, uint32_t z) {
            const float s[4] = {f(a), f(a), f(a), f(a)};
            const float t[4] = {f(b), f(b), f(b), f(b)};
            const float acc[4] = {f(z), f(z), f(z), f(z)};
            float fast[4], ref[4];
            vu1nFmac<kVu1nMsub, 0xF>(s, t, acc, fast);
            (void)vu1nRefFmac<kVu1nMsub>(0xF, s, t, acc, ref);
            CHECK_EQ(vu1nBits(fast[0]), vu1nBits(ref[0]));
            return vu1nBits(ref[0]);
        };
        // 1.0 - 2^-140 (a denormal product): truncated to the float below 1.0, which flush to zero would not give.
        CHECK_EQ(madd(0x1C800000u, 0x1C800000u, 0x3F800000u), 0x3F7FFFFFu);
        // A product below the smallest denormal is 0: 1.0 stays.
        CHECK_EQ(madd(0x00C00000u, 0x00C00000u, 0x3F800000u), 0x3F800000u);
        // 0 - 2^-140: the zero of the binary64 value's sign.
        CHECK_EQ(madd(0x1C800000u, 0x1C800000u, 0x00000000u), 0x80000000u);
        // An overflowing product: 0x7F7FFFFF with the sign.
        CHECK_EQ(madd(0x7F000000u, 0x7F000000u, 0x00000000u), 0xFF7FFFFFu);
        // Exponent-255 operands are 0x7F7FFFFF.
        CHECK_EQ(madd(0x7F800001u, 0x3F800000u, 0x00000000u), 0xFF7FFFFFu);

        // The slow-lane bounds, from the review of the fast arithmetic. A product just below 2^-79 whose float
        // rounding cancels the accumulator exactly while the binary64 value is -2^-127: the interpreter gives -0
        // (a bound of 2^-80 would leave this lane on the fast path, with +0).
        const auto maddAdd = [](uint32_t a, uint32_t b, uint32_t z) {
            const float s[4] = {f(a), f(a), f(a), f(a)};
            const float t[4] = {f(b), f(b), f(b), f(b)};
            const float acc[4] = {f(z), f(z), f(z), f(z)};
            float fast[4], ref[4];
            vu1nFmac<kVu1nMadd, 0xF>(s, t, acc, fast);
            (void)vu1nRefFmac<kVu1nMadd>(0xF, s, t, acc, ref);
            CHECK_EQ(vu1nBits(fast[0]), vu1nBits(ref[0]));
            return vu1nBits(ref[0]);
        };
        CHECK_EQ(maddAdd(0xABFFFFFFu, 0x2B7FFFFFu, 0x17FFFFFEu), 0x80000000u);
        CHECK_EQ(madd(0xABFFFFFFu, 0xAB7FFFFFu, 0x17FFFFFEu), 0x80000000u);
        // A product of 2^130 clamped to FLT_MAX cancels an accumulator of -FLT_MAX in float, but the binary64 value
        // overflows: 0x7F7FFFFF.
        CHECK_EQ(maddAdd(0x60000000u, 0x60000000u, 0xFF7FFFFFu), 0x7F7FFFFFu);
    });
}
