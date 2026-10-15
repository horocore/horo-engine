#pragma once
#include "PrefabValueOverridesInternal.h"

#include <map>
#include <set>

namespace Horo::Prefab::ValueDetail {
    /** @brief Private borrowed access metadata exists only during one synchronous detached resolution. */
    struct ValueBinding {
        PrefabValueProvenance provenance;
        std::size_t object{};
        std::optional<std::size_t> component;
        const PrefabValuePropertySchema *schema{};
    };

    /** @brief All mutable preparation storage is owned by this bounded operation, never by a widget or source document. */
    struct ResolutionState {
        const EffectivePrefabCandidate &source;
        const PrefabValueSchemaSnapshot *schemas;
        const PrefabLimitProfile &limits;
        const CancellationToken &cancellation;
        PrefabExpansionBudget budget;
        std::vector<ResolvedPrefabObject> objects;
        std::vector<ValueBinding> bindings;
        std::map<std::pair<std::size_t, std::size_t>, Json> payloads;
        std::set<std::pair<std::size_t, std::size_t>> modifiedPayloads;
        std::size_t provenanceBytes{};
    };

    [[nodiscard]] Result<void> CaptureBindings(ResolutionState &state);
    [[nodiscard]] ValueBinding *FindBinding(ResolutionState &state, const PrefabValueOverrideTarget &target);
    [[nodiscard]] Result<void> WriteBinding(ResolutionState &state, ValueBinding &binding, const PrefabOverrideValue &value);
    [[nodiscard]] Result<void> FinishPayloads(ResolutionState &state);
}  // namespace Horo::Prefab::ValueDetail
