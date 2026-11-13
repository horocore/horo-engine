#include "PrefabValueResolutionInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Prefab {
    namespace {
        using namespace ValueDetail;

        /** @brief Retain each record's original complete source/schema context across
         * repeated rebase and module-unavailable cycles. */
        struct Intent {
            const PrefabValueOverrideRecord *record;
            const PrefabSourceRevision *source;
            Sha256Digest schema;
        };

        /** @brief Enumerate normal and preserved records canonically without copying
         * their owned payloads. */
        std::vector<Intent> Intents(const PrefabValueOverrideSet &set) {
            std::vector<Intent> records;
            records.reserve(set.records.size() + set.conflicts.size() + set.orphans.size());
            for (const auto &record : set.records)
                records.emplace_back(&record, &set.authoredAgainst, set.schemaRevision);
            for (const auto &conflict : set.conflicts)
                records.emplace_back(&conflict.original, &conflict.authoredAgainst, conflict.schemaRevision);
            for (const auto &orphan : set.orphans)
                records.emplace_back(&orphan.original, &orphan.authoredAgainst, orphan.schemaRevision);
            std::ranges::sort(records, [](const Intent &a, const Intent &b) {
                return TargetLess(a.record->target, b.record->target);
            });
            return records;
        }

        /** @brief Explain unavailability without guessing a replacement property or
         * component occurrence. */
        PrefabValueOverrideIssue MissingReason(const ResolutionState &state, const Intent &intent) {
            using enum PrefabValueOverrideIssue;
            const auto *property = std::get_if<PrefabPropertyAddress>(&intent.record->target);
            if (!property)
                return TargetRemoved;
            if (!state.schemas || !state.schemas->Find(*property))
                return SchemaUnavailable;
            return TargetRemoved;
        }

        /** @brief Keeps exact validating schema/version checks independent from value application. */
        bool SchemaChanged(const ResolutionState &state, const ValueBinding &binding, const Intent &intent) noexcept {
            return binding.schema && (intent.schema != state.schemas->Revision() ||
                                      intent.record->componentSchemaVersion != binding.schema->componentSchemaVersion);
        }

        /** @brief Resolve one immutable precondition before writing operation-owned
         * component data. */
        Result<void> ApplyIntent(ResolutionState &state, const Intent &intent, PrefabOverrideResolution &result) {
            if (state.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
            if (auto charged = state.budget.Consume(1); charged.HasError())
                return charged;
            const auto &record = *intent.record;
            auto *binding = FindBinding(state, record.target);
            if (!binding) {
                result.rebased.orphans.emplace_back(record, *intent.source, intent.schema, MissingReason(state, intent));
                return Result<void>::Success();
            }
            if (SchemaChanged(state, *binding, intent)) {
                result.rebased.orphans.emplace_back(record, *intent.source, intent.schema, PrefabValueOverrideIssue::SchemaChanged);
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
                result.rebased.conflicts.emplace_back(record, *intent.source, intent.schema, digest, state.source.SourceRevision());
                return Result<void>::Success();
            }
            if (auto applied = WriteBinding(state, *binding, record.value); applied.HasError())
                return applied;
            result.rebased.records.emplace_back(record);
            return Result<void>::Success();
        }

        /** @brief Publishes only a complete detached canonical object collection and its exact delta commitment. */
        Result<PrefabOverrideResolution> FinishResolution(ResolutionState &state, const PrefabValueOverrideSet &set,
                                                          PrefabOverrideResolution result) {
            result.provenance.reserve(state.bindings.size());
            for (auto &binding : state.bindings)
                result.provenance.push_back(std::move(binding.provenance));
            if (!result.rebased.HasUnresolved()) {
                auto bytes = SerializePrefabValueOverrides(set, state.limits);
                if (bytes.HasError())
                    return Result<PrefabOverrideResolution>::Failure(bytes.ErrorValue());
                const auto digest = ComputeSha256(std::as_bytes(std::span(bytes.Value().data(), bytes.Value().size())));
                result.effective = std::move(state.objects);
                result.appliedDigest = digest;
            }
            if (state.cancellation.IsCancellationRequested())
                return Result<PrefabOverrideResolution>::Failure(MakeError(PrefabErrors::Cancelled));
            return Result<PrefabOverrideResolution>::Success(std::move(result));
        }

        /** @brief Complete one bounded detached resolution; errors expose no partially
         * changed candidate. */
        Result<PrefabOverrideResolution> Resolve(const PrefabOverrideBaseline &source, const PrefabValueOverrideSet &set,
                                                 const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits,
                                                 const CancellationToken &cancellation) {
            if (set.sourcePrefab != source.RootAsset() || source.ValueOverridesDigest() != Sha256Digest{})
                return Result<PrefabOverrideResolution>::Failure(MakeError(PrefabErrors::OverrideSourceStale));
            if (auto valid = ValidatePrefabValueOverrideSet(set, limits); valid.HasError())
                return Result<PrefabOverrideResolution>::Failure(valid.ErrorValue());
            ResolutionState state{source, schemas, limits, cancellation, PrefabExpansionBudget{limits}, {}, {}, {}, {}, 0};
            if (auto captured = CaptureBindings(state); captured.HasError())
                return Result<PrefabOverrideResolution>::Failure(captured.ErrorValue());
            PrefabOverrideResolution result;
            result.rebased = {source.RootAsset(), source.SourceRevision(), schemas ? schemas->Revision() : set.schemaRevision, {}, {}, {}};
            for (const auto &intent : Intents(set))
                if (auto applied = ApplyIntent(state, intent, result); applied.HasError())
                    return Result<PrefabOverrideResolution>::Failure(applied.ErrorValue());
            if (auto finished = FinishPayloads(state); finished.HasError())
                return Result<PrefabOverrideResolution>::Failure(finished.ErrorValue());
            if (auto topology = ResolveComponentIntents(state, set, result); topology.HasError())
                return Result<PrefabOverrideResolution>::Failure(topology.ErrorValue());
            return FinishResolution(state, set, std::move(result));
        }

        /** @brief Explicit edit removes exactly the selected normal/conflict/orphan
         * record, leaving every peer intent intact. */
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

        /** @brief Creates one exact assignment precondition from actual inspected provenance, never a caller digest. */
        Result<void> Assign(PrefabValueOverrideSet &next, const PrefabOverrideResolution &state, const PrefabValueEdit &edit,
                            const PrefabValueSchemaSnapshot *schemas) {
            const auto binding = std::ranges::find_if(state.provenance, [&](const auto &value) {
                return value.disposition == PrefabValueDisposition::Present && value.target == edit.target;
            });
            if (binding == state.provenance.end())
                return Result<void>::Failure(MakeError(PrefabErrors::OverrideOrphan));
            const auto *property = std::get_if<PrefabPropertyAddress>(&edit.target);
            const auto *schema = property && schemas ? schemas->Find(*property) : nullptr;
            if (property && !schema)
                return Result<void>::Failure(MakeError(PrefabErrors::OverrideOrphan));
            if (auto valid = ValidateValue(edit.value, schema); valid.HasError())
                return valid;
            if (std::holds_alternative<PrefabObjectAddress>(edit.target) != std::holds_alternative<Math::Transform>(edit.value))
                return Invalid<void>();
            next.records.emplace_back(edit.target, schema ? schema->componentSchemaVersion : 0, ValueDigest(binding->sourceValue),
                                      edit.value);
            return Result<void>::Success();
        }

        /** @brief Lowers one typed edit through the same initial inspection and final canonical resolver. */
        Result<PrefabOverrideResolution> PrepareEdit(const PrefabOverrideBaseline &source,
                                                     const std::optional<PrefabValueOverrideSet> &current, const PrefabValueEdit &edit,
                                                     const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits,
                                                     const CancellationToken &cancellation) {
            const PrefabValueOverrideSet empty{source.RootAsset(),
                                               source.SourceRevision(),
                                               schemas ? schemas->Revision() : Sha256Digest{},
                                               {},
                                               {},
                                               {}};
            const auto &input = current ? *current : empty;
            auto inspected = Resolve(source, input, schemas, limits, cancellation);
            if (inspected.HasError() || edit.kind == PrefabValueEditKind::Rebase)
                return inspected;
            if (edit.kind != PrefabValueEditKind::Assign && edit.kind != PrefabValueEditKind::Revert)
                return Invalid<PrefabOverrideResolution>();
            auto state = std::move(inspected).Value();
            auto next = std::move(state.rebased);
            RemoveTarget(next, edit.target);
            if (edit.kind == PrefabValueEditKind::Assign)
                if (auto assigned = Assign(next, state, edit, schemas); assigned.HasError())
                    return Result<PrefabOverrideResolution>::Failure(assigned.ErrorValue());
            return Resolve(source, next, schemas, limits, cancellation);
        }
    }  // namespace

    /** @copydoc ResolvePrefabValueOverrides */
    Result<PrefabOverrideResolution> ResolvePrefabValueOverrides(const PrefabOverrideBaseline &source, const PrefabValueOverrideSet &set,
                                                                 const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits,
                                                                 const CancellationToken &cancellation) {
        PrefabMetricOperation observation{source.Metrics(), PrefabMetricPhase::Rebase};
        auto result = [&]() -> Result<PrefabOverrideResolution> {
            try {
                return Resolve(source, set, schemas, limits, cancellation);
            } catch (const std::bad_alloc &) {
                return Result<PrefabOverrideResolution>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
            } catch (const ValueDetail::Json::exception &) {
                return ValueDetail::Invalid<PrefabOverrideResolution>("Component schema/value encoding is invalid.");
            }
        }();
        if (result.HasError())
            observation.Fail(result.ErrorValue());
        else {
            observation.Observe(PrefabMetricItem::Overrides, set.records.size() + set.conflicts.size() + set.orphans.size() +
                                                                 set.componentRecords.size() + set.componentConflicts.size() +
                                                                 set.componentOrphans.size());
            observation.Complete();
        }
        return result;
    }

    /** @copydoc PreparePrefabValueEdit */
    Result<PrefabOverrideResolution> PreparePrefabValueEdit(const PrefabOverrideBaseline &source,
                                                            const std::optional<PrefabValueOverrideSet> &current,
                                                            const PrefabValueEdit &edit, const PrefabValueSchemaSnapshot *schemas,
                                                            const PrefabLimitProfile &limits, const CancellationToken &cancellation) {
        PrefabMetricOperation observation{source.Metrics(), PrefabMetricPhase::Rebase};
        auto result = [&]() -> Result<PrefabOverrideResolution> {
            try {
                return PrepareEdit(source, current, edit, schemas, limits, cancellation);
            } catch (const std::bad_alloc &) {
                return Result<PrefabOverrideResolution>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
            } catch (const ValueDetail::Json::exception &) {
                return ValueDetail::Invalid<PrefabOverrideResolution>();
            }
        }();
        if (result.HasError())
            observation.Fail(result.ErrorValue());
        else {
            observation.Observe(PrefabMetricItem::Overrides, result.Value().rebased.records.size() +
                                                                 result.Value().rebased.conflicts.size() +
                                                                 result.Value().rebased.orphans.size());
            observation.Complete();
        }
        return result;
    }
}  // namespace Horo::Prefab
