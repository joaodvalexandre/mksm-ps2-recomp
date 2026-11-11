// vu1gen: ahead-of-time translator of the game's VU1 programs.
//
//   vu1gen --elf <SLES_535.24> --images <config/vu1_images.txt> [--listing <folder>]
//
// Reads the images the configuration names from the game ELF, checks them against the configuration's hashes and
// decodes every pair. --listing writes listing_<image>.txt and usage_<image>.txt there (they quote the game's
// microcode: a folder outside the repository). The translation itself comes with the later stages (PROGRESS.md).
#include "images.h"
#include "listing.h"
#include "vu_decode.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    int usage()
    {
        std::fprintf(stderr, "usage: vu1gen --elf <game ELF> --images <config/vu1_images.txt> [--listing <folder>]\n");
        return 2;
    }
}

int main(int argc, char **argv)
{
    std::string elfPath;
    std::string imagesPath;
    std::string listingDir;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        std::string *value = nullptr;
        if (argument == "--elf")
            value = &elfPath;
        else if (argument == "--images")
            value = &imagesPath;
        else if (argument == "--listing")
            value = &listingDir;
        if (!value || index + 1 >= argc)
        {
            std::fprintf(stderr, "vu1gen: unknown or incomplete argument '%s'\n", argument.c_str());
            return usage();
        }
        *value = argv[++index];
    }
    if (elfPath.empty() || imagesPath.empty())
        return usage();

    std::string error;
    vu1gen::ImagesConfig config;
    std::vector<uint8_t> elf;
    std::vector<vu1gen::Image> images;
    if (!vu1gen::loadImagesConfig(imagesPath, config, error) || !vu1gen::readFile(elfPath, elf, error) ||
        !vu1gen::loadImages(config, elf, images, error))
    {
        std::fprintf(stderr, "vu1gen: %s\n", error.c_str());
        return 1;
    }

    if (!listingDir.empty())
    {
        std::error_code code;
        std::filesystem::create_directories(listingDir, code);
    }

    for (const vu1gen::Image &image : images)
    {
        const vu1gen::ImageConfig &imageConfig = *image.config;
        const std::vector<vu1gen::Pair> pairs = vu1gen::decodeProgram(image.code.data(), image.code.size());
        uint32_t reservedOwn = 0;
        uint32_t reservedAll = 0;
        for (const vu1gen::Pair &pair : pairs)
        {
            if (!pair.reserved)
                continue;
            ++reservedAll;
            if (imageConfig.ownPairs == 0u || pair.index() < imageConfig.ownPairs)
                ++reservedOwn;
        }
        std::printf("image %-6s ELF 0x%06X  hash %016llX%s  pairs %u (own %u)  reserved %u (own %u)  MSCAL entries %u  subroutines %u\n",
                    imageConfig.name.c_str(), imageConfig.elfAddress, static_cast<unsigned long long>(image.hash),
                    imageConfig.hasHash ? "" : " (not checked)", static_cast<unsigned>(pairs.size()), imageConfig.ownPairs,
                    reservedAll, reservedOwn, static_cast<unsigned>(imageConfig.mscalEntries.size()),
                    static_cast<unsigned>(imageConfig.subroutines.size()));

        if (!listingDir.empty())
        {
            const std::string base = (std::filesystem::path(listingDir) / "").string();
            if (!vu1gen::writeFileIfChanged(base + "listing_" + imageConfig.name + ".txt", vu1gen::rawListing(imageConfig, pairs), error) ||
                !vu1gen::writeFileIfChanged(base + "usage_" + imageConfig.name + ".txt", vu1gen::usageListing(pairs), error))
            {
                std::fprintf(stderr, "vu1gen: %s\n", error.c_str());
                return 1;
            }
        }
    }
    return 0;
}
