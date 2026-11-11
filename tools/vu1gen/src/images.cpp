#include "images.h"

#include "vu_decode.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace vu1gen
{
    namespace
    {
        std::string lineError(int line, const std::string &message)
        {
            return "line " + std::to_string(line) + ": " + message;
        }

        // Decimal, or hexadecimal with 0x. The whole token must be the number.
        bool parseNumber(const std::string &token, uint64_t &value, bool hexOnly = false)
        {
            if (token.empty())
                return false;
            size_t start = 0;
            int base = hexOnly ? 16 : 10;
            if (token.size() > 2u && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            {
                start = 2u;
                base = 16;
            }
            if (token.size() - start > 16u)
                return false;
            value = 0;
            for (size_t index = start; index < token.size(); ++index)
            {
                const char c = token[index];
                int digit = -1;
                if (c >= '0' && c <= '9')
                    digit = c - '0';
                else if (c >= 'a' && c <= 'f')
                    digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    digit = c - 'A' + 10;
                if (digit < 0 || digit >= base)
                    return false;
                value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
            }
            return true;
        }

        bool validName(const std::string &name)
        {
            if (name.empty() || name[0] < 'a' || name[0] > 'z')
                return false;
            return std::all_of(name.begin(), name.end(), [](char c)
                               { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
        }

        bool addAddresses(const std::vector<std::string> &tokens, std::vector<uint32_t> &list, int line, std::string &error)
        {
            if (tokens.size() < 2u)
            {
                error = lineError(line, "'" + tokens[0] + "' needs at least one address");
                return false;
            }
            for (size_t index = 1; index < tokens.size(); ++index)
            {
                uint64_t value = 0;
                if (!parseNumber(tokens[index], value) || value >= kCodeBytes || (value & 7u) != 0u)
                {
                    error = lineError(line, "'" + tokens[index] + "' is not the address of a pair in micro memory (a multiple of 8 below 0x4000)");
                    return false;
                }
                if (std::find(list.begin(), list.end(), static_cast<uint32_t>(value)) != list.end())
                {
                    error = lineError(line, "address " + tokens[index] + " is listed twice");
                    return false;
                }
                list.push_back(static_cast<uint32_t>(value));
            }
            return true;
        }

        uint16_t read16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t read32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
    }

    const ImageConfig *ImagesConfig::find(const std::string &name) const
    {
        for (const ImageConfig &image : images)
        {
            if (image.name == name)
                return &image;
        }
        return nullptr;
    }

    const ImageConfig *ImagesConfig::findByHash(uint64_t hash) const
    {
        for (const ImageConfig &image : images)
        {
            if (image.hasHash && image.hash == hash)
                return &image;
        }
        return nullptr;
    }

    bool parseImagesConfig(const std::string &text, ImagesConfig &config, std::string &error)
    {
        config.images.clear();
        std::istringstream stream(text);
        std::string rawLine;
        int line = 0;
        bool sawPairs = false;
        while (std::getline(stream, rawLine))
        {
            ++line;
            const size_t comment = rawLine.find('#');
            if (comment != std::string::npos)
                rawLine.erase(comment);
            std::istringstream words(rawLine);
            std::vector<std::string> tokens;
            for (std::string token; words >> token;)
                tokens.push_back(token);
            if (tokens.empty())
                continue;

            const std::string &keyword = tokens[0];
            if (keyword == "image")
            {
                uint64_t address = 0;
                if (tokens.size() != 3u || !validName(tokens[1]) || !parseNumber(tokens[2], address) ||
                    address == 0u || address > 0xFFFFFFFFull - kCodeBytes)
                {
                    error = lineError(line, "expected 'image <name> <ELF address>', the name made of a-z, 0-9 and _");
                    return false;
                }
                for (const ImageConfig &other : config.images)
                {
                    if (other.name == tokens[1] || other.elfAddress == address)
                    {
                        error = lineError(line, "image '" + tokens[1] + "' repeats the name or the address of line " + std::to_string(other.line));
                        return false;
                    }
                }
                ImageConfig image;
                image.name = tokens[1];
                image.elfAddress = static_cast<uint32_t>(address);
                image.line = line;
                config.images.push_back(image);
                sawPairs = false;
                continue;
            }

            if (config.images.empty())
            {
                error = lineError(line, "'" + keyword + "' before the first 'image'");
                return false;
            }
            ImageConfig &image = config.images.back();
            if (keyword == "hash")
            {
                uint64_t value = 0;
                if (tokens.size() != 2u || image.hasHash || !parseNumber(tokens[1], value, true))
                {
                    error = lineError(line, "expected one 'hash <16 hexadecimal digits>' per image");
                    return false;
                }
                image.hasHash = true;
                image.hash = value;
            }
            else if (keyword == "pairs")
            {
                uint64_t value = 0;
                if (tokens.size() != 2u || sawPairs || !parseNumber(tokens[1], value) || value == 0u || value > kCodePairs)
                {
                    error = lineError(line, "expected one 'pairs <1..2048>' per image");
                    return false;
                }
                sawPairs = true;
                image.ownPairs = static_cast<uint32_t>(value);
            }
            else if (keyword == "mscal")
            {
                if (!addAddresses(tokens, image.mscalEntries, line, error))
                    return false;
            }
            else if (keyword == "subroutine")
            {
                if (!addAddresses(tokens, image.subroutines, line, error))
                    return false;
            }
            else
            {
                error = lineError(line, "unknown keyword '" + keyword + "'");
                return false;
            }
        }

        if (config.images.empty())
        {
            error = "no image";
            return false;
        }
        for (ImageConfig &image : config.images)
        {
            if (image.mscalEntries.empty())
            {
                error = lineError(image.line, "image '" + image.name + "' has no 'mscal' entry");
                return false;
            }
            std::sort(image.mscalEntries.begin(), image.mscalEntries.end());
            std::sort(image.subroutines.begin(), image.subroutines.end());
        }
        return true;
    }

    bool loadImagesConfig(const std::string &path, ImagesConfig &config, std::string &error)
    {
        std::vector<uint8_t> bytes;
        if (!readFile(path, bytes, error))
            return false;
        if (!parseImagesConfig(std::string(bytes.begin(), bytes.end()), config, error))
        {
            error = path + ": " + error;
            return false;
        }
        return true;
    }

    bool readFile(const std::string &path, std::vector<uint8_t> &bytes, std::string &error)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            error = "cannot open " + path;
            return false;
        }
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        file.seekg(0, std::ios::beg);
        bytes.resize(size > 0 ? static_cast<size_t>(size) : 0u);
        if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        {
            error = "cannot read " + path;
            return false;
        }
        return true;
    }

    bool writeFileIfChanged(const std::string &path, const std::string &content, std::string &error)
    {
        std::vector<uint8_t> existing;
        std::string ignored;
        if (readFile(path, existing, ignored) && existing.size() == content.size() &&
            std::equal(existing.begin(), existing.end(), content.begin(), [](uint8_t a, char b)
                       { return a == static_cast<uint8_t>(b); }))
        {
            return true;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(content.data(), static_cast<std::streamsize>(content.size())))
        {
            error = "cannot write " + path;
            return false;
        }
        return true;
    }

    uint64_t imageHash(const uint8_t *bytes, size_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t index = 0; index < size; ++index)
            hash = (hash ^ bytes[index]) * 1099511628211ull;
        return hash;
    }

    const uint8_t *elfBytesAt(const std::vector<uint8_t> &elf, uint32_t address, uint32_t size, std::string &error)
    {
        constexpr size_t kHeaderSize = 52u;
        constexpr size_t kProgramHeaderSize = 32u;
        if (elf.size() < kHeaderSize || elf[0] != 0x7Fu || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' ||
            elf[4] != 1u /* 32 bits */ || elf[5] != 1u /* little endian */)
        {
            error = "not a little-endian ELF32 file";
            return nullptr;
        }
        const uint32_t programHeaders = read32(elf.data() + 28u);
        const uint32_t entrySize = read16(elf.data() + 42u);
        const uint32_t count = read16(elf.data() + 44u);
        if (entrySize < kProgramHeaderSize)
        {
            error = "ELF program header entries are too small";
            return nullptr;
        }
        for (uint32_t index = 0; index < count; ++index)
        {
            const uint64_t at = static_cast<uint64_t>(programHeaders) + static_cast<uint64_t>(index) * entrySize;
            if (at + kProgramHeaderSize > elf.size())
                break;
            const uint8_t *header = elf.data() + at;
            const uint32_t type = read32(header);
            const uint64_t fileOffset = read32(header + 4u);
            const uint64_t virtualAddress = read32(header + 8u);
            const uint64_t fileSize = read32(header + 16u);
            if (type != 1u /* PT_LOAD */ || address < virtualAddress ||
                static_cast<uint64_t>(address) + size > virtualAddress + fileSize)
            {
                continue;
            }
            const uint64_t offset = fileOffset + (address - virtualAddress);
            if (offset + size > elf.size())
            {
                error = "the ELF file ends inside the segment that holds the address";
                return nullptr;
            }
            return elf.data() + offset;
        }
        char text[96];
        std::snprintf(text, sizeof(text), "no loadable ELF segment holds 0x%X..0x%X", address, address + size - 1u);
        error = text;
        return nullptr;
    }

    bool loadImages(const ImagesConfig &config, const std::vector<uint8_t> &elf, std::vector<Image> &images, std::string &error)
    {
        images.clear();
        for (const ImageConfig &imageConfig : config.images)
        {
            const uint8_t *bytes = elfBytesAt(elf, imageConfig.elfAddress, kCodeBytes, error);
            if (!bytes)
            {
                error = "image '" + imageConfig.name + "': " + error;
                return false;
            }
            Image image;
            image.config = &imageConfig;
            image.code.assign(bytes, bytes + kCodeBytes);
            image.hash = imageHash(bytes, kCodeBytes);
            if (imageConfig.hasHash && imageConfig.hash != image.hash)
            {
                char text[160];
                std::snprintf(text, sizeof(text), "image '%s': the 16 KB at 0x%X have the hash %016llX, the configuration expects %016llX",
                              imageConfig.name.c_str(), imageConfig.elfAddress, static_cast<unsigned long long>(image.hash),
                              static_cast<unsigned long long>(imageConfig.hash));
                error = text;
                return false;
            }
            images.push_back(std::move(image));
        }
        return true;
    }
}
