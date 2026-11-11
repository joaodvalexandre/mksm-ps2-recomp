// vu1gen: ahead-of-time translator of the game's VU1 programs.
//
//   vu1gen --elf <SLES_535.24> --images <config/vu1_images.txt> [--listing <folder>] [--out <folder>] [--report <file>]
//
// Reads the images the configuration names from the game ELF, checks them against the configuration's hashes and
// decodes every pair. --listing writes listing_<image>.txt and usage_<image>.txt there. --out analyses every image
// (control flow and timing, vu_flow.h) and writes the translation: vu1n_<image>.cpp per image and vu1n_table.cpp.
// --report writes what the analysis found. All of them quote the game's microcode: folders outside the repository.
#include "images.h"
#include "listing.h"
#include "vu_decode.h"
#include "vu_emit.h"
#include "vu_flow.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    int usage()
    {
        std::fprintf(stderr, "usage: vu1gen --elf <game ELF> --images <config/vu1_images.txt> [--listing <folder>] [--out <folder>] [--report <file>]\n");
        return 2;
    }
}

int main(int argc, char **argv)
{
    std::string elfPath;
    std::string imagesPath;
    std::string listingDir;
    std::string outDir;
    std::string reportPath;
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
        else if (argument == "--out")
            value = &outDir;
        else if (argument == "--report")
            value = &reportPath;
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

    if (outDir.empty() && reportPath.empty())
        return 0;
    std::vector<vu1gen::ProgramFlow> programs(images.size());
    std::string report;
    for (size_t index = 0; index < images.size(); ++index)
    {
        if (!vu1gen::analyzeProgram(images[index], programs[index], error))
        {
            std::fprintf(stderr, "vu1gen: image %s: %s\n", images[index].config->name.c_str(), error.c_str());
            return 1;
        }
        report += vu1gen::flowReport(programs[index]);
        uint32_t handled = 0;
        for (const auto &[pc, entry] : programs[index].entries)
            handled += entry.handled ? 1u : 0u;
        std::printf("image %-6s %llu states, %u entries, %u handled, %u pairs emitted\n",
                    images[index].config->name.c_str(), static_cast<unsigned long long>(programs[index].states),
                    static_cast<unsigned>(programs[index].entries.size()), handled,
                    static_cast<unsigned>(programs[index].emitPcs.size()));
    }
    if (!reportPath.empty())
    {
        std::error_code code;
        const std::filesystem::path parent = std::filesystem::path(reportPath).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, code);
    }
    if (!reportPath.empty() && !vu1gen::writeFileIfChanged(reportPath, report, error))
    {
        std::fprintf(stderr, "vu1gen: %s\n", error.c_str());
        return 1;
    }
    if (outDir.empty())
        return 0;
    std::error_code code;
    std::filesystem::create_directories(outDir, code);
    const std::string base = (std::filesystem::path(outDir) / "").string();
    std::vector<const vu1gen::ProgramFlow *> table;
    try
    {
        for (size_t index = 0; index < images.size(); ++index)
        {
            const std::string source = vu1gen::emitProgram(images[index], programs[index]);
            if (!vu1gen::writeFileIfChanged(base + "vu1n_" + images[index].config->name + ".cpp", source, error))
                break;
            table.push_back(&programs[index]);
        }
        if (error.empty())
            vu1gen::writeFileIfChanged(base + "vu1n_table.cpp", vu1gen::emitTable(table), error);
    }
    catch (const std::exception &exception)
    {
        error = exception.what();
    }
    if (!error.empty())
    {
        std::fprintf(stderr, "vu1gen: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
