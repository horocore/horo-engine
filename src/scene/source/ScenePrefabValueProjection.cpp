#include "ScenePrefabProjectionContext.h"

namespace Horo::SceneSource::Detail {
    /** @copydoc ApplyInstanceValues */
    void ApplyInstanceValues(ScenePrefabInstanceProjection &entry, const Prefab::PrefabValueSchemaSnapshot *schemas,
                             const Prefab::PrefabLimitProfile &limits, const CancellationToken &cancellation) {
        if (!entry.authored.valueOverrides || !entry.expanded)
            return;
        entry.schemaRevision = schemas ? schemas->Revision() : Sha256Digest{};
        auto resolved = Prefab::ResolvePrefabValueOverrides(*entry.expanded, *entry.authored.valueOverrides, schemas, limits, cancellation);
        if (resolved.HasError()) {
            entry.failure = resolved.ErrorValue();
            entry.expanded.reset();
        } else {
            auto values = std::move(resolved).Value();
            entry.rebasedValues = std::move(values.rebased);
            entry.valueProvenance = std::move(values.provenance);
            entry.expanded = std::move(values.effective);
            if (!entry.expanded)
                entry.failure = MakeError(entry.rebasedValues->conflicts.empty() ? Prefab::PrefabErrors::OverrideOrphan
                                                                                 : Prefab::PrefabErrors::OverrideConflict);
        }
        if (entry.failure)
            AddInstanceContext(*entry.failure, entry);
    }

    /** @copydoc ValidateInstanceValues */
    Result<void> ValidateInstanceValues(const ScenePrefabInstanceProjection &entry, const Prefab::PrefabValueSchemaSnapshot *schemas,
                                        const Prefab::PrefabLimitProfile &limits) {
        Sha256Digest expected{};
        if (entry.authored.valueOverrides) {
            if (entry.schemaRevision != (schemas ? schemas->Revision() : Sha256Digest{}))
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
            auto bytes = Prefab::SerializePrefabValueOverrides(*entry.authored.valueOverrides, limits);
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            expected = ComputeSha256(std::as_bytes(std::span(bytes.Value().data(), bytes.Value().size())));
        }
        if (!entry.expanded || entry.expanded->ValueOverridesDigest() != expected)
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
        return Result<void>::Success();
    }
}  // namespace Horo::SceneSource::Detail
