#include "Horo/Prefab/PrefabDebugSnapshot.h"

#include <algorithm>
#include <new>

namespace Horo::Prefab {
    namespace {
        /** @brief Admit a deterministic prefix while charging one shared copy/work budget. */
        PrefabDebugSection Admit(const std::size_t available, std::size_t &remaining) noexcept {
            const auto retained = std::min(available, remaining);
            remaining -= retained;
            return {available, retained};
        }

        /** @brief Validate only copied target identity, never inspect or encode its arbitrary value. */
        bool ValidTarget(const PrefabValueOverrideTarget &target, const PrefabProjectPolicy &policy) noexcept {
            if (const auto *property = std::get_if<PrefabPropertyAddress>(&target))
                return property->Object().NestedInstanceScope().size() <= policy.maximumNestedPrefabDepth &&
                       property->ComponentType().IsValid() && property->ComponentInstance().IsValid() && property->Property().IsValid();
            const auto *object = std::get_if<PrefabObjectAddress>(&target);
            return object && object->NestedInstanceScope().size() <= policy.maximumNestedPrefabDepth;
        }

        /** @brief Match retained provenance to an exact canonical object within the hard bounded expanded hierarchy. */
        bool MatchesObject(const PrefabValueProvenance &value, const EffectivePrefabCandidate &candidate) noexcept {
            const auto &address = std::holds_alternative<PrefabObjectAddress>(value.target)
                                      ? std::get<PrefabObjectAddress>(value.target)
                                      : std::get<PrefabPropertyAddress>(value.target).Object();
            return std::ranges::any_of(candidate.Objects(), [&](const auto &object) {
                return object.key.object == address && object.sourcePrefab == value.sourcePrefab;
            });
        }

        /** @brief Strip arbitrary value bytes while preserving the exact stable override target. */
        Result<PrefabDebugOverride> Summarize(const PrefabValueOverrideRecord &record, const PrefabDebugOverrideState state,
                                              const PrefabProjectPolicy &policy) {
            if (!ValidTarget(record.target, policy))
                return Result<PrefabDebugOverride>::Failure(MakeError(PrefabErrors::DiagnosticInvalid));
            return Result<PrefabDebugOverride>::Success({record.target, PrefabDebugOverrideKind::AssignValue, state});
        }

        /** @brief Strip component payload bytes without executing schema/provider code. */
        Result<PrefabDebugOverride> Summarize(const PrefabComponentOverrideRecord &record, const PrefabDebugOverrideState state,
                                              const PrefabProjectPolicy &policy) {
            PrefabComponentOverrideTarget target;
            auto kind = PrefabDebugOverrideKind::RemoveComponent;
            if (const auto *added = std::get_if<PrefabAddComponentOverride>(&record)) {
                target = {added->object, added->payload.component.typeId, added->payload.instance};
                kind = PrefabDebugOverrideKind::AddComponent;
            } else if (const auto *removed = std::get_if<PrefabRemoveComponentOverride>(&record))
                target = removed->target;
            else
                return Result<PrefabDebugOverride>::Failure(MakeError(PrefabErrors::DiagnosticInvalid));
            if (!target.type.IsValid() || !target.instance.IsValid() ||
                target.object.NestedInstanceScope().size() > policy.maximumNestedPrefabDepth)
                return Result<PrefabDebugOverride>::Failure(MakeError(PrefabErrors::DiagnosticInvalid));
            return Result<PrefabDebugOverride>::Success({std::move(target), kind, state});
        }

        /** @brief Append a bounded prefix without traversing omitted records or unresolved payloads. */
        template <typename Records>
        Result<void> AppendOverrides(std::vector<PrefabDebugOverride> &output, const Records &records, const PrefabDebugOverrideState state,
                                     std::size_t &remaining, const PrefabProjectPolicy &policy, const CancellationToken &cancellation) {
            const auto count = std::min(records.size(), remaining);
            for (std::size_t index = 0; index < count; ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
                const auto &record = records[index];
                auto summary = [&] {
                    if constexpr (requires { record.original; })
                        return Summarize(record.original, state, policy);
                    else
                        return Summarize(record, state, policy);
                }();
                if (summary.HasError())
                    return Result<void>::Failure(summary.ErrorValue());
                output.push_back(std::move(summary).Value());
            }
            remaining -= count;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PrefabDebugSnapshot::IsTruncated */
    bool PrefabDebugSnapshot::IsTruncated() const noexcept {
        return nodeCoverage_.Omitted() || edgeCoverage_.Omitted() || objectCoverage_.Omitted() || overrideCoverage_.Omitted() ||
               valueCoverage_.Omitted();
    }

    /** @copydoc CapturePrefabDebugSnapshot */
    Result<PrefabDebugSnapshot> CapturePrefabDebugSnapshot(const EffectivePrefabCandidate &candidate, const PrefabLimitProfile &policy,
                                                           const PrefabDebugCaptureLimits limits, const PrefabValueOverrideSet *overrides,
                                                           const std::span<const PrefabValueProvenance> provenance,
                                                           const std::size_t redundantRecords, const CancellationToken &cancellation) {
        using Return = Result<PrefabDebugSnapshot>;
        if (cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(PrefabErrors::Cancelled));
        if (limits.maximumEntries > PrefabDebugCaptureLimits::MaximumEntries)
            return Return::Failure(MakeError(PrefabErrors::DiagnosticBudgetExceeded));
        if (!candidate.RootAsset().IsValid() || candidate.Objects().empty() ||
            candidate.Objects().size() > PrefabHardLimits::SourceObjectCount ||
            (overrides && overrides->sourcePrefab != candidate.RootAsset()) ||
            (!overrides && (!provenance.empty() || redundantRecords != 0)))
            return Return::Failure(MakeError(PrefabErrors::DiagnosticInvalid));
        // Cardinality checks are constant-time and make the total below overflow-safe without visiting intent bytes.
        if (overrides && (overrides->records.size() > PrefabHardLimits::OverrideRecords ||
                          overrides->componentRecords.size() > PrefabHardLimits::OverrideRecords ||
                          overrides->conflicts.size() > PrefabHardLimits::ConflictAndOrphanRecords ||
                          overrides->orphans.size() > PrefabHardLimits::ConflictAndOrphanRecords ||
                          overrides->componentConflicts.size() > PrefabHardLimits::ConflictAndOrphanRecords ||
                          overrides->componentOrphans.size() > PrefabHardLimits::ConflictAndOrphanRecords))
            return Return::Failure(MakeError(PrefabErrors::DiagnosticInvalid));
        try {
            PrefabDebugSnapshot result;
            result.root_ = candidate.RootAsset();
            result.source_ = candidate.Revision().rootSource;
            result.registry_ = candidate.Revision().registry;
            result.policy_ = policy.Policy();
            result.expansionWorkCeiling_ = policy.MaximumExpansionWorkItems();
            result.valueOverridesDigest_ = candidate.ValueOverridesDigest();
            auto remaining = limits.maximumEntries;
            result.nodeCoverage_ = Admit(candidate.Revision().dependencies.size(), remaining);
            result.edgeCoverage_ = Admit(candidate.Revision().edges.size(), remaining);
            result.objectCoverage_ = Admit(candidate.Objects().size(), remaining);
            const auto overrideCount = overrides ? overrides->records.size() + overrides->conflicts.size() + overrides->orphans.size() +
                                                       overrides->componentRecords.size() + overrides->componentConflicts.size() +
                                                       overrides->componentOrphans.size()
                                                 : 0;
            result.overrideCoverage_ = Admit(overrideCount, remaining);
            result.valueCoverage_ = Admit(provenance.size(), remaining);
            result.nodes_.reserve(result.nodeCoverage_.retained);
            result.edges_.reserve(result.edgeCoverage_.retained);
            result.objects_.reserve(result.objectCoverage_.retained);
            result.values_.reserve(result.valueCoverage_.retained);
            result.overrideRecords_.reserve(result.overrideCoverage_.retained);
            for (const auto &node : std::span(candidate.Revision().dependencies).first(result.nodeCoverage_.retained)) {
                if (cancellation.IsCancellationRequested())
                    return Return::Failure(MakeError(PrefabErrors::Cancelled));
                result.nodes_.push_back(node);
            }
            for (const auto &edge : std::span(candidate.Revision().edges).first(result.edgeCoverage_.retained)) {
                if (cancellation.IsCancellationRequested())
                    return Return::Failure(MakeError(PrefabErrors::Cancelled));
                result.edges_.push_back(edge);
            }
            for (const auto &object : candidate.Objects().first(result.objectCoverage_.retained)) {
                if (cancellation.IsCancellationRequested())
                    return Return::Failure(MakeError(PrefabErrors::Cancelled));
                if (object.key.object.NestedInstanceScope().size() > policy.Policy().maximumNestedPrefabDepth)
                    return Return::Failure(MakeError(PrefabErrors::DiagnosticInvalid));
                result.objects_.push_back({object.sourcePrefab, object.key, object.parent, object.object.components.size()});
            }
            for (const auto &value : provenance.first(result.valueCoverage_.retained)) {
                if (cancellation.IsCancellationRequested())
                    return Return::Failure(MakeError(PrefabErrors::Cancelled));
                if (!ValidTarget(value.target, policy.Policy()) || !value.sourcePrefab.IsValid() || !MatchesObject(value, candidate) ||
                    (value.origin != PrefabValueOrigin::Source && value.origin != PrefabValueOrigin::Inherited &&
                     value.origin != PrefabValueOrigin::Instance))
                    return Return::Failure(MakeError(PrefabErrors::DiagnosticInvalid));
                result.values_.push_back({value.target, value.origin, value.sourcePrefab});
            }
            if (overrides) {
                auto intentRemaining = result.overrideCoverage_.retained;
                // Stable section order preserves authored targets without merging conflicting records.
                if (auto appended = AppendOverrides(result.overrideRecords_, overrides->records, PrefabDebugOverrideState::Active,
                                                    intentRemaining, policy.Policy(), cancellation);
                    appended.HasError())
                    return Return::Failure(std::move(appended).ErrorValue());
                if (auto appended = AppendOverrides(result.overrideRecords_, overrides->conflicts, PrefabDebugOverrideState::Conflict,
                                                    intentRemaining, policy.Policy(), cancellation);
                    appended.HasError())
                    return Return::Failure(std::move(appended).ErrorValue());
                if (auto appended = AppendOverrides(result.overrideRecords_, overrides->orphans, PrefabDebugOverrideState::Orphan,
                                                    intentRemaining, policy.Policy(), cancellation);
                    appended.HasError())
                    return Return::Failure(std::move(appended).ErrorValue());
                if (auto appended = AppendOverrides(result.overrideRecords_, overrides->componentRecords, PrefabDebugOverrideState::Active,
                                                    intentRemaining, policy.Policy(), cancellation);
                    appended.HasError())
                    return Return::Failure(std::move(appended).ErrorValue());
                if (auto appended = AppendOverrides(result.overrideRecords_, overrides->componentConflicts,
                                                    PrefabDebugOverrideState::Conflict, intentRemaining, policy.Policy(), cancellation);
                    appended.HasError())
                    return Return::Failure(std::move(appended).ErrorValue());
                if (auto appended = AppendOverrides(result.overrideRecords_, overrides->componentOrphans, PrefabDebugOverrideState::Orphan,
                                                    intentRemaining, policy.Policy(), cancellation);
                    appended.HasError())
                    return Return::Failure(std::move(appended).ErrorValue());
                result.overrides_ = {.supplied = true,
                                     .authoredAgainst = overrides->authoredAgainst,
                                     .schemaRevision = overrides->schemaRevision,
                                     .assignments = overrides->records.size(),
                                     .conflicts = overrides->conflicts.size(),
                                     .orphans = overrides->orphans.size(),
                                     .componentOperations = overrides->componentRecords.size(),
                                     .componentConflicts = overrides->componentConflicts.size(),
                                     .componentOrphans = overrides->componentOrphans.size(),
                                     .redundantRecords = redundantRecords};
            }
            return Return::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(PrefabErrors::DiagnosticBudgetExceeded));
        }
    }
}  // namespace Horo::Prefab
