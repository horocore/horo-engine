#include "PrefabValueResolutionInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Prefab {
    /** @brief The only instance-override factory of immutable effective candidates. Source construction remains resolver-owned. */
    class PrefabValueOverrideResolver final {
    public:
        static EffectivePrefabCandidate Publish(const EffectivePrefabCandidate &source, std::vector<ResolvedPrefabObject> objects,
                                                const Sha256Digest digest) {
            return EffectivePrefabCandidate{source.RootAsset(), source.Revision(), std::move(objects), digest};
        }
    };

    namespace {
        using namespace ValueDetail;

        /** @brief Retain each record's original complete source/schema context across repeated rebase and module-unavailable cycles. */
        struct Intent {
            const PrefabValueOverrideRecord *record;
            const PrefabSourceRevision *source;
            Sha256Digest schema;
        };

        /** @brief Enumerate normal and preserved records canonically without copying their owned payloads. */
        std::vector<Intent> Intents(const PrefabValueOverrideSet &set) {
            std::vector<Intent> records;
            records.reserve(set.records.size() + set.conflicts.size() + set.orphans.size());
            for (const auto &record : set.records)
                records.push_back({&record, &set.authoredAgainst, set.schemaRevision});
            for (const auto &conflict : set.conflicts)
                records.push_back({&conflict.original, &conflict.authoredAgainst, conflict.schemaRevision});
            for (const auto &orphan : set.orphans)
                records.push_back({&orphan.original, &orphan.authoredAgainst, orphan.schemaRevision});
            std::ranges::sort(records, [](const Intent &a, const Intent &b) {
                return TargetLess(a.record->target, b.record->target);
            });
            return records;
        }

        /** @brief Explain unavailability without guessing a replacement property or component occurrence. */
        PrefabValueOverrideIssue MissingReason(const ResolutionState &state, const Intent &intent) {
            const auto *property = std::get_if<PrefabPropertyAddress>(&intent.record->target);
            if (!property)
                return PrefabValueOverrideIssue::TargetRemoved;
            if (!state.schemas || !state.schemas->Find(*property))
                return PrefabValueOverrideIssue::SchemaUnavailable;
            return PrefabValueOverrideIssue::TargetRemoved;
        }

        /** @brief Resolve one immutable precondition before writing operation-owned component data. */
        Result<void> ApplyIntent(ResolutionState &state, const Intent &intent, PrefabValueOverrideResolution &result) {
            if (state.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
            if (auto charged = state.budget.Consume(1); charged.HasError())
                return charged;
            const auto &record = *intent.record;
            auto *binding = FindBinding(state, record.target);
            if (!binding) {
                result.rebased.orphans.push_back({record, *intent.source, intent.schema, MissingReason(state, intent)});
                return Result<void>::Success();
            }
            if (binding->schema &&
                (intent.schema != state.schemas->Revision() || record.componentSchemaVersion != binding->schema->componentSchemaVersion)) {
                result.rebased.orphans.push_back({record, *intent.source, intent.schema, PrefabValueOverrideIssue::SchemaChanged});
                return Result<void>::Success();
            }
            if (auto valid = ValidateValue(record.value, binding->schema); valid.HasError())
                return valid;
            const auto digest = ValueDigest(binding->provenance.sourceValue);
            if (SameValue(record.value, binding->provenance.sourceValue)) {
                ++result.redundantRecords;
                return Result<void>::Success();
            }
            if (record.expectedSourceValue != digest) {
                result.rebased.conflicts.push_back({record, *intent.source, intent.schema, digest, state.source.Revision().rootSource});
                return Result<void>::Success();
            }
            if (auto applied = WriteBinding(state, *binding, record.value); applied.HasError())
                return applied;
            result.rebased.records.push_back(record);
            return Result<void>::Success();
        }

        /** @brief Complete one bounded detached resolution; errors expose no partially changed candidate. */
        Result<PrefabValueOverrideResolution> Resolve(const EffectivePrefabCandidate &source, const PrefabValueOverrideSet &set,
                                                      const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits,
                                                      const CancellationToken &cancellation) {
            if (set.sourcePrefab != source.RootAsset() || source.ValueOverridesDigest() != Sha256Digest{})
                return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::OverrideSourceStale));
            if (auto valid = ValidatePrefabValueOverrideSet(set, limits); valid.HasError())
                return Result<PrefabValueOverrideResolution>::Failure(valid.ErrorValue());
            ResolutionState state{source, schemas, limits, cancellation, PrefabExpansionBudget{limits}, {}, {}, {}, {}, 0};
            if (auto captured = CaptureBindings(state); captured.HasError())
                return Result<PrefabValueOverrideResolution>::Failure(captured.ErrorValue());
            PrefabValueOverrideResolution result;
            result.rebased =
                {source.RootAsset(), source.Revision().rootSource, schemas ? schemas->Revision() : set.schemaRevision, {}, {}, {}};
            for (const auto &intent : Intents(set))
                if (auto applied = ApplyIntent(state, intent, result); applied.HasError())
                    return Result<PrefabValueOverrideResolution>::Failure(applied.ErrorValue());
            result.provenance.reserve(state.bindings.size());
            for (auto &binding : state.bindings)
                result.provenance.push_back(std::move(binding.provenance));
            if (result.rebased.conflicts.empty() && result.rebased.orphans.empty()) {
                if (auto finished = FinishPayloads(state); finished.HasError())
                    return Result<PrefabValueOverrideResolution>::Failure(finished.ErrorValue());
                auto bytes = SerializePrefabValueOverrides(set, limits);
                if (bytes.HasError())
                    return Result<PrefabValueOverrideResolution>::Failure(bytes.ErrorValue());
                const auto digest = ComputeSha256(std::as_bytes(std::span(bytes.Value().data(), bytes.Value().size())));
                result.effective = PrefabValueOverrideResolver::Publish(source, std::move(state.objects), digest);
            }
            if (cancellation.IsCancellationRequested())
                return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::Cancelled));
            return Result<PrefabValueOverrideResolution>::Success(std::move(result));
        }

        /** @brief Explicit edit removes exactly the selected normal/conflict/orphan record, leaving every peer intent intact. */
        void RemoveTarget(PrefabValueOverrideSet &set, const PrefabValueOverrideTarget &target) {
            std::erase_if(set.records, [&target](const auto &record) {
                return record.target == target;
            });
            std::erase_if(set.conflicts, [&target](const auto &conflict) {
                return conflict.original.target == target;
            });
            std::erase_if(set.orphans, [&target](const auto &orphan) {
                return orphan.original.target == target;
            });
        }
    }  // namespace

    /** @copydoc ResolvePrefabValueOverrides */
    Result<PrefabValueOverrideResolution> ResolvePrefabValueOverrides(const EffectivePrefabCandidate &source,
                                                                      const PrefabValueOverrideSet &set,
                                                                      const PrefabValueSchemaSnapshot *schemas,
                                                                      const PrefabLimitProfile &limits,
                                                                      const CancellationToken &cancellation) {
        try {
            return Resolve(source, set, schemas, limits, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        } catch (const ValueDetail::Json::exception &) {
            return ValueDetail::Invalid<PrefabValueOverrideResolution>("Component schema/value encoding is invalid.");
        }
    }

    /** @copydoc PreparePrefabValueEdit */
    Result<PrefabValueOverrideResolution> PreparePrefabValueEdit(const EffectivePrefabCandidate &source,
                                                                 const std::optional<PrefabValueOverrideSet> &current,
                                                                 const PrefabValueEdit &edit, const PrefabValueSchemaSnapshot *schemas,
                                                                 const PrefabLimitProfile &limits, const CancellationToken &cancellation) {
        try {
            const PrefabValueOverrideSet empty{source.RootAsset(),
                                               source.Revision().rootSource,
                                               schemas ? schemas->Revision() : Sha256Digest{},
                                               {},
                                               {},
                                               {}};
            auto inspected = Resolve(source, current ? *current : empty, schemas, limits, cancellation);
            if (inspected.HasError() || edit.kind == PrefabValueEditKind::Rebase)
                return inspected;
            if (edit.kind != PrefabValueEditKind::Assign && edit.kind != PrefabValueEditKind::Revert)
                return ValueDetail::Invalid<PrefabValueOverrideResolution>();
            auto state = std::move(inspected).Value();
            auto next = std::move(state.rebased);
            RemoveTarget(next, edit.target);
            if (edit.kind == PrefabValueEditKind::Assign) {
                const auto binding = std::ranges::find(state.provenance, edit.target, &PrefabValueProvenance::target);
                if (binding == state.provenance.end())
                    return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::OverrideOrphan));
                const auto *property = std::get_if<PrefabPropertyAddress>(&edit.target);
                const auto *schema = property && schemas ? schemas->Find(*property) : nullptr;
                if (property && !schema)
                    return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::OverrideOrphan));
                if (auto valid = ValueDetail::ValidateValue(edit.value, schema); valid.HasError())
                    return Result<PrefabValueOverrideResolution>::Failure(valid.ErrorValue());
                if (std::holds_alternative<PrefabObjectAddress>(edit.target) != std::holds_alternative<Math::Transform>(edit.value))
                    return ValueDetail::Invalid<PrefabValueOverrideResolution>();
                next.records.push_back(
                    {edit.target, schema ? schema->componentSchemaVersion : 0, ValueDetail::ValueDigest(binding->sourceValue), edit.value});
            }
            return Resolve(source, next, schemas, limits, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        } catch (const ValueDetail::Json::exception &) {
            return ValueDetail::Invalid<PrefabValueOverrideResolution>();
        }
    }
}  // namespace Horo::Prefab
