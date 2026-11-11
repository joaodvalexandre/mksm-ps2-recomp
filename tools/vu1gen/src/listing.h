// Text listings of a decoded image. They quote the game's microcode: write them to the build tree only.
#ifndef MKSM_VU1GEN_LISTING_H
#define MKSM_VU1GEN_LISTING_H

#include "images.h"
#include "vu_decode.h"

#include <string>
#include <vector>

namespace vu1gen
{
    // One line per pair: address, index, words, the I E M D T bits, both instructions. The format of the listings
    // of the study this generator was written from, so that the two can be compared byte for byte.
    std::string rawListing(const ImageConfig &config, const std::vector<Pair> &pairs);

    // Three lines per pair with every field of the decoder (the usage the interpreter declares, the semantics,
    // the encoding fields): what the decoder self-test compares with the reference decoder.
    std::string usageListing(const std::vector<Pair> &pairs);
}

#endif
