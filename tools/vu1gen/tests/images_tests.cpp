// The image list parser, the ELF reader and the image hash.
#include "test.h"
#include "images.h"
#include "vu_decode.h"

#include <cstdio>
#include <cstring>

using namespace vu1gen;

namespace
{
    bool parses(const char *text, ImagesConfig &config, std::string &error)
    {
        error.clear();
        return parseImagesConfig(text, config, error);
    }

    // The message of a text that must be refused.
    std::string refusal(const char *text)
    {
        ImagesConfig config;
        std::string error;
        return parseImagesConfig(text, config, error) ? std::string("(accepted)") : error;
    }

    void put16(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
    {
        bytes[at] = static_cast<uint8_t>(value);
        bytes[at + 1u] = static_cast<uint8_t>(value >> 8);
    }

    void put32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
    {
        put16(bytes, at, value & 0xFFFFu);
        put16(bytes, at + 2u, value >> 16);
    }

    // A little-endian ELF32 file with two loadable segments after a segment of another type:
    //   0x00100000: fileSize bytes at file offset 0x1000 (byte k = k * 7 + 1)
    //   0x00500000: 0x100 bytes right after them, of which the file holds only 0x80
    std::vector<uint8_t> makeElf(uint32_t fileSize)
    {
        std::vector<uint8_t> elf(0x1000u + fileSize + 0x80u, 0u);
        elf[0] = 0x7Fu;
        elf[1] = 'E';
        elf[2] = 'L';
        elf[3] = 'F';
        elf[4] = 1u;
        elf[5] = 1u;
        put32(elf, 28u, 52u);  // e_phoff
        put16(elf, 42u, 32u);  // e_phentsize
        put16(elf, 44u, 3u);   // e_phnum
        const uint32_t headers[3][4] = {
            // type, offset, vaddr, filesz
            {6u, 52u, 0x00100000u, 0x60u},
            {1u, 0x1000u, 0x00100000u, fileSize},
            {1u, 0x1000u + fileSize, 0x00500000u, 0x100u},
        };
        for (uint32_t index = 0; index < 3u; ++index)
        {
            const size_t at = 52u + index * 32u;
            put32(elf, at, headers[index][0]);
            put32(elf, at + 4u, headers[index][1]);
            put32(elf, at + 8u, headers[index][2]);
            put32(elf, at + 16u, headers[index][3]);
        }
        for (uint32_t k = 0; k < fileSize; ++k)
            elf[0x1000u + k] = static_cast<uint8_t>(k * 7u + 1u);
        return elf;
    }
}

TEST_CASE(images_config_is_parsed)
{
    ImagesConfig config;
    std::string error;
    const char *text =
        "# a comment\n"
        "\n"
        "image small 0x49BAE0   # trailing comment\n"
        "  hash F6DCE9B3599C989A\n"
        "  pairs 17\n"
        "  mscal 0x0000\n"
        "image cut 4858736\n"
        "  mscal 0x36F0 0x04A0\n"
        "  mscal 8\n"
        "  subroutine 0x30E0\n"
        "\tpairs 1932\r\n";
    CHECK(parses(text, config, error));
    CHECK_EQ(error, "");
    CHECK_EQ(config.images.size(), 2u);
    if (config.images.size() != 2u)
        return;

    const ImageConfig &small = config.images[0];
    CHECK_EQ(small.name, "small");
    CHECK_EQ(small.elfAddress, 0x49BAE0u);
    CHECK(small.hasHash);
    CHECK(small.hash == 0xF6DCE9B3599C989Aull);
    CHECK_EQ(small.ownPairs, 17u);
    CHECK_EQ(small.mscalEntries.size(), 1u);
    CHECK_EQ(small.subroutines.size(), 0u);
    CHECK_EQ(small.line, 3);

    const ImageConfig &cut = config.images[1];
    CHECK_EQ(cut.name, "cut");
    CHECK_EQ(cut.elfAddress, 0x4A2370u);
    CHECK(!cut.hasHash);
    CHECK_EQ(cut.ownPairs, 1932u);
    CHECK_EQ(cut.mscalEntries.size(), 3u);
    if (cut.mscalEntries.size() == 3u)
    {
        // ascending, whatever the order in the file
        CHECK_EQ(cut.mscalEntries[0], 0x0008u);
        CHECK_EQ(cut.mscalEntries[1], 0x04A0u);
        CHECK_EQ(cut.mscalEntries[2], 0x36F0u);
    }
    CHECK_EQ(cut.subroutines.size(), 1u);

    CHECK(config.find("cut") == &cut);
    CHECK(config.find("game") == nullptr);
    CHECK(config.findByHash(0xF6DCE9B3599C989Aull) == &small);
    CHECK(config.findByHash(0u) == nullptr);
}

TEST_CASE(images_config_errors_name_the_line)
{
    CHECK_EQ(refusal(""), "no image");
    CHECK_EQ(refusal("# nothing\n"), "no image");
    CHECK_EQ(refusal("mscal 0\n"), "line 1: 'mscal' before the first 'image'");
    CHECK_EQ(refusal("image a 0x1000\n"), "line 1: image 'a' has no 'mscal' entry");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\n bogus 1\n"), "line 3: unknown keyword 'bogus'");
    CHECK_EQ(refusal("image A 0x1000\n mscal 0\n").substr(0, 7), "line 1:");
    CHECK_EQ(refusal("image 9a 0x1000\n mscal 0\n").substr(0, 7), "line 1:");
    CHECK_EQ(refusal("image a\n mscal 0\n").substr(0, 7), "line 1:");
    CHECK_EQ(refusal("image a 0\n mscal 0\n").substr(0, 7), "line 1:");
    CHECK_EQ(refusal("image a 0x1000 extra\n mscal 0\n").substr(0, 7), "line 1:");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\nimage a 0x2000\n mscal 0\n").substr(0, 7), "line 3:");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\nimage b 0x1000\n mscal 0\n").substr(0, 7), "line 3:");
    // addresses: a pair inside micro memory, listed once
    CHECK_EQ(refusal("image a 0x1000\n mscal\n"), "line 2: 'mscal' needs at least one address");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0x0004\n").substr(0, 7), "line 2:");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0x4000\n").substr(0, 7), "line 2:");
    CHECK_EQ(refusal("image a 0x1000\n mscal zero\n").substr(0, 7), "line 2:");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0x10 0x20\n mscal 0x10\n"), "line 3: address 0x10 is listed twice");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\n subroutine 0x3FFC\n").substr(0, 7), "line 3:");
    // one hash and one pair count per image
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\n hash 12\n hash 12\n").substr(0, 7), "line 4:");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\n hash xyz\n").substr(0, 7), "line 3:");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\n hash 11112222333344445\n").substr(0, 7), "line 3:");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\n pairs 0\n").substr(0, 7), "line 3:");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\n pairs 2049\n").substr(0, 7), "line 3:");
    CHECK_EQ(refusal("image a 0x1000\n mscal 0\n pairs 5\n pairs 5\n").substr(0, 7), "line 4:");

    // The same address may be an MSCAL entry and a subroutine; the last pair is a valid address.
    ImagesConfig config;
    std::string error;
    CHECK(parses("image a_1 0x1000\n mscal 0x3FF8\n subroutine 0x3FF8\n pairs 2048\n hash 0xFFFFFFFFFFFFFFFF\n", config, error));
    CHECK(config.images.size() == 1u && config.images[0].hash == 0xFFFFFFFFFFFFFFFFull);
}

TEST_CASE(repository_image_list_is_valid)
{
    // config/vu1_images.txt of this repository (the path is given by the build).
    ImagesConfig config;
    std::string error;
    CHECK(loadImagesConfig(VU1GEN_IMAGES_FILE, config, error));
    CHECK_EQ(error, "");
    CHECK_EQ(config.images.size(), 4u);
    unsigned entries = 0;
    unsigned subroutines = 0;
    for (const ImageConfig &image : config.images)
    {
        CHECK(image.hasHash);
        CHECK(image.ownPairs != 0u);
        for (const uint32_t entry : image.mscalEntries)
            CHECK(entry < image.ownPairs * 8u);
        for (const uint32_t entry : image.subroutines)
            CHECK(entry < image.ownPairs * 8u);
        entries += static_cast<unsigned>(image.mscalEntries.size());
        subroutines += static_cast<unsigned>(image.subroutines.size());
    }
    CHECK_EQ(entries, 1u + 9u + 12u + 14u);
    CHECK_EQ(subroutines, 1u);
    CHECK(config.find("small") && config.find("front") && config.find("game") && config.find("cut"));

    CHECK(!loadImagesConfig(std::string(VU1GEN_IMAGES_FILE) + ".missing", config, error));
}

TEST_CASE(image_hash_is_the_captures_hash)
{
    // 64-bit FNV-1a with the offset basis 1469598103934665603; reference values from a Python implementation.
    CHECK(imageHash(nullptr, 0u) == 0x14650FB0739D0383ull);
    const uint8_t abc[3] = {'a', 'b', 'c'};
    CHECK(imageHash(abc, sizeof(abc)) == 0xE16801510DB89EFDull);
    uint8_t bytes[256];
    for (uint32_t index = 0; index < 256u; ++index)
        bytes[index] = static_cast<uint8_t>(index);
    CHECK(imageHash(bytes, sizeof(bytes)) == 0x16D173BDFCDAE583ull);
}

TEST_CASE(elf_segments_are_found_by_address)
{
    const std::vector<uint8_t> elf = makeElf(0x5000u);
    std::string error;

    const uint8_t *bytes = elfBytesAt(elf, 0x00100000u, 16u, error);
    CHECK(bytes == elf.data() + 0x1000u);
    bytes = elfBytesAt(elf, 0x00100123u, 0x4000u, error);
    CHECK(bytes == elf.data() + 0x1123u);
    if (bytes)
        CHECK_EQ(bytes[0], static_cast<uint8_t>(0x123u * 7u + 1u));
    // up to the last byte of the segment, not beyond, not across two segments
    CHECK(elfBytesAt(elf, 0x00104FFFu, 1u, error) == elf.data() + 0x5FFFu);
    CHECK(elfBytesAt(elf, 0x00104FFFu, 2u, error) == nullptr);
    CHECK_EQ(error, "no loadable ELF segment holds 0x104FFF..0x105000");
    CHECK(elfBytesAt(elf, 0x000FFFFFu, 2u, error) == nullptr);
    CHECK(elfBytesAt(elf, 0x00200000u, 1u, error) == nullptr);
    // the second segment says 0x100 bytes but the file ends after 0x80
    CHECK(elfBytesAt(elf, 0x00500000u, 0x80u, error) == elf.data() + 0x6000u);
    CHECK(elfBytesAt(elf, 0x00500000u, 0x81u, error) == nullptr);
    CHECK_EQ(error, "the ELF file ends inside the segment that holds the address");

    std::vector<uint8_t> notElf = elf;
    notElf[1] = 'X';
    CHECK(elfBytesAt(notElf, 0x00100000u, 16u, error) == nullptr);
    CHECK_EQ(error, "not a little-endian ELF32 file");
    std::vector<uint8_t> bigEndian = elf;
    bigEndian[5] = 2u;
    CHECK(elfBytesAt(bigEndian, 0x00100000u, 16u, error) == nullptr);
    CHECK(elfBytesAt(std::vector<uint8_t>(10u, 0u), 0x00100000u, 16u, error) == nullptr);
}

TEST_CASE(images_are_read_and_their_hash_checked)
{
    const std::vector<uint8_t> elf = makeElf(0x9000u);
    const uint64_t hashAt0 = imageHash(elf.data() + 0x1000u, kCodeBytes);
    const uint64_t hashAt8 = imageHash(elf.data() + 0x1008u, kCodeBytes);
    CHECK(hashAt0 != hashAt8);

    char text[256];
    std::snprintf(text, sizeof(text), "image one 0x100000\n hash %016llX\n mscal 0\nimage two 0x100008\n mscal 0\n",
                  static_cast<unsigned long long>(hashAt0));
    ImagesConfig config;
    std::string error;
    CHECK(parses(text, config, error));
    std::vector<Image> images;
    CHECK(loadImages(config, elf, images, error));
    CHECK_EQ(images.size(), 2u);
    if (images.size() == 2u)
    {
        CHECK(images[0].config == &config.images[0]);
        CHECK_EQ(images[0].code.size(), static_cast<size_t>(kCodeBytes));
        CHECK(std::memcmp(images[0].code.data(), elf.data() + 0x1000u, kCodeBytes) == 0);
        CHECK(images[0].hash == hashAt0);
        CHECK(images[1].hash == hashAt8);
    }

    // Another hash: the configuration was written for other code.
    std::snprintf(text, sizeof(text), "image one 0x100000\n hash %016llX\n mscal 0\n", static_cast<unsigned long long>(hashAt8));
    CHECK(parses(text, config, error));
    CHECK(!loadImages(config, elf, images, error));
    CHECK_EQ(error.substr(0, 12), "image 'one':");

    // 16 KB that do not fit the segment.
    CHECK(parses("image far 0x106000\n mscal 0\n", config, error));
    CHECK(!loadImages(config, elf, images, error));
    CHECK_EQ(error.substr(0, 12), "image 'far':");
}
