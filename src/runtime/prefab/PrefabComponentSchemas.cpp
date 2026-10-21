#include "PrefabValueOverridesInternal.h"

#include <algorithm>

namespace Horo::Prefab::ValueDetail {
    namespace {
        /** @brief Bound and canonicalize declarative identity sets without looking up services or providers. */
        template <class Id> Result<void> CanonicalIds(std::vector<Id> &ids, const std::size_t maximum) {
            if (ids.size() > maximum)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            std::ranges::sort(ids);
            if (std::ranges::any_of(ids,
                                    [](const Id &id) {
                return !id.IsValid();
            }) ||
                std::ranges::adjacent_find(ids) != ids.end())
                return Invalid<void>("Component compatibility identities must be valid and unique.");
            return Result<void>::Success();
        }

        /** @brief Copy canonical stable identities into the schema digest; presentation names never participate. */
        template <class Id> Wire EncodeIds(const std::vector<Id> &ids) {
            auto encoded = Wire::array();
            for (const auto &id : ids)
                encoded.push_back(id.Value());
            return encoded;
        }

        /** @brief Charge one bounded schema before adding it to the aggregate canonical writer. */
        Result<void> Append(Wire &destination, Wire entry, std::size_t &bytes, const std::size_t maximum) {
            const auto size = entry.dump(0).size();
            if (bytes > maximum || size > maximum - bytes || maximum - bytes - size < 2)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            bytes += size + 2;
            destination.push_back(std::move(entry));
            return Result<void>::Success();
        }

        /** @brief Capture only payload-validation and topology semantics from an already validated descriptor. */
        Wire EncodeComponent(const PrefabComponentOverridePolicy &policy) {
            auto properties = Wire::array();
            for (const auto &property : policy.component.properties)
                properties.push_back(
                    Wire{{"id", property.id.Value()}, {"kind", static_cast<unsigned>(property.kind)}, {"required", property.required}});
            return Wire{{"type", policy.component.typeId.Value()},
                        {"schema", policy.component.schemaVersion},
                        {"properties", std::move(properties)},
                        {"maximumInstances", policy.maximumInstances},
                        {"allowAddition", policy.allowAddition},
                        {"allowRemoval", policy.allowRemoval},
                        {"requiredComponents", EncodeIds(policy.requiredComponents)},
                        {"incompatibleComponents", EncodeIds(policy.incompatibleComponents)},
                        {"incompatibleBehaviors", EncodeIds(policy.incompatibleBehaviors)}};
        }

        /** @brief Validate explicit prefab permissions using the existing Gameplay descriptor validator as the schema authority. */
        Result<void> ValidateComponent(PrefabComponentOverridePolicy &policy, Gameplay::ComponentRegistry &registry,
                                       const PrefabProjectPolicy &limits) {
            if (policy.maximumInstances == 0 || policy.maximumInstances > limits.maximumComponentsPerObject ||
                policy.component.properties.size() > 4096)
                return Invalid<void>("Component topology multiplicity or property bounds are invalid.");
            if (auto valid = CanonicalIds(policy.requiredComponents, limits.maximumComponentsPerObject); valid.HasError())
                return valid;
            if (auto valid = CanonicalIds(policy.incompatibleComponents, 4096); valid.HasError())
                return valid;
            if (auto valid = CanonicalIds(policy.incompatibleBehaviors, 4096); valid.HasError())
                return valid;
            if (std::ranges::binary_search(policy.requiredComponents, policy.component.typeId) ||
                std::ranges::binary_search(policy.incompatibleComponents, policy.component.typeId))
                return Invalid<void>("Component compatibility cannot require or exclude its own type.");
            if (std::ranges::any_of(policy.requiredComponents, [&policy](const auto &type) {
                return std::ranges::binary_search(policy.incompatibleComponents, type);
            }))
                return Invalid<void>("A component cannot require and exclude the same dependency.");
            std::ranges::sort(policy.component.properties, {}, &Gameplay::ComponentPropertyDescriptor::id);
            return registry.Register(policy.component);
        }

        /** @brief Ensure value and topology captures cannot disagree about a shared registered payload field. */
        Result<void> ValidatePropertyBindings(const std::span<const PrefabValuePropertySchema> properties,
                                              const Gameplay::ComponentRegistry &registry) {
            for (const auto &property : properties) {
                const auto *component = registry.Find(property.componentType);
                if (!component)
                    continue;
                const auto field =
                    std::ranges::find(component->properties, property.payloadProperty, &Gameplay::ComponentPropertyDescriptor::id);
                if (component->schemaVersion != property.componentSchemaVersion || field == component->properties.end() ||
                    field->kind != property.kind || field->required != property.required)
                    return Invalid<void>("Value and topology metadata disagree about one registered property.");
            }
            return Result<void>::Success();
        }

        /** @brief Admit declared dependencies only after the complete component registry is frozen. */
        Result<void> ValidateRequirements(const std::span<const PrefabComponentOverridePolicy> components,
                                          const Gameplay::ComponentRegistry &registry) {
            for (const auto &component : components)
                if (std::ranges::any_of(component.requiredComponents, [&registry](const auto &type) {
                    return !registry.Find(type);
                }))
                    return Invalid<void>("Required components must have captured schemas.");
            return Result<void>::Success();
        }

        /** @brief Validate canonical behavior requirements without importing scheduling, callbacks or component-name aliases. */
        Result<void> ValidateBehavior(PrefabBehaviorComponentPolicy &policy, const PrefabProjectPolicy &limits,
                                      const Gameplay::ComponentRegistry &registry) {
            if (!policy.type.IsValid() || policy.schemaVersion == 0)
                return Invalid<void>("Behavior compatibility requires exact type and schema identities.");
            if (auto valid = CanonicalIds(policy.requiredComponents, limits.maximumComponentsPerObject); valid.HasError())
                return valid;
            if (std::ranges::any_of(policy.requiredComponents, [&registry](const auto &type) {
                return !registry.Find(type);
            }))
                return Invalid<void>("Required behavior components must have captured schemas.");
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Validate bounded copied topology metadata and extend the existing property snapshot digest without ambient effects. */
    Result<Sha256Digest> CaptureComponentSchemas(std::vector<PrefabComponentOverridePolicy> &components,
                                                 std::vector<PrefabBehaviorComponentPolicy> &behaviors,
                                                 const std::span<const PrefabValuePropertySchema> properties,
                                                 const Sha256Digest &propertyRevision, const PrefabLimitProfile &limits) {
        if (components.empty() && behaviors.empty())
            return Result<Sha256Digest>::Success(propertyRevision);
        if (components.size() > 4096 || behaviors.size() > 4096)
            return Result<Sha256Digest>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
        std::ranges::sort(components, {}, [](const auto &entry) -> const auto & {
            return entry.component.typeId;
        });
        std::ranges::sort(behaviors, {}, &PrefabBehaviorComponentPolicy::type);
        Gameplay::ComponentRegistry registry;
        auto encodedComponents = Wire::array();
        auto encodedBehaviors = Wire::array();
        std::size_t bytes = 128;
        for (auto &component : components) {
            if (auto valid = ValidateComponent(component, registry, limits.Policy()); valid.HasError())
                return Result<Sha256Digest>::Failure(valid.ErrorValue());
            if (auto added = Append(encodedComponents, EncodeComponent(component), bytes, limits.Policy().maximumOverrideSetBytes);
                added.HasError())
                return Result<Sha256Digest>::Failure(added.ErrorValue());
        }
        if (auto frozen = registry.Freeze(); frozen.HasError())
            return Result<Sha256Digest>::Failure(frozen.ErrorValue());
        if (auto valid = ValidateRequirements(components, registry); valid.HasError())
            return Result<Sha256Digest>::Failure(valid.ErrorValue());
        if (auto valid = ValidatePropertyBindings(properties, registry); valid.HasError())
            return Result<Sha256Digest>::Failure(valid.ErrorValue());
        for (std::size_t i = 0; i < behaviors.size(); ++i) {
            auto &behavior = behaviors[i];
            if (i != 0 && behaviors[i - 1].type == behavior.type)
                return Invalid<Sha256Digest>("Behavior compatibility identities must be unique.");
            if (auto valid = ValidateBehavior(behavior, limits.Policy(), registry); valid.HasError())
                return Result<Sha256Digest>::Failure(valid.ErrorValue());
            auto entry = Wire{{"type", behavior.type.Value()},
                              {"schema", behavior.schemaVersion},
                              {"requiredComponents", EncodeIds(behavior.requiredComponents)}};
            if (auto added = Append(encodedBehaviors, std::move(entry), bytes, limits.Policy().maximumOverrideSetBytes); added.HasError())
                return Result<Sha256Digest>::Failure(added.ErrorValue());
        }
        const auto canonical = Wire{{"properties", FormatSha256(propertyRevision)},
                                    {"components", std::move(encodedComponents)},
                                    {"behaviors", std::move(encodedBehaviors)}}
                                   .dump(0);
        if (canonical.size() > limits.Policy().maximumOverrideSetBytes)
            return Result<Sha256Digest>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
        return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span(canonical.data(), canonical.size()))));
    }
}  // namespace Horo::Prefab::ValueDetail
