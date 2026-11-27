#include "PrefabValueResolutionInternal.h"

#include <algorithm>
#include <map>
#include <new>
#include <set>
#include <type_traits>

namespace Horo::Prefab {
    namespace {
        using namespace ValueDetail;

        /** @brief Explicit commands coalesce only the selected occurrence, including
         * preserved source evidence. */
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

        /** @brief Removing a selected component deliberately removes its property
         * intents, without touching peers. */
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

        /** @brief Capture source preconditions ourselves; removing an instance addition
         * simply coalesces its add record. */
        Result<void> PrepareRemoval(PrefabValueOverrideSet &set, const PrefabOverrideBaseline &source,
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

        /** @brief Add identity cannot reinterpret any retained topology or property
         * intent, even an unavailable one. */
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

        /** @brief Checks dependencies even when coalescing removed the last topology record. */
        Result<void> ValidateTargetObjects(std::span<const ResolvedPrefabObject> objects, const std::set<PrefabObjectAddress> &targets,
                                           const PrefabValueSchemaSnapshot &schemas) {
            for (const auto &object : objects)
                if (targets.contains(object.key.object))
                    if (auto valid = ValidateComponentObject(object, schemas); valid.HasError())
                        return valid;
            return Result<void>::Success();
        }

        /** @brief Coalescing the last add record must still validate the resulting
         * object's dependencies. */
        Result<void> ValidateEditedObjects(const std::span<const ResolvedPrefabObject> objects, const PrefabComponentEdit &edit,
                                           const PrefabValueSchemaSnapshot &schemas) {
            std::set<PrefabObjectAddress> targets;
            std::visit([&targets]<typename Edit>(const Edit &value) {
                if constexpr (std::is_same_v<Edit, PrefabAddComponentOverride>)
                    targets.insert(value.object);
                else if constexpr (std::is_same_v<Edit, PrefabRevertComponentOverridesEdit>) {
                    for (const auto &target : value.targets)
                        targets.insert(target.object);
                } else
                    targets.insert(value.target.object);
            }, edit);
            return ValidateTargetObjects(objects, targets, schemas);
        }

        /** @brief Checks the selection against the complete layer currently inherited by the Scene. */
        Result<void> ValidatePromotionSelection(const ResolvedPrefabObject &object, const PrefabComponentOverrideTarget &target,
                                                const PrefabComponentOverrideRecord &record, const PrefabValueSchemaSnapshot &schemas) {
            const auto *policy = schemas.FindComponent(target.type);
            if (!policy)
                return Result<void>::Failure(MakeError(PrefabErrors::OverrideOrphan));
            const auto &components = object.object.components;
            const auto existing = std::ranges::find(components, target.instance, &RawComponentPayload::instance);
            if (std::holds_alternative<PrefabAddComponentOverride>(record)) {
                if (existing != components.end() || !policy->allowAddition)
                    return Result<void>::Failure(MakeError(PrefabErrors::OverrideConflict));
                return Result<void>::Success();
            }
            const auto &remove = std::get<PrefabRemoveComponentOverride>(record);
            if (existing == components.end() || existing->component.typeId != target.type)
                return Result<void>::Failure(MakeError(PrefabErrors::OverrideOrphan));
            if (!policy->allowRemoval || existing->component.schemaVersion != remove.componentSchemaVersion ||
                ComponentDigest(*existing) != remove.expectedSourceComponent)
                return Result<void>::Failure(MakeError(PrefabErrors::OverrideConflict));
            return Result<void>::Success();
        }

        /** @brief Replacement may never overwrite another occurrence's retained
         * addition or preserved evidence. */
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

        /** @brief Lower atomic commands into the persisted add/remove algebra without
         * an intermediate publication. */
        Result<void> PrepareIntent(PrefabValueOverrideSet &set, const PrefabOverrideBaseline &source, const PrefabComponentEdit &edit) {
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

    namespace {
        /** @brief Final edited-object validation preserves the same complete resolution and exact conflict behavior. */
        Result<PrefabOverrideResolution> ValidateResolvedComponentEdit(Result<PrefabOverrideResolution> resolved,
                                                                       const PrefabComponentEdit &edit,
                                                                       const PrefabValueSchemaSnapshot *schemas) {
            if (resolved.HasValue() && !resolved.Value().effective && !std::holds_alternative<PrefabRevertComponentOverridesEdit>(edit))
                return Result<PrefabOverrideResolution>::Failure(MakeError(PrefabErrors::OverrideConflict));
            if (resolved.HasValue() && resolved.Value().effective && schemas)
                if (auto valid = ValidateEditedObjects(*resolved.Value().effective, edit, *schemas); valid.HasError())
                    return Result<PrefabOverrideResolution>::Failure(valid.ErrorValue());
            return resolved;
        }
    }  // namespace

    /** @copydoc PreparePrefabComponentEdit */
    Result<PrefabOverrideResolution> PreparePrefabComponentEdit(const PrefabOverrideBaseline &source,
                                                                const std::optional<PrefabValueOverrideSet> &current,
                                                                const PrefabComponentEdit &edit, const PrefabValueSchemaSnapshot *schemas,
                                                                const PrefabLimitProfile &limits, const CancellationToken &cancellation) {
        try {
            if (const auto *revert = std::get_if<PrefabRevertComponentOverridesEdit>(&edit);
                revert && revert->targets.size() > limits.Policy().maximumOverrideRecords)
                return Result<PrefabOverrideResolution>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            if (edit.valueless_by_exception())
                return ValueDetail::Invalid<PrefabOverrideResolution>();
            const PrefabValueOverrideSet empty{source.RootAsset(),
                                               source.SourceRevision(),
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
                return Result<PrefabOverrideResolution>::Failure(prepared.ErrorValue());
            return ValidateResolvedComponentEdit(ResolvePrefabValueOverrides(source, next, schemas, limits, cancellation), edit, schemas);
        } catch (const std::bad_alloc &) {
            return Result<PrefabOverrideResolution>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PreparePrefabComponentPromotion */
    Result<PrefabOverrideResolution> PreparePrefabComponentPromotion(const PrefabOverrideBaseline &source,
                                                                     const std::optional<PrefabValueOverrideSet> &current,
                                                                     std::span<const PrefabComponentOverrideRecord> selected,
                                                                     const PrefabValueSchemaSnapshot *schemas,
                                                                     const PrefabLimitProfile &limits,
                                                                     const CancellationToken &cancellation) {
        using Return = Result<PrefabOverrideResolution>;
        try {
            if (!schemas)
                return Return::Failure(MakeError(PrefabErrors::OverrideOrphan));
            if (selected.empty())
                return Return::Failure(MakeError(PrefabErrors::OverrideInvalid));
            const auto &policy = limits.Policy();
            if (selected.size() > policy.maximumOverrideRecords)
                return Return::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            const PrefabValueOverrideSet empty{source.RootAsset(), source.SourceRevision(), schemas->Revision()};
            auto inspected = ResolvePrefabValueOverrides(source, current ? *current : empty, schemas, limits, cancellation);
            if (inspected.HasError())
                return inspected;
            if (!inspected.Value().effective)
                return Return::Failure(MakeError(PrefabErrors::OverrideConflict));

            auto layer = std::move(inspected).Value();
            auto next = std::move(layer.rebased);
            std::map<PrefabObjectAddress, const ResolvedPrefabObject *> effective;
            for (const auto &object : *layer.effective)
                effective.emplace(object.key.object, &object);
            std::set<std::pair<PrefabObjectAddress, PrefabComponentInstanceId>> occurrences;
            std::set<PrefabObjectAddress> objects;
            PrefabExpansionBudget work{limits};
            std::size_t payloadBytes{};
            for (const auto &record : selected) {
                if (cancellation.IsCancellationRequested())
                    return Return::Failure(MakeError(PrefabErrors::Cancelled));
                if (record.valueless_by_exception())
                    return Return::Failure(MakeError(PrefabErrors::OverrideInvalid));
                const auto target = ValueDetail::ComponentTarget(record);
                if (!occurrences.emplace(target.object, target.instance).second)
                    return Return::Failure(MakeError(PrefabErrors::OverrideConflict));
                const auto found = effective.find(target.object);
                if (found == effective.end())
                    return Return::Failure(MakeError(PrefabErrors::OverrideOrphan));
                if (auto valid = ValidatePromotionSelection(*found->second, target, record, *schemas); valid.HasError())
                    return Return::Failure(std::move(valid).ErrorValue());
                objects.insert(target.object);
                if (auto charged = work.Consume(source.Objects().size()); charged.HasError())
                    return Return::Failure(std::move(charged).ErrorValue());
                if (auto charged = work.Consume(next.componentRecords.size() + next.records.size()); charged.HasError())
                    return Return::Failure(std::move(charged).ErrorValue());
                const auto *add = std::get_if<PrefabAddComponentOverride>(&record);
                if (add) {
                    const auto bytes = add->payload.component.payload.size();
                    if (bytes > policy.maximumOverrideValueBytes || bytes > policy.maximumOverrideSetBytes - payloadBytes)
                        return Return::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                    if (auto valid = ValidateRawComponentPayload(add->payload); valid.HasError())
                        return Return::Failure(std::move(valid).ErrorValue());
                    payloadBytes += bytes;
                }
                auto lowered = add ? PrepareAddition(next, *add) : PrepareRemoval(next, source, target);
                if (lowered.HasError())
                    return Return::Failure(std::move(lowered).ErrorValue());
            }

            auto resolved = ResolvePrefabValueOverrides(source, next, schemas, limits, cancellation);
            if (resolved.HasError())
                return resolved;
            if (!resolved.Value().effective)
                return Return::Failure(MakeError(PrefabErrors::OverrideConflict));
            if (auto valid = ValidateTargetObjects(*resolved.Value().effective, objects, *schemas); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            return resolved;
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Prefab
