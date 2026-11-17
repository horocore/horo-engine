#include "PrefabValueOverridesInternal.h"

#include <algorithm>
#include <array>
#include <set>
#include <tuple>

namespace Horo::Prefab::ValueDetail {
    namespace {
        using Occurrence = std::pair<PrefabObjectAddress, std::uint64_t>;

        /** @brief Admit the original closed record before target access or opaque-byte encoding. */
        Result<void> ValidateRecord(const PrefabComponentOverrideRecord &record) {
            if (record.valueless_by_exception())
                return Invalid<void>();
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record))
                return ValidateRawComponentPayload(add->payload);
            if (const auto &remove = std::get<PrefabRemoveComponentOverride>(record);
                !remove.target.type.IsValid() || !remove.target.instance.IsValid() || remove.componentSchemaVersion == 0)
                return Invalid<void>("Component removal requires exact stable type, occurrence and schema identities.");
            return Result<void>::Success();
        }

        /** @brief Bound standalone operation bytes separately from preserved metadata and aggregate wire storage. */
        Result<void> Charge(const PrefabComponentOverrideRecord &record, const Wire &encoded, const PrefabProjectPolicy &policy,
                            std::size_t &bytes) {
            if (const auto standalone = EncodeComponentRecord(record).dump(0).size(); standalone > policy.maximumOverrideValueBytes)
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
            if (record.valueless_by_exception())
                return Invalid<void>();
            if (const auto target = ComponentTarget(record); !targets.emplace(target.object, target.instance.Value()).second)
                return Result<void>::Failure(
                    MakeError(PrefabErrors::OverrideConflict, "Duplicate component intent cannot add/remove the same occurrence twice."));
            return Result<void>::Success();
        }

        /** @brief Same-layer property patches cannot target a component being added or removed by that layer. */
        Result<void> CheckValueTarget(const PrefabValueOverrideRecord &record, const std::set<Occurrence> &targets) {
            if (const auto *property = std::get_if<PrefabPropertyAddress>(&record.target);
                property && targets.contains({property->Object(), property->ComponentInstance().Value()}))
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

    /** @brief Rejects contradictory occurrence claims independently of record payload validity or arrival order. */
    Result<void> PreflightComponentClaims(const PrefabValueOverrideSet &set) {
        std::set<Occurrence> targets;
        for (const auto &record : set.componentRecords)
            if (auto claimed = InsertOccurrence(record, targets); claimed.HasError())
                return claimed;
        for (const auto &conflict : set.componentConflicts)
            if (auto claimed = InsertOccurrence(conflict.original, targets); claimed.HasError())
                return claimed;
        for (const auto &orphan : set.componentOrphans)
            if (auto claimed = InsertOccurrence(orphan.original, targets); claimed.HasError())
                return claimed;
        return CheckValueTargets(set, targets);
    }

    namespace {
        /** @brief Borrows every canonical ordering dimension without copying component type strings in sort comparisons. */
        struct ComponentKey final {
            const PrefabObjectAddress &object;
            std::string_view type;
            std::uint64_t instance;
        };

        /** @brief Projects both supported operations into their common stable occurrence key. */
        ComponentKey Key(const PrefabComponentOverrideRecord &record) {
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record))
                return {add->object, add->payload.component.typeId.Value(), add->payload.instance.Value()};
            const auto &remove = std::get<PrefabRemoveComponentOverride>(record);
            return {remove.target.object, remove.target.type.Value(), remove.target.instance.Value()};
        }

        /** @brief Uses the exact semantic object/component identity order shared with canonical encoding. */
        bool KeyLess(const PrefabComponentOverrideRecord &a, const PrefabComponentOverrideRecord &b) {
            const auto x = Key(a);
            const auto y = Key(b);
            if (const auto order = CompareObjectAddresses(x.object, y.object); order != 0)
                return std::is_lt(order);
            return std::tie(x.type, x.instance) < std::tie(y.type, y.instance);
        }

        /** @brief Orders bounded borrowed entries without copying or rewriting preserved payload data. */
        template <class Entry, class Record>
        std::vector<const Entry *> OrderedComponents(const std::vector<Entry> &entries, Record record) {
            std::vector<const Entry *> ordered;
            ordered.reserve(entries.size());
            for (const auto &entry : entries)
                ordered.push_back(&entry);
            std::ranges::sort(ordered, [&](const auto *a, const auto *b) {
                return KeyLess(record(*a), record(*b));
            });
            return ordered;
        }

        /** @brief Validates ordinary topology records in stable-target order. */
        Result<void> ValidateNormalComponents(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy, std::size_t &bytes,
                                              std::set<Occurrence> &targets) {
            for (const auto *record : OrderedComponents(set.componentRecords, [](const auto &entry) -> const auto & {
                return entry;
            })) {
                if (auto valid = Admit(*record, policy, targets); valid.HasError())
                    return valid;
                if (auto charged = Charge(*record, EncodeComponentRecord(*record), policy, bytes); charged.HasError())
                    return charged;
            }
            return Result<void>::Success();
        }

        /** @brief Validates retained topology conflicts without allowing source vector order to choose their failure. */
        Result<void> ValidateComponentConflicts(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy, std::size_t &bytes,
                                                std::set<Occurrence> &targets) {
            for (const auto *conflict : OrderedComponents(set.componentConflicts, [](const auto &entry) -> const auto & {
                return entry.original;
            })) {
                if (auto valid = Admit(conflict->original, policy, targets); valid.HasError())
                    return valid;
                if (!Detail::IsCanonicalProjectVersion(conflict->authoredAgainst.projectVersion) ||
                    !Detail::IsCanonicalProjectVersion(conflict->observedSource.projectVersion))
                    return Invalid<void>();
                if (auto charged = Charge(conflict->original, EncodeComponentConflict(*conflict), policy, bytes); charged.HasError())
                    return charged;
            }
            return Result<void>::Success();
        }

        /** @brief Preserves opaque orphan payloads while admitting their envelopes in canonical identity order. */
        Result<void> ValidateComponentOrphans(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy, std::size_t &bytes,
                                              std::set<Occurrence> &targets) {
            for (const auto *orphan : OrderedComponents(set.componentOrphans, [](const auto &entry) -> const auto & {
                return entry.original;
            })) {
                if (auto valid = Admit(orphan->original, policy, targets); valid.HasError())
                    return valid;
                if (!Detail::IsCanonicalProjectVersion(orphan->authoredAgainst.projectVersion) ||
                    orphan->reason > PrefabComponentOverrideIssue::SchemaChanged)
                    return Invalid<void>();
                if (auto charged = Charge(orphan->original, EncodeComponentOrphan(*orphan), policy, bytes); charged.HasError())
                    return charged;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Shares the same complete stable occurrence order with parsed canonical record storage. */
    bool ComponentRecordLess(const PrefabComponentOverrideRecord &left, const PrefabComponentOverrideRecord &right) {
        return KeyLess(left, right);
    }

    /** @brief Admits all normal and preserved topology intent with shared deterministic structural and byte rules. */
    Result<void> ValidateComponentRecords(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy, std::size_t &bytes) {
        std::set<Occurrence> targets;
        if (auto valid = ValidateNormalComponents(set, policy, bytes, targets); valid.HasError())
            return valid;
        if (auto valid = ValidateComponentConflicts(set, policy, bytes, targets); valid.HasError())
            return valid;
        if (auto valid = ValidateComponentOrphans(set, policy, bytes, targets); valid.HasError())
            return valid;
        return CheckValueTargets(set, targets);
    }
}  // namespace Horo::Prefab::ValueDetail
