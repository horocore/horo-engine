#include "ScenePrefabProjectionContext.h"

#include <algorithm>

namespace Horo::SceneSource::Detail {
    namespace {
        /** @brief Retain bounded inspection separately from the authoritative expansion outcome. */
        void CaptureDebug(ScenePrefabInstanceProjection &entry, const Prefab::EffectivePrefabCandidate &candidate,
                          const Prefab::PrefabLimitProfile &limits, const Prefab::PrefabValueOverrideSet *overrides,
                          const std::span<const Prefab::PrefabValueProvenance> provenance, const std::size_t redundant,
                          ScenePrefabDebugCaptureBudget &debugBudget, const CancellationToken &cancellation) {
            entry.debugSnapshot.reset();
            entry.debugCaptureFailure.reset();
            entry.debugCaptureOmitted = debugBudget.remainingEntries == 0 || debugBudget.remainingSnapshots == 0;
            if (entry.debugCaptureOmitted)
                return;
            --debugBudget.remainingSnapshots;
            auto snapshot = Prefab::CapturePrefabDebugSnapshot(candidate, limits,
                                                               {std::min(debugBudget.remainingEntries,
                                                                         Prefab::PrefabDebugCaptureLimits{}.maximumEntries)},
                                                               overrides, provenance, redundant, cancellation);
            if (snapshot.HasValue()) {
                debugBudget.remainingEntries -= snapshot.Value().NodeCoverage().retained + snapshot.Value().EdgeCoverage().retained +
                                                snapshot.Value().ObjectCoverage().retained + snapshot.Value().OverrideCoverage().retained +
                                                snapshot.Value().ValueCoverage().retained;
                entry.debugSnapshot = std::move(snapshot).Value();
            } else
                entry.debugCaptureFailure = std::move(snapshot).ErrorValue();
        }
    }  // namespace

    /** @copydoc ApplyInstanceValues */
    void ApplyInstanceValues(ScenePrefabInstanceProjection &entry, const Prefab::PrefabValueSchemaSnapshot *schemas,
                             const Prefab::PrefabLimitProfile &limits, ScenePrefabDebugCaptureBudget &debugBudget,
                             const CancellationToken &cancellation) {
        if (!entry.expanded)
            return;
        entry.valueSnapshot.reset();
        entry.valueInspectionFailure.reset();
        entry.schemaRevision = schemas ? schemas->Revision() : Sha256Digest{};
        if (auto resolved = Prefab::PrefabValueSnapshot::Prepare(*entry.expanded, entry.authored.valueOverrides, schemas, limits,
                                                                 {std::min(debugBudget.remainingValueEntries, std::size_t{256}),
                                                                  std::min(debugBudget.remainingValueBytes, std::size_t{1024 * 1024})},
                                                                 cancellation);
            resolved.HasError()) {
            entry.valueInspectionFailure = resolved.ErrorValue();
            CaptureDebug(entry, *entry.expanded, limits, entry.authored.valueOverrides ? &*entry.authored.valueOverrides : nullptr, {}, 0,
                         debugBudget, cancellation);
            if (entry.authored.valueOverrides) {
                entry.failure = resolved.ErrorValue();
                entry.expanded.reset();
            }
        } else {
            auto prepared = std::move(resolved).Value();
            entry.valueSnapshot = std::move(prepared.snapshot);
            entry.valueInspectionFailure = std::move(prepared.inspectionFailure);
            if (entry.valueSnapshot) {
                debugBudget.remainingValueEntries -= entry.valueSnapshot->Coverage().retained;
                debugBudget.remainingValueBytes -= entry.valueSnapshot->RetainedBytes();
            }
            auto &values = prepared.resolution;
            const auto *intent = entry.authored.valueOverrides ? &values.rebased : nullptr;
            const auto debugValues = intent ? std::span<const Prefab::PrefabValueProvenance>{values.provenance}
                                            : std::span<const Prefab::PrefabValueProvenance>{};
            CaptureDebug(entry, intent && values.effective ? *values.effective : *entry.expanded, limits, intent, debugValues,
                         intent ? values.redundantRecords : 0, debugBudget, cancellation);
            if (entry.authored.valueOverrides) {
                entry.rebasedValues = std::move(values.rebased);
                entry.expanded = std::move(values.effective);
                if (!entry.expanded)
                    entry.failure = MakeError(entry.rebasedValues->conflicts.empty() && entry.rebasedValues->componentConflicts.empty()
                                                  ? Prefab::PrefabErrors::OverrideOrphan
                                                  : Prefab::PrefabErrors::OverrideConflict);
            }
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
