// The game's VU1 program images: the list in config/vu1_images.txt and their bytes in the game ELF.
#ifndef MKSM_VU1GEN_IMAGES_H
#define MKSM_VU1GEN_IMAGES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vu1gen
{
    // One "image" block of config/vu1_images.txt. The game uploads 16 KB from elfAddress to micro address 0, so an
    // image is its own code followed by whatever comes next in the ELF.
    struct ImageConfig
    {
        std::string name;                  // [a-z][a-z0-9_]*: part of the generated function's name
        uint32_t elfAddress = 0;
        bool hasHash = false;
        uint64_t hash = 0;                 // imageHash() of the 16 KB
        uint32_t ownPairs = 0;             // pairs of the image's own code, 0 when not given
        std::vector<uint32_t> mscalEntries; // byte addresses in micro memory, ascending
        std::vector<uint32_t> subroutines;  // targets of indirect jumps (JALR), ascending
        int line = 0;
    };

    struct ImagesConfig
    {
        std::vector<ImageConfig> images;

        const ImageConfig *find(const std::string &name) const;
        const ImageConfig *findByHash(uint64_t hash) const;
    };

    // Both return false with a message "line n: ..." (or about the file) in error.
    bool parseImagesConfig(const std::string &text, ImagesConfig &config, std::string &error);
    bool loadImagesConfig(const std::string &path, ImagesConfig &config, std::string &error);

    bool readFile(const std::string &path, std::vector<uint8_t> &bytes, std::string &error);
    bool writeFileIfChanged(const std::string &path, const std::string &content, std::string &error);

    // The hash the launch captures (logs/vu1cap/code_<hash>.bin) and the studies name the images by: 64-bit FNV-1a
    // with the offset basis 1469598103934665603. That is the standard basis without its last decimal digit; it is
    // kept as it is because the capture files carry it.
    uint64_t imageHash(const uint8_t *bytes, size_t size);

    // The size bytes at a virtual address of a little-endian ELF32 file, taken from the loadable segment that holds
    // them all. Null with a message when no segment does.
    const uint8_t *elfBytesAt(const std::vector<uint8_t> &elf, uint32_t address, uint32_t size, std::string &error);

    struct Image
    {
        const ImageConfig *config = nullptr;
        std::vector<uint8_t> code; // kCodeBytes
        uint64_t hash = 0;
    };

    // Reads every image of the configuration from the ELF and checks its hash. A wrong hash is an error: the
    // configuration (entry points) was written for other code.
    bool loadImages(const ImagesConfig &config, const std::vector<uint8_t> &elf, std::vector<Image> &images, std::string &error);
}

#endif
