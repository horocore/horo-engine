#include "PrefabValueResolutionInternal.h"

#include <algorithm>
#include <memory>
#include <type_traits>

namespace Horo::Prefab::ValueDetail {
    namespace {
        /** @brief Borrow original authored evidence until this synchronous operation completes. */
        struct ComponentIntent {
            const PrefabComponentOverrideRecord *record;
            const PrefabSourceRevision *source;
            Sha256Digest schema;
        };

        /** @brief Never resolve an occurrence by type alone or by an unstable vector position. */
        const RawComponentPayload *FindComponent(const ResolvedPrefabObject &object, const PrefabComponentOverrideTarget &target) {
            const auto found = std::ranges::find(object.object.components, target.instance, &RawComponentPayload::instance);
            return found != object.object.components.end() && found->component.typeId == target.type ? std::to_address(found) : nullptr;
        }

        /** @brief Validate known fields without normalizing or discarding provider-owned opaque bytes. */
        Result<void> ValidatePayload(const RawComponentPayload &payload, const PrefabComponentOverridePolicy &policy) {
            Detail::JsonParseGuard guard{PrefabHardLimits::SourceJsonDepth};
            const auto &bytes = payload.component.payload;
            const auto json = Json::parse(bytes.begin(), bytes.end(), std::ref(guard), false);
            if (json.is_discarded() || !json.is_object() || guard.HasDuplicate() || guard.IsTooDeep())
                return Invalid<void>("Component topology payload must be a bounded object with unique keys.");
            for (const auto &property : policy.component.properties) {
                const auto found = json.find(property.id.Value());
                if (found == json.end()) {
                    if (property.required)
                        return Invalid<void>("Required component payload property is absent.");
                    continue;
                }
                auto value = ReadPayloadValue(*found, property.kind);
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                if (auto valid = ValidateValue(value.Value()); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Missing metadata cannot silently grant component or behavior compatibility. */
        std::optional<PrefabComponentOverrideIssue> SchemaIssue(const ResolutionState &state, const ResolvedPrefabObject &object,
                                                                const ComponentIntent &intent,
                                                                const PrefabComponentOverrideTarget &target) {
            using enum PrefabComponentOverrideIssue;
            if (!state.schemas || !state.schemas->FindComponent(target.type))
                return SchemaUnavailable;
            if (intent.schema != state.schemas->Revision())
                return SchemaChanged;
            for (const auto &component : object.object.components) {
                const auto *policy = state.schemas->FindComponent(component.component.typeId);
                if (!policy)
                    return SchemaUnavailable;
                if (policy->component.schemaVersion != component.component.schemaVersion)
                    return SchemaChanged;
            }
            for (const auto &behavior : object.object.behaviors) {
                const auto *policy = state.schemas->FindBehavior(behavior.typeId);
                if (!policy)
                    return SchemaUnavailable;
                if (policy->schemaVersion != behavior.schemaVersion)
                    return SchemaChanged;
            }
            if (const auto version = std::visit(
                    []<typename Record>(const Record &record) {
                if constexpr (std::is_same_v<Record, PrefabAddComponentOverride>)
                    return record.payload.component.schemaVersion;
                else
                    return record.componentSchemaVersion;
            }, *intent.record);
                state.schemas->FindComponent(target.type)->component.schemaVersion != version)
                return SchemaChanged;
            return std::nullopt;
        }

        /** @brief Admit against the immutable source; staged peers never change a retained precondition. */
        Result<void> InspectIntent(const ResolutionState &state, const ComponentIntent &intent, PrefabValueOverrideResolution &result) {
            const auto target = ComponentTarget(*intent.record);
            const auto objects = state.source.Objects();
            const auto object = std::ranges::find_if(objects, [&target](const auto &entry) {
                return entry.key.object == target.object;
            });
            const auto orphan = [&](const PrefabComponentOverrideIssue reason) {
                result.rebased.componentOrphans.emplace_back(*intent.record, *intent.source, intent.schema, reason);
            };
            if (object == objects.end()) {
                orphan(PrefabComponentOverrideIssue::TargetRemoved);
                return Result<void>::Success();
            }
            if (std::holds_alternative<PrefabRemoveComponentOverride>(*intent.record) && !FindComponent(*object, target)) {
                orphan(PrefabComponentOverrideIssue::TargetRemoved);
                return Result<void>::Success();
            }
            if (const auto issue = SchemaIssue(state, *object, intent, target)) {
                orphan(*issue);
                return Result<void>::Success();
            }
            const auto *policy = state.schemas->FindComponent(target.type);
            const auto existing = std::ranges::find(object->object.components, target.instance, &RawComponentPayload::instance);
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(intent.record)) {
                if (!policy->allowAddition)
                    return Invalid<void>("Component contract forbids instance addition.");
                if (existing != object->object.components.end()) {
                    result.rebased.componentConflicts.emplace_back(*intent.record, *intent.source, intent.schema,
                                                                   ComponentDigest(*existing), state.source.Revision().rootSource);
                    return Result<void>::Success();
                }
                if (auto valid = ValidatePayload(add->payload, *policy); valid.HasError())
                    return valid;
            } else {
                if (!policy->allowRemoval)
                    return Invalid<void>("Component contract forbids instance removal.");
                const auto *component = FindComponent(*object, target);
                const auto digest = ComponentDigest(*component);
                if (digest != std::get<PrefabRemoveComponentOverride>(*intent.record).expectedSourceComponent) {
                    result.rebased.componentConflicts.emplace_back(*intent.record, *intent.source, intent.schema, digest,
                                                                   state.source.Revision().rootSource);
                    return Result<void>::Success();
                }
            }
            result.rebased.componentRecords.push_back(*intent.record);
            return Result<void>::Success();
        }

        /** @brief Validate final presence constraints after every removal and addition, including replacement at capacity. */
        Result<void> ValidateObject(const ResolvedPrefabObject &object, const PrefabValueSchemaSnapshot &schemas) {
            const auto &components = object.object.components;
            const auto present = [&components](const auto &type) {
                return std::ranges::any_of(components, [&type](const auto &component) {
                    return component.component.typeId == type;
                });
            };
            for (const auto &component : components) {
                const auto *policy = schemas.FindComponent(component.component.typeId);
                if (!policy || std::ranges::count_if(components, [&](const auto &peer) {
                    return peer.component.typeId == component.component.typeId;
                }) > policy->maximumInstances)
                    return Invalid<void>("Component multiplicity or schema admission failed.");
                if (!std::ranges::all_of(policy->requiredComponents, present) ||
                    std::ranges::any_of(policy->incompatibleComponents, present))
                    return Invalid<void>("Component dependencies or exclusions are violated.");
                if (auto valid = ValidatePayload(component, *policy); valid.HasError())
                    return valid;
                for (const auto &behavior : object.object.behaviors)
                    if (std::ranges::find(policy->incompatibleBehaviors, behavior.typeId) != policy->incompatibleBehaviors.end())
                        return Invalid<void>("Component is incompatible with an attached behavior.");
            }
            for (const auto &behavior : object.object.behaviors) {
                const auto *policy = schemas.FindBehavior(behavior.typeId);
                if (!policy || !std::ranges::all_of(policy->requiredComponents, present))
                    return Invalid<void>("Behavior component requirements are violated.");
            }
            return Result<void>::Success();
        }

        /** @brief Apply admitted topology only after value payload indices have been finalized. */
        Result<void> ApplyRecords(ResolutionState &state, const PrefabValueOverrideSet &set) {
            std::set<PrefabObjectAddress> edited;
            for (const auto &record : set.componentRecords) {
                const auto target = ComponentTarget(record);
                edited.insert(target.object);
                auto object = std::ranges::find_if(state.objects, [&target](const auto &entry) {
                    return entry.key.object == target.object;
                });
                if (std::holds_alternative<PrefabRemoveComponentOverride>(record))
                    std::erase_if(object->object.components, [&target](const auto &component) {
                        return component.instance == target.instance;
                    });
            }
            for (const auto &record : set.componentRecords)
                if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record)) {
                    auto object = std::ranges::find_if(state.objects, [add](const auto &entry) {
                        return entry.key.object == add->object;
                    });
                    object->object.components.push_back(add->payload);
                }
            std::size_t bytes{};
            for (auto &object : state.objects) {
                if (object.object.components.size() > state.limits.Policy().maximumComponentsPerObject)
                    return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                for (const auto &component : object.object.components) {
                    if (component.component.payload.size() > state.limits.Policy().maximumExpandedPayloadBytes - bytes)
                        return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                    bytes += component.component.payload.size();
                }
                if (edited.contains(object.key.object)) {
                    if (auto valid = ValidateObject(object, *state.schemas); valid.HasError())
                        return valid;
                    std::ranges::sort(object.object.components, {}, &RawComponentPayload::instance);
                }
            }
            for (auto &binding : state.bindings) {
                const auto *property = std::get_if<PrefabPropertyAddress>(&binding.provenance.target);
                if (!property)
                    continue;
                const auto &components = state.objects[binding.object].object.components;
                if (auto charged = state.budget.Consume(components.size()); charged.HasError())
                    return charged;
                if (std::ranges::none_of(components, [property](const auto &component) {
                    return component.instance == property->ComponentInstance() && component.component.typeId == property->ComponentType();
                })) {
                    binding.provenance.disposition = PrefabValueDisposition::RemovedByInstance;
                    binding.provenance.origin = PrefabValueOrigin::Instance;
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Re-admit an explicitly edited object even when coalescing removed its last persisted record. */
    Result<void> ValidateComponentObject(const ResolvedPrefabObject &object, const PrefabValueSchemaSnapshot &schemas) {
        return ValidateObject(object, schemas);
    }

    /** @brief Preserve all retained topology evidence before publishing one completely validated detached candidate. */
    Result<void> ResolveComponentIntents(ResolutionState &state, const PrefabValueOverrideSet &set, PrefabValueOverrideResolution &result) {
        std::vector<ComponentIntent> intents;
        for (const auto &record : set.componentRecords)
            intents.emplace_back(&record, &set.authoredAgainst, set.schemaRevision);
        for (const auto &conflict : set.componentConflicts)
            intents.emplace_back(&conflict.original, &conflict.authoredAgainst, conflict.schemaRevision);
        for (const auto &orphan : set.componentOrphans)
            intents.emplace_back(&orphan.original, &orphan.authoredAgainst, orphan.schemaRevision);
        std::ranges::sort(intents, [](const auto &a, const auto &b) {
            return ComponentTarget(*a.record) < ComponentTarget(*b.record);
        });
        for (const auto &intent : intents) {
            if (state.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
            if (auto charged = state.budget.Consume(1); charged.HasError())
                return charged;
            if (auto inspected = InspectIntent(state, intent, result); inspected.HasError())
                return inspected;
        }
        if (result.rebased.HasUnresolved() || intents.empty())
            return Result<void>::Success();
        return ApplyRecords(state, result.rebased);
    }
}  // namespace Horo::Prefab::ValueDetail
