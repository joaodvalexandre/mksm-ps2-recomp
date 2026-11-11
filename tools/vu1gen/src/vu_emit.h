// Emission of C++ for an analysed VU1 program (vu_flow.h).
//
// One translation unit per image with
//   Vu1nStatus vu1n_run_<image>(VU1State &state, uint8_t *data, Vu1nHost &host, uint32_t entryPc)
// a switch on the entry pc (anything else returns NotHandled before touching anything), a label per pair reached in
// sequence, the delay slot of a branch and the last pair of a launch emitted again after their branch / E bit, goto
// for static branches and a switch for the indirect ones. Every instruction goes through the helpers of
// runtime/vu1n.h. Plus one table unit with vu1nPrograms().
//
// The output quotes the game's microcode (the image bytes, the disassembly in comments): it goes to a folder
// outside the repository.
#ifndef MKSM_VU1GEN_VU_EMIT_H
#define MKSM_VU1GEN_VU_EMIT_H

#include "images.h"
#include "vu_flow.h"

#include <string>
#include <vector>

namespace vu1gen
{
    // The source of vu1n_<image>.cpp.
    std::string emitProgram(const Image &image, const ProgramFlow &program);

    // The source of vu1n_table.cpp for the given images, in this order.
    std::string emitTable(const std::vector<const ProgramFlow *> &programs);
}

#endif
