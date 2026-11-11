// What the code vu1gen generates includes (after upstream's runtime/ps2_vu1.h, for VU1State).
//
// Present: the interface (vu1n_api.h), the MXCSR the helpers are written for, the address rule of VU1 data memory.
// The kick of an XGKICK is Vu1nHost::kick(data, (uint16_t)vi).
//
// Still to come, with the emitter (PROGRESS.md):
//
//   Arithmetic helpers, in two implementations behind one set of names, selected when the generated library is
//   compiled (one implementation for all of its translation units):
//     VU1N_MATH_REFERENCE  scalar, lane by lane, written straight from the interpreter's rules: operands normalised
//                          (exponent 0 -> signed zero, exponent 255 -> signed 0x7F7FFFFF), float32 operations
//                          rounded toward zero, a product-sum as two rounded operations, the result fixed up from
//                          the same expression in binary64. Needs round toward zero only (kVu1nMxcsr below).
//     default              SSE without FMA, under round toward zero + flush to zero + denormals are zero, with
//                          explicit code for what those modes do not give.
//   Memory helpers: loads and stores of qwords and of 16-bit integers at vu1nQwordOffset().
//
// Rules for a translation unit with generated arithmetic: compiled /fp:precise (never /fp:fast), no contraction of
// a multiply and an add, no arithmetic on two literals (the compiler would fold it under the default rounding), and
// it never changes MXCSR itself: vu1nRun sets Vu1nProgram::mxcsr around the whole launch.
#ifndef MKSM_VU1N_H
#define MKSM_VU1N_H

#include "vu1n_api.h"

// MXCSR during a launch, exceptions masked. The table vu1gen generates stores it in Vu1nProgram::mxcsr.
#if defined(VU1N_MATH_REFERENCE)
constexpr uint32_t kVu1nMxcsr = 0x7F80u; // round toward zero; denormals as IEEE, which is the interpreter's environment
#else
constexpr uint32_t kVu1nMxcsr = 0xFFC0u; // round toward zero, flush to zero, denormals are zero
#endif

// Byte offset of a qword of VU1 data memory. The interpreter computes ((vi + imm) * 16) & 0x3FFF for LQ, SQ, ILW and
// ISW and ((uint16_t)vi * 16) & 0x3FFF for the register forms and XGKICK: both are this, and nothing else bounds an
// access.
constexpr uint32_t vu1nQwordOffset(int32_t qwordAddress)
{
    return (static_cast<uint32_t>(qwordAddress) & 0x3FFu) * 16u;
}

#endif
