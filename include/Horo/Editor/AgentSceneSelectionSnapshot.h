#pragma once

/** @file AgentSceneSelectionSnapshot.h @brief Bounded scene selection evidence for explicit agent consent. */

#include "Horo/Foundation/CancellationToken.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Horo::Editor {
    /** @brief Owner-issued IDs from the selected scene revision, with no runtime handles or scene contents. */
    struct AgentSceneSelectionSnapshot final {
        static constexpr std::size_t MaximumObjects = 16;
        std::uint64_t documentEpoch{};
        std::uint64_t documentRevision{};
        std::uint64_t selectionRevision{};
        std::array<std::uint64_t, MaximumObjects> objects{};
        std::size_t objectCount{};
        bool truncated{};
        CancellationToken lifetime; /**< Revoked by the owner when the source scene or selection changes. */
    };
}  // namespace Horo::Editor
