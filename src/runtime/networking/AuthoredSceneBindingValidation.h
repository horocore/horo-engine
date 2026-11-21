#pragma once

#include "Horo/Network/ReplicationWorldLifecycle.h"

namespace Horo::Network::Detail {
    /** @brief Checks the canonical order and scope of one authority-issued initial identity. */
    [[nodiscard]] inline bool ValidAuthoredBinding(const ReplicationAuthoredObjectBinding &row, ReplicationAuthorityEpoch authority,
                                                   std::uint64_t slot, std::optional<Runtime::SceneObjectId> previous) noexcept {
        return row.object.IsValid() && row.object.Epoch() == authority && row.object.Slot() == slot && row.object.Generation() == 1 &&
               row.provenance.IsValid() && row.provenance.authoredObject && (!previous || *previous < *row.provenance.authoredObject);
    }
}  // namespace Horo::Network::Detail
