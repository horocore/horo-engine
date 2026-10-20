#include "PrefabValueResolutionInternal.h"

#include <algorithm>
#include <new>
#include <type_traits>

namespace Horo::Prefab {
    namespace {
        using namespace ValueDetail;

        /** @brief Explicit commands coalesce only the selected occurrence, including preserved source evidence. */
        void EraseComponent(PrefabValueOverrideSet &set, const PrefabComponentOverrideTarget &target) {
            std::erase_if(set.componentRecords, [&target](const auto &record) {
                return ComponentTarget(record) == target;
            });
            std::erase_if(set.componentConflicts, [&target](const auto &record) {
                return ComponentTarget(record.original) == target;
            });
            std::erase_if(set.componentOrphans, [&target](const auto &record) {
                return ComponentTarget(record.original) == target;
            });
        }

        /** @brief Removing a selected component deliberately removes its property intents, without touching peers. */
        void EraseProperties(PrefabValueOverrideSet &set, const PrefabComponentOverrideTarget &target) {
            const auto matches = [&target](const PrefabValueOverrideRecord &record) {
                const auto *property = std::get_if<PrefabPropertyAddress>(&record.target);
                return property && property->Object() == target.object && property->ComponentType() == target.type &&
                       property->ComponentInstance() == target.instance;
            };
            std::erase_if(set.records, matches);
            std::erase_if(set.conflicts, [&matches](const auto &record) {
                return matches(record.original);
            });
            std::erase_if(set.orphans, [&matches](const auto &record) {
                return matches(record.original);
            });
        }

        /** @brief Capture source preconditions ourselves; removing an instance addition simply coalesces its add record. */
        Result<void> PrepareRemoval(PrefabValueOverrideSet &set, const EffectivePrefabCandidate &source,
                                    const PrefabComponentOverrideTarget &target) {
            const auto objects = source.Objects();
            const auto object = std::ranges::find_if(objects, [&target](const auto &entry) {
                return entry.key.object == target.object;
            });
            if (object == objects.end())
                return Result<void>::Failure(MakeError(PrefabErrors::OverrideOrphan));
            if (const auto component = std::ranges::find(object->object.components, target.instance, &RawComponentPayload::instance);
                component == object->object.components.end()) {
                if (const auto added = std::ranges::find_if(set.componentRecords,
                                                            [&target](const auto &record) {
                    return std::holds_alternative<PrefabAddComponentOverride>(record) && ComponentTarget(record) == target;
                });
                    added == set.componentRecords.end())
                    return Result<void>::Failure(MakeError(PrefabErrors::OverrideOrphan));
                EraseComponent(set, target);
            } else {
                if (component->component.typeId != target.type)
                    return Result<void>::Failure(MakeError(PrefabErrors::OverrideOrphan));
                EraseComponent(set, target);
                set.componentRecords.emplace_back(
                    PrefabRemoveComponentOverride{target, component->component.schemaVersion, ComponentDigest(*component)});
            }
            EraseProperties(set, target);
            return Result<void>::Success();
        }

        /** @brief Add identity cannot reinterpret any retained topology or property intent, even an unavailable one. */
        Result<void> PrepareAddition(PrefabValueOverrideSet &set, const PrefabAddComponentOverride &add) {
            const PrefabComponentOverrideTarget target{add.object, add.payload.component.typeId, add.payload.instance};
            const auto sameOccurrence = [&target](const PrefabComponentOverrideRecord &record) {
                const auto other = ComponentTarget(record);
                return other.object == target.object && other.instance == target.instance;
            };
            if (const auto existing = std::ranges::find_if(set.componentRecords, sameOccurrence); existing != set.componentRecords.end()) {
                if (!std::holds_alternative<PrefabAddComponentOverride>(*existing) || ComponentTarget(*existing) != target)
                    return Result<void>::Failure(MakeError(PrefabErrors::OverrideConflict));
                *existing = add;
            } else
                set.componentRecords.emplace_back(add);
            return Result<void>::Success();
        }

        /** @brief Coalescing the last add record must still validate the resulting object's dependencies. */
        Result<void> ValidateEditedObjects(const EffectivePrefabCandidate &candidate, const PrefabComponentEdit &edit,
                                           const PrefabValueSchemaSnapshot &schemas) {
            std::vector<PrefabObjectAddress> targets;
            std::visit([&targets]<typename Edit>(const Edit &value) {
                if constexpr (std::is_same_v<Edit, PrefabAddComponentOverride>)
                    targets.push_back(value.object);
                else if constexpr (std::is_same_v<Edit, PrefabRevertComponentOverridesEdit>) {
                    for (const auto &target : value.targets)
                        targets.push_back(target.object);
                } else
                    targets.push_back(value.target.object);
            }, edit);
            std::ranges::sort(targets);
            const auto duplicates = std::ranges::unique(targets);
            targets.erase(duplicates.begin(), duplicates.end());
            for (const auto &target : targets) {
                const auto object = std::ranges::find_if(candidate.Objects(), [&target](const auto &entry) {
                    return entry.key.object == target;
                });
                if (object != candidate.Objects().end())
                    if (auto valid = ValidateComponentObject(*object, schemas); valid.HasError())
                        return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Replacement may never overwrite another occurrence's retained addition or preserved evidence. */
        bool HasReplacementIdentity(const PrefabValueOverrideSet &set, const PrefabReplaceComponentEdit &edit) {
            const auto matches = [&edit](const PrefabComponentOverrideRecord &record) {
                const auto target = ComponentTarget(record);
                return target.object == edit.target.object && target.instance == edit.replacement.instance;
            };
            return std::ranges::any_of(set.componentRecords, matches) ||
                   std::ranges::any_of(set.componentConflicts, [&matches](const auto &record) {
                return matches(record.original);
            }) || std::ranges::any_of(set.componentOrphans, [&matches](const auto &record) {
                return matches(record.original);
            });
        }

        /** @brief Lower atomic commands into the persisted add/remove algebra without an intermediate publication. */
        Result<void> PrepareIntent(PrefabValueOverrideSet &set, const EffectivePrefabCandidate &source, const PrefabComponentEdit &edit) {
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(&edit))
                return PrepareAddition(set, *add);
            if (const auto *remove = std::get_if<PrefabRemoveComponentEdit>(&edit))
                return PrepareRemoval(set, source, remove->target);
            if (const auto *replace = std::get_if<PrefabReplaceComponentEdit>(&edit)) {
                if (replace->target.instance == replace->replacement.instance || HasReplacementIdentity(set, *replace))
                    return Invalid<void>("Replacement requires a fresh component occurrence identity.");
                if (auto removed = PrepareRemoval(set, source, replace->target); removed.HasError())
                    return removed;
                return PrepareAddition(set, {replace->target.object, replace->replacement});
            }
            for (const auto &target : std::get<PrefabRevertComponentOverridesEdit>(edit).targets)
                EraseComponent(set, target);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PreparePrefabComponentEdit */
    Result<PrefabValueOverrideResolution> PreparePrefabComponentEdit(
        const EffectivePrefabCandidate &source, const std::optional<PrefabValueOverrideSet> &current, const PrefabComponentEdit &edit,
        const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits, const CancellationToken &cancellation) {
        try {
            if (const auto *revert = std::get_if<PrefabRevertComponentOverridesEdit>(&edit);
                revert && revert->targets.size() > limits.Policy().maximumOverrideRecords)
                return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            if (edit.valueless_by_exception())
                return ValueDetail::Invalid<PrefabValueOverrideResolution>();
            const PrefabValueOverrideSet empty{source.RootAsset(),
                                               source.Revision().rootSource,
                                               schemas ? schemas->Revision() : Sha256Digest{},
                                               {},
                                               {},
                                               {}};
            const PrefabValueOverrideSet *initial = &empty;
            if (current)
                initial = &*current;
            auto inspected = ResolvePrefabValueOverrides(source, *initial, schemas, limits, cancellation);
            if (inspected.HasError())
                return inspected;
            auto next = std::move(inspected).Value().rebased;
            if (auto prepared = PrepareIntent(next, source, edit); prepared.HasError())
                return Result<PrefabValueOverrideResolution>::Failure(prepared.ErrorValue());
            auto resolved = ResolvePrefabValueOverrides(source, next, schemas, limits, cancellation);
            if (resolved.HasValue() && !resolved.Value().effective && !std::holds_alternative<PrefabRevertComponentOverridesEdit>(edit))
                return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::OverrideConflict));
            if (resolved.HasValue() && resolved.Value().effective && schemas)
                if (auto valid = ValidateEditedObjects(*resolved.Value().effective, edit, *schemas); valid.HasError())
                    return Result<PrefabValueOverrideResolution>::Failure(valid.ErrorValue());
            return resolved;
        } catch (const std::bad_alloc &) {
            return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Prefab
