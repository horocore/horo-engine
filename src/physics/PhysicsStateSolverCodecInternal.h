#pragma once

#include "PhysicsStateSolverFields.h"
#include "PhysicsStateSolverStorage.h"

namespace Horo::Physics::StateDetail {
    inline constexpr std::uint32_t SolverWireTag = 0x31535048U;
    inline constexpr std::size_t SolverWireHeaderBytes = 48;
    inline constexpr std::array<std::size_t, 6> SolverRecordBytes{164, 8, 75, 48, 164, 24};

    /** @brief Header counts have a fixed meaning independent of platform layout. */
    struct SolverHeader final {
        std::uint32_t tag{SolverWireTag}, version{PhysicsStateSolverSchemaVersion};
        float previousDelta{};
        std::array<float, 3> gravity{};
        std::array<std::uint32_t, 6> counts{};
    };

    /** @brief One bounded required header, with no ignored extensions or optional
     * unknown fields. */
    inline void HeaderFields(Wire &wire, SolverHeader &header) noexcept {
        wire.Integer(header.tag);
        wire.Integer(header.version);
        wire.Float(header.previousDelta);
        wire.Floats(header.gravity);
        for (auto &count : header.counts)
            wire.Integer(count);
    }

    /** @brief Admits the exact full wire size before parsing records or writing
     * output. */
    inline bool WireSize(const SolverHeader &header, std::size_t ceiling, std::size_t &size) noexcept {
        if (SolverWireHeaderBytes > ceiling)
            return false;
        size = SolverWireHeaderBytes;
        for (std::size_t index = 0; index < header.counts.size(); ++index) {
            if (header.counts[index] > (ceiling - size) / SolverRecordBytes[index])
                return false;
            size += header.counts[index] * SolverRecordBytes[index];
        }
        return true;
    }

    /** @brief Encodes copies of scalar records so the source remains immutable
     * under either wire direction. */
    template <class T, class Encode> void WriteRows(Wire &wire, std::span<const T> rows, Encode encode) noexcept {
        for (const auto &row : rows) {
            T copied = row;
            encode(wire, copied);
        }
    }

    /** @brief Initializes exactly the already-admitted staging prefix, including
     * unused fields. */
    template <class T, class Decode> void ReadRows(Wire &wire, std::span<T> rows, Decode decode) noexcept {
        for (auto &row : rows) {
            row = T{};
            decode(wire, row);
        }
    }
}  // namespace Horo::Physics::StateDetail
