#include "Horo/Prefab/PrefabValueSnapshot.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Prefab {
    namespace {
        /** @brief Charges exact variable strings in addition to the fixed owned carrier. */
        std::size_t ValueBytes(const PrefabOverrideValue &value) {
            const auto *text = std::get_if<std::string>(&value);
            return sizeof(value) + (text ? text->size() : 0);
        }

        /** @brief Accounts nested target storage without interpreting labels or mutable documents. */
        std::size_t TargetBytes(const PrefabValueOverrideTarget &target) {
            const auto &object = std::holds_alternative<PrefabObjectAddress>(target) ? std::get<PrefabObjectAddress>(target)
                                                                                     : std::get<PrefabPropertyAddress>(target).Object();
            const auto *property = std::get_if<PrefabPropertyAddress>(&target);
            return sizeof(target) + object.NestedInstanceScope().size() * sizeof(LocalObjectId) +
                   (property ? property->ComponentType().Value().size() : 0);
        }

        /** @brief Charges preserved intent without serializing an alternate delta representation. */
        std::size_t RecordBytes(const PrefabValueOverrideRecord &record) {
            return sizeof(record) + TargetBytes(record.target) + ValueBytes(record.value);
        }

        /** @brief Charges every owned type, address and opaque payload in supported component intent. */
        std::size_t ComponentBytes(const PrefabComponentOverrideRecord &record) {
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record))
                return 2 * sizeof(PrefabComponentDiffEvidence) + add->object.NestedInstanceScope().size() * sizeof(LocalObjectId) +
                       add->payload.component.typeId.Value().size() + add->payload.component.payload.size();
            const auto &remove = std::get<PrefabRemoveComponentOverride>(record);
            return 2 * sizeof(PrefabComponentDiffEvidence) + remove.target.object.NestedInstanceScope().size() * sizeof(LocalObjectId) +
                   remove.target.type.Value().size();
        }
    }  // namespace

    /** @copydoc CapturePrefabValueSnapshotRevision */
    Result<PrefabValueSnapshotRevision> CapturePrefabValueSnapshotRevision(const EffectivePrefabCandidate &source,
                                                                           const std::optional<PrefabValueOverrideSet> &authored,
                                                                           const PrefabValueSchemaSnapshot *schemas,
                                                                           const PrefabLimitProfile &limits) {
        try {
            PrefabValueSnapshotRevision revision{source.Revision(), schemas ? schemas->Revision() : Sha256Digest{}, {}};
            if (authored) {
                auto encoded = SerializePrefabValueOverrides(*authored, limits);
                if (encoded.HasError())
                    return Result<PrefabValueSnapshotRevision>::Failure(encoded.ErrorValue());
                revision.delta = ComputeSha256(std::as_bytes(std::span{encoded.Value()}));
            }
            return Result<PrefabValueSnapshotRevision>::Success(std::move(revision));
        } catch (const std::bad_alloc &) {
            return Result<PrefabValueSnapshotRevision>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PrefabValueSnapshot::Prepare */
    Result<PrefabValueSnapshotPreparation> PrefabValueSnapshot::Prepare(const EffectivePrefabCandidate &source,
                                                                        const std::optional<PrefabValueOverrideSet> &authored,
                                                                        const PrefabValueSchemaSnapshot *schemas,
                                                                        const PrefabLimitProfile &limits,
                                                                        const PrefabValueSnapshotLimits capture,
                                                                        const CancellationToken &cancellation) {
        using Return = Result<PrefabValueSnapshotPreparation>;
        const PrefabValueOverrideSet empty{source.RootAsset(),
                                           source.Revision().rootSource,
                                           schemas ? schemas->Revision() : Sha256Digest{},
                                           {},
                                           {},
                                           {}};
        auto resolved = ResolvePrefabValueOverrides(source, authored ? *authored : empty, schemas, limits, cancellation);
        if (resolved.HasError())
            return Return::Failure(resolved.ErrorValue());
        auto inspection = [&]() -> Result<std::shared_ptr<const PrefabValueSnapshot>> {
            using Return = Result<std::shared_ptr<const PrefabValueSnapshot>>;
            if (capture.maximumEntries > PrefabHardLimits::OverrideRecords || capture.maximumBytes > PrefabHardLimits::SourceDocumentBytes)
                return Return::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            try {
                auto revision = CapturePrefabValueSnapshotRevision(source, authored, schemas, limits);
                if (revision.HasError())
                    return Return::Failure(revision.ErrorValue());
                auto snapshot = std::shared_ptr<PrefabValueSnapshot>{new PrefabValueSnapshot};
                snapshot->revision_ = std::move(revision).Value();
                std::size_t used{};
                bool omitted{};
                const auto retain = [&](const std::size_t bytes) {
                    ++snapshot->coverage_.total;
                    if (omitted || snapshot->coverage_.retained == capture.maximumEntries || bytes > capture.maximumBytes - used) {
                        omitted = true;
                        return false;
                    }
                    used += bytes;
                    ++snapshot->coverage_.retained;
                    return true;
                };
                for (const auto &value : resolved.Value().provenance) {
                    if (cancellation.IsCancellationRequested())
                        return Return::Failure(MakeError(PrefabErrors::Cancelled));
                    const auto bytes = 2 * sizeof(value) + TargetBytes(value.target) + ValueBytes(value.sourceValue) +
                                       ValueBytes(value.effectiveValue) + (value.schemaDefault ? ValueBytes(*value.schemaDefault) : 0) +
                                       value.originChain.size() * sizeof(PrefabCompositionOriginStep);
                    if (retain(bytes))
                        snapshot->values_.push_back(value);
                }
                for (const auto &conflict : resolved.Value().rebased.conflicts) {
                    if (cancellation.IsCancellationRequested())
                        return Return::Failure(MakeError(PrefabErrors::Cancelled));
                    if (retain(sizeof(conflict) + RecordBytes(conflict.original)))
                        snapshot->conflicts_.push_back(conflict);
                }
                for (const auto &orphan : resolved.Value().rebased.orphans) {
                    if (cancellation.IsCancellationRequested())
                        return Return::Failure(MakeError(PrefabErrors::Cancelled));
                    if (retain(sizeof(orphan) + RecordBytes(orphan.original)))
                        snapshot->orphans_.push_back(orphan);
                }
                const auto captureComponent = [&](const auto &evidence, const PrefabComponentOverrideRecord &record) -> Result<void> {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
                    if (retain(ComponentBytes(record)))
                        snapshot->components_.emplace_back(evidence);
                    return Result<void>::Success();
                };
                for (const auto &record : resolved.Value().rebased.componentRecords)
                    if (auto captured = captureComponent(record, record); captured.HasError())
                        return Return::Failure(captured.ErrorValue());
                for (const auto &conflict : resolved.Value().rebased.componentConflicts)
                    if (auto captured = captureComponent(conflict, conflict.original); captured.HasError())
                        return Return::Failure(captured.ErrorValue());
                for (const auto &orphan : resolved.Value().rebased.componentOrphans)
                    if (auto captured = captureComponent(orphan, orphan.original); captured.HasError())
                        return Return::Failure(captured.ErrorValue());
                PrefabExpansionBudget inspectionBudget{limits};
                for (const auto &object : source.Objects())
                    for (const auto &component : object.object.components) {
                        if (cancellation.IsCancellationRequested())
                            return Return::Failure(MakeError(PrefabErrors::Cancelled));
                        if (auto admitted = inspectionBudget.Consume(1); admitted.HasError())
                            return Return::Failure(admitted.ErrorValue());
                        bool known{};
                        if (schemas) {
                            const auto properties = schemas->Properties();
                            auto property = std::ranges::lower_bound(properties, std::string_view(component.component.typeId.Value()), {},
                                                                     [](const auto &value) {
                                return std::string_view(value.componentType.Value());
                            });
                            while (property != properties.end() && property->componentType == component.component.typeId) {
                                if (auto admitted = inspectionBudget.Consume(1); admitted.HasError())
                                    return Return::Failure(admitted.ErrorValue());
                                if (property->componentSchemaVersion == component.component.schemaVersion) {
                                    known = true;
                                    break;
                                }
                                ++property;
                            }
                        }
                        if (known)
                            continue;
                        if (retain(2 * sizeof(PrefabOpaqueValueEvidence) + component.component.typeId.Value().size() +
                                   object.key.object.NestedInstanceScope().size() * sizeof(LocalObjectId)))
                            snapshot->opaque_.push_back({object.key, component.component.typeId, component.instance,
                                                         component.component.schemaVersion, ComputeSha256(component.component.payload),
                                                         component.component.payload.size()});
                    }
                snapshot->retainedBytes_ = used;
                return Return::Success(std::move(snapshot));
            } catch (const std::bad_alloc &) {
                return Return::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
            }
        }();
        if (cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(PrefabErrors::Cancelled));
        if (inspection.HasError())
            return Return::Success({{}, std::move(resolved).Value(), std::move(inspection.ErrorValue())});
        return Return::Success({std::move(inspection).Value(), std::move(resolved).Value(), {}});
    }
}  // namespace Horo::Prefab
