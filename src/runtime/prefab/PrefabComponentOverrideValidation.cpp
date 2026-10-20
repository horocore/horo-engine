#include "PrefabValueOverridesInternal.h"

#include <array>
#include <set>

namespace Horo::Prefab::ValueDetail {
    namespace {
        using Occurrence = std::pair<PrefabObjectAddress, std::uint64_t>;

        /** @brief Admit the original closed record before target access or opaque-byte encoding. */
        Result<void> ValidateRecord(const PrefabComponentOverrideRecord &record) {
            if (record.valueless_by_exception())
                return Invalid<void>();
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record))
                return ValidateRawComponentPayload(add->payload);
            const auto &remove = std::get<PrefabRemoveComponentOverride>(record);
            if (!remove.target.type.IsValid() || !remove.target.instance.IsValid() || remove.componentSchemaVersion == 0)
                return Invalid<void>("Component removal requires exact stable type, occurrence and schema identities.");
            return Result<void>::Success();
        }

        /** @brief Bound standalone operation bytes separately from preserved metadata and aggregate wire storage. */
        Result<void> Charge(const PrefabComponentOverrideRecord &record, const Wire &encoded, const PrefabProjectPolicy &policy,
                            std::size_t &bytes) {
            const auto standalone = EncodeComponentRecord(record).dump(0).size();
            if (standalone > policy.maximumOverrideValueBytes)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            const auto size = encoded.dump(0).size();
            if (bytes > policy.maximumOverrideSetBytes || size > policy.maximumOverrideSetBytes - bytes ||
                policy.maximumOverrideSetBytes - bytes - size < 2)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            bytes += size + 2;
            return Result<void>::Success();
        }

        /** @brief Instance identity is unique on an object even when two records claim different component types. */
        Result<void> InsertOccurrence(const PrefabComponentOverrideRecord &record, std::set<Occurrence> &targets) {
            const auto target = ComponentTarget(record);
            if (!targets.emplace(target.object, target.instance.Value()).second)
                return Result<void>::Failure(
                    MakeError(PrefabErrors::OverrideConflict, "Duplicate component intent cannot add/remove the same occurrence twice."));
            return Result<void>::Success();
        }

        /** @brief Same-layer property patches cannot target a component being added or removed by that layer. */
        Result<void> CheckValueTarget(const PrefabValueOverrideRecord &record, const std::set<Occurrence> &targets) {
            const auto *property = std::get_if<PrefabPropertyAddress>(&record.target);
            if (property && targets.contains({property->Object(), property->ComponentInstance().Value()}))
                return Result<void>::Failure(
                    MakeError(PrefabErrors::OverrideConflict, "A component topology change and a property patch cannot share a layer."));
            return Result<void>::Success();
        }

        /** @brief Validate one component operation and claim its occurrence before any candidate mutation. */
        Result<void> Admit(const PrefabComponentOverrideRecord &record, const PrefabProjectPolicy &policy, std::set<Occurrence> &targets) {
            if (auto valid = ValidateRecord(record); valid.HasError())
                return valid;
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record);
                add && add->payload.component.payload.size() > policy.maximumOverrideValueBytes)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            return InsertOccurrence(record, targets);
        }

        /** @brief Check ordinary and preserved value intent against the complete component occurrence claims. */
        Result<void> CheckValueTargets(const PrefabValueOverrideSet &set, const std::set<Occurrence> &targets) {
            for (const auto &record : set.records)
                if (auto valid = CheckValueTarget(record, targets); valid.HasError())
                    return valid;
            for (const auto &conflict : set.conflicts)
                if (auto valid = CheckValueTarget(conflict.original, targets); valid.HasError())
                    return valid;
            for (const auto &orphan : set.orphans)
                if (auto valid = CheckValueTarget(orphan.original, targets); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Check complete typed-vector totals before reserve, encoding or subtraction-based aggregate accounting. */
    Result<void> ValidateOverrideCounts(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy) {
        const std::array normal{set.records.size(), set.componentRecords.size()};
        const std::array preserved{set.conflicts.size(), set.orphans.size(), set.componentConflicts.size(), set.componentOrphans.size()};
        std::size_t issues{};
        for (const auto count : preserved) {
            if (count > policy.maximumConflictAndOrphanRecords - issues)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            issues += count;
        }
        if (issues > policy.maximumOverrideRecords)
            return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
        for (const auto count : normal) {
            if (count > policy.maximumOverrideRecords - issues)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            issues += count;
        }
        return Result<void>::Success();
    }

    /** @brief Admit all normal and preserved topology intent with exact shared count/byte and cross-kind conflict rules. */
    Result<void> ValidateComponentRecords(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy, std::size_t &bytes) {
        std::set<Occurrence> targets;
        for (const auto &record : set.componentRecords) {
            if (auto valid = Admit(record, policy, targets); valid.HasError())
                return valid;
            if (auto charged = Charge(record, EncodeComponentRecord(record), policy, bytes); charged.HasError())
                return charged;
        }
        for (const auto &conflict : set.componentConflicts) {
            if (auto valid = Admit(conflict.original, policy, targets); valid.HasError())
                return valid;
            if (!Detail::IsCanonicalProjectVersion(conflict.authoredAgainst.projectVersion) ||
                !Detail::IsCanonicalProjectVersion(conflict.observedSource.projectVersion))
                return Invalid<void>();
            if (auto charged = Charge(conflict.original, EncodeComponentConflict(conflict), policy, bytes); charged.HasError())
                return charged;
        }
        for (const auto &orphan : set.componentOrphans) {
            if (auto valid = Admit(orphan.original, policy, targets); valid.HasError())
                return valid;
            if (!Detail::IsCanonicalProjectVersion(orphan.authoredAgainst.projectVersion) ||
                orphan.reason > PrefabComponentOverrideIssue::SchemaChanged)
                return Invalid<void>();
            if (auto charged = Charge(orphan.original, EncodeComponentOrphan(orphan), policy, bytes); charged.HasError())
                return charged;
        }
        return CheckValueTargets(set, targets);
    }
}  // namespace Horo::Prefab::ValueDetail
