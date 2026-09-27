#pragma once

/** @file DestructionReplicationFields.h @brief Private stable field IDs and exact canonical widths for DFR wire version 1. */

#include "Horo/Destruction/DestructionReplication.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Horo::Destruction::Detail {
    inline constexpr std::uint64_t ReplicationSchemaValue = 0x4446520000000001ULL;
    inline constexpr std::array<std::uint32_t, 10> FieldValues{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    inline constexpr std::array<std::uint32_t, 10> MaximumFieldBytes{24, 56, 8, 8, 8, 5, 20, 516, 8200, 4};
    inline constexpr std::size_t FixedFieldBytes = 24 + 56 + 8 + 8 + 8 + 5 + 20 + 4 + 4 + 4;
    inline constexpr std::size_t MaximumPayloadBytes = 16 * 1024;

    [[nodiscard]] inline Network::ReplicationSchemaId SchemaId() {
        return Network::ReplicationSchemaId::Create(ReplicationSchemaValue).Value();
    }

    [[nodiscard]] inline Network::FieldId FieldId(const std::size_t index) {
        return Network::FieldId::Create(FieldValues[index]).Value();
    }
}  // namespace Horo::Destruction::Detail
