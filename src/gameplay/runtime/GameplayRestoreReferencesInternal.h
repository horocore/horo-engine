#pragma once
#include "Horo/Gameplay/BehaviorRestore.h"
#include "Horo/Runtime/Scene/RestoreReferenceGraph.h"

namespace Horo::Gameplay::Detail {
    [[nodiscard]] Result<Runtime::RestoreReferenceContribution> ConvertBehaviorRestoreReferences(
        std::span<const BehaviorRestoreReference>, const BehaviorPersistenceTarget &, const Runtime::RestoreReferenceAuthorities &,
        const Runtime::SaveParticipantId &, const Runtime::RestoreReferenceGraphLimits &);
}
