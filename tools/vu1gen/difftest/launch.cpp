#include "launch.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace vu1diff
{
    namespace
    {
        std::vector<StateField> makeStateFields()
        {
            std::vector<StateField> fields;
            char name[32];
            char group[32];
            const char lanes[] = "xyzw";
            for (uint32_t reg = 0; reg < 32u; ++reg)
            {
                for (uint32_t lane = 0; lane < 4u; ++lane)
                {
                    std::snprintf(name, sizeof(name), "vf%02u.%c", reg, lanes[lane]);
                    std::snprintf(group, sizeof(group), "vf%02u", reg);
                    fields.push_back({name, group, offsetof(VU1State, vf) + (reg * 4u + lane) * sizeof(float), sizeof(float),
                                      reg == 0u ? kFieldConstant : kFieldCompared});
                }
            }
            for (uint32_t reg = 0; reg < 16u; ++reg)
            {
                std::snprintf(name, sizeof(name), "vi%02u", reg);
                fields.push_back({name, name, offsetof(VU1State, vi) + reg * sizeof(int32_t), sizeof(int32_t),
                                  reg == 0u ? kFieldConstant : kFieldCompared});
            }
            for (uint32_t lane = 0; lane < 4u; ++lane)
            {
                std::snprintf(name, sizeof(name), "acc.%c", lanes[lane]);
                fields.push_back({name, "acc", offsetof(VU1State, acc) + lane * sizeof(float), sizeof(float), kFieldCompared});
            }
#define VU1DIFF_FIELD(member, fieldClass) \
    fields.push_back({#member, #member, offsetof(VU1State, member), sizeof(VU1State::member), fieldClass})
            VU1DIFF_FIELD(q, kFieldCompared);
            VU1DIFF_FIELD(p, kFieldCompared);
            VU1DIFF_FIELD(i, kFieldCompared);
            VU1DIFF_FIELD(r, kFieldCompared);
            VU1DIFF_FIELD(pc, kFieldCompared);
            VU1DIFF_FIELD(mac, kFieldFlags);
            VU1DIFF_FIELD(clip, kFieldCompared);
            VU1DIFF_FIELD(status, kFieldFlags);
            VU1DIFF_FIELD(cycles, kFieldCycles);
            VU1DIFF_FIELD(ebit, kFieldCompared);
            VU1DIFF_FIELD(haltAfterDelaySlot, kFieldCompared);
            VU1DIFF_FIELD(dBitEnabled, kFieldInput);
            VU1DIFF_FIELD(tBitEnabled, kFieldInput);
            VU1DIFF_FIELD(stoppedByD, kFieldCompared);
            VU1DIFF_FIELD(stoppedByT, kFieldCompared);
            VU1DIFF_FIELD(top, kFieldInput);
            VU1DIFF_FIELD(itop, kFieldInput);
            VU1DIFF_FIELD(branchPending, kFieldCompared);
            VU1DIFF_FIELD(branchTarget, kFieldCompared);
            VU1DIFF_FIELD(branchDelay, kFieldCompared);
#undef VU1DIFF_FIELD
            return fields;
        }

        std::string valueText(const uint8_t *bytes, size_t size)
        {
            char text[32];
            if (size == 1u)
            {
                std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(bytes[0]));
            }
            else if (size == 8u)
            {
                uint64_t value = 0;
                std::memcpy(&value, bytes, sizeof(value));
                std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
            }
            else
            {
                uint32_t value = 0;
                std::memcpy(&value, bytes, sizeof(value));
                std::snprintf(text, sizeof(text), "%08X", value);
            }
            return text;
        }

        // The first 32-bit word that differs in two byte ranges of the same size (a multiple of 4).
        size_t firstDifferentWord(const uint8_t *a, const uint8_t *b, size_t bytes)
        {
            for (size_t offset = 0; offset < bytes; offset += 4u)
            {
                if (std::memcmp(a + offset, b + offset, 4u) != 0)
                    return offset;
            }
            return bytes;
        }
    }

    const std::vector<StateField> &stateFields()
    {
        static const std::vector<StateField> fields = makeStateFields();
        return fields;
    }

    Difference compareState(const VU1State &expected, const VU1State &actual, uint32_t classes)
    {
        const uint8_t *a = reinterpret_cast<const uint8_t *>(&expected);
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&actual);
        Difference difference;
        for (const StateField &field : stateFields())
        {
            if ((field.fieldClass & classes) == 0u || std::memcmp(a + field.offset, b + field.offset, field.size) == 0)
                continue;
            difference.any = true;
            difference.field = field.name;
            difference.group = field.group;
            difference.expected = valueText(a + field.offset, field.size);
            difference.actual = valueText(b + field.offset, field.size);
            break;
        }
        return difference;
    }

    Difference compareLaunch(const LaunchResult &expected, const LaunchResult &actual)
    {
        Difference difference = compareState(expected.state, actual.state, kFieldCompared);
        if (difference)
            return difference;

        char text[96];
        if (std::memcmp(expected.data.data(), actual.data.data(), kDataBytes) != 0)
        {
            const size_t offset = firstDifferentWord(expected.data.data(), actual.data.data(), kDataBytes);
            std::snprintf(text, sizeof(text), "data qword 0x%03X word %u", static_cast<unsigned>(offset / 16u),
                          static_cast<unsigned>((offset / 4u) & 3u));
            difference.any = true;
            difference.field = text;
            difference.group = "data";
            difference.expected = valueText(expected.data.data() + offset, 4u);
            difference.actual = valueText(actual.data.data() + offset, 4u);
            return difference;
        }

        const Vu1nHost &a = expected.packets;
        const Vu1nHost &b = actual.packets;
        if (a.kicks.size() != b.kicks.size())
        {
            difference.any = true;
            difference.field = "packet count";
            difference.group = "packets";
            difference.expected = std::to_string(a.kicks.size());
            difference.actual = std::to_string(b.kicks.size());
            return difference;
        }
        for (size_t index = 0; index < a.kicks.size(); ++index)
        {
            const uint32_t bytes = a.kicks[index].bytes;
            if (bytes != b.kicks[index].bytes)
            {
                std::snprintf(text, sizeof(text), "packet %u of %u: size", static_cast<unsigned>(index + 1u),
                              static_cast<unsigned>(a.kicks.size()));
                difference.any = true;
                difference.field = text;
                difference.group = "packets";
                difference.expected = std::to_string(bytes);
                difference.actual = std::to_string(b.kicks[index].bytes);
                return difference;
            }
            if (std::memcmp(a.packet(index), b.packet(index), bytes) == 0)
                continue;
            const size_t offset = firstDifferentWord(a.packet(index), b.packet(index), bytes & ~3u);
            std::snprintf(text, sizeof(text), "packet %u of %u: word at 0x%X", static_cast<unsigned>(index + 1u),
                          static_cast<unsigned>(a.kicks.size()), static_cast<unsigned>(offset));
            difference.any = true;
            difference.field = text;
            difference.group = "packets";
            difference.expected = valueText(a.packet(index) + offset, 4u);
            difference.actual = valueText(b.packet(index) + offset, 4u);
            return difference;
        }
        return difference;
    }

    void prepareLaunch(VU1State &state, uint32_t entryPc, uint32_t top, uint32_t itop)
    {
        state.pc = entryPc & kPcMask;
        state.ebit = false;
        state.haltAfterDelaySlot = false;
        state.stoppedByD = false;
        state.stoppedByT = false;
        state.top = top;
        state.itop = itop;
        state.branchPending = false;
        state.branchTarget = 0u;
        state.branchDelay = 0u;
        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
    }
}
