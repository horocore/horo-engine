#include "PrefabValueOverridesInternal.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <set>

namespace Horo::Prefab {
    namespace {
        using namespace ValueDetail;

        /** @brief Schema order uses stable component/property identities rather than registration order. */
        auto Key(const PrefabValuePropertySchema &schema) noexcept {
            return std::pair{std::string_view(schema.componentType.Value()), schema.property.Value()};
        }

        /** @brief Capture every semantic validator/default field into the schema publication identity. */
        Wire EncodeSchema(const PrefabValuePropertySchema &schema) {
            return Wire{{"component", schema.componentType.Value()},
                        {"property", schema.property.Value()},
                        {"payloadProperty", schema.payloadProperty.Value()},
                        {"componentSchemaVersion", schema.componentSchemaVersion},
                        {"default", EncodeValue(schema.defaultValue)},
                        {"minimum", schema.minimum.has_value() ? Wire(*schema.minimum) : Wire(nullptr)},
                        {"maximum", schema.maximum.has_value() ? Wire(*schema.maximum) : Wire(nullptr)},
                        {"maximumStringBytes", schema.maximumStringBytes},
                        {"required", schema.required}};
        }

        /** @brief Validate inert schema metadata before any snapshot becomes visible. */
        Result<void> ValidateSchema(const PrefabValuePropertySchema &schema) {
            if (!schema.componentType.IsValid() || !schema.property.IsValid() || !schema.payloadProperty.IsValid() ||
                ((schema.kind == Gameplay::ComponentPropertyKind::Boolean || schema.kind == Gameplay::ComponentPropertyKind::String) &&
                 (schema.minimum.has_value() || schema.maximum.has_value())) ||
                schema.componentSchemaVersion == 0 || schema.maximumStringBytes > PrefabHardLimits::OverrideValueBytes ||
                (schema.minimum.has_value() && !std::isfinite(*schema.minimum)) ||
                (schema.maximum.has_value() && !std::isfinite(*schema.maximum)) ||
                (schema.minimum.has_value() && schema.maximum.has_value() && *schema.minimum > *schema.maximum))
                return Invalid<void>("Prefab property schema metadata is invalid.");
            return ValidateValue(schema.defaultValue, &schema);
        }

        /** @brief Validate bounded unique metadata, charging actual canonical bytes before the snapshot writer. */
        Result<Sha256Digest> ValidateSchemas(std::vector<PrefabValuePropertySchema> &schemas, const PrefabLimitProfile &limits) {
            if (schemas.size() > 4096)
                return Result<Sha256Digest>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            std::ranges::sort(schemas, {}, Key);
            std::set<std::pair<Gameplay::ComponentTypeId, Gameplay::ComponentPropertyId>> payloads;
            std::size_t bytes = 2;
            auto encoded = Wire::array();
            for (std::size_t i = 0; i < schemas.size(); ++i) {
                const auto &schema = schemas[i];
                if (auto valid = ValidateSchema(schema); valid.HasError())
                    return Result<Sha256Digest>::Failure(valid.ErrorValue());
                if ((i && Key(schemas[i - 1]) == Key(schema)) || !payloads.emplace(schema.componentType, schema.payloadProperty).second ||
                    (i && schemas[i - 1].componentType == schema.componentType &&
                     schemas[i - 1].componentSchemaVersion != schema.componentSchemaVersion))
                    return Invalid<Sha256Digest>("Prefab schema has duplicate identities, aliases, or competing component versions.");
                auto item = EncodeSchema(schema);
                const auto itemBytes = item.dump(0).size();
                if (bytes > limits.Policy().maximumOverrideSetBytes || itemBytes + 2 > limits.Policy().maximumOverrideSetBytes - bytes)
                    return Result<Sha256Digest>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                bytes += itemBytes + 2;
                encoded.push_back(std::move(item));
            }
            const auto canonical = encoded.dump(0);
            return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span(canonical.data(), canonical.size()))));
        }
    }  // namespace

    /** @copydoc PrefabValueSchemaSnapshot::PrefabValueSchemaSnapshot */
    PrefabValueSchemaSnapshot::PrefabValueSchemaSnapshot(std::vector<PrefabValuePropertySchema> properties, const Sha256Digest &revision,
                                                         std::vector<PrefabComponentOverridePolicy> components,
                                                         std::vector<PrefabBehaviorComponentPolicy> behaviors) noexcept
        : properties_(std::move(properties)), revision_(revision), components_(std::move(components)), behaviors_(std::move(behaviors)) {}

    /** @copydoc PrefabValueSchemaSnapshot::Create */
    Result<PrefabValueSchemaSnapshot> PrefabValueSchemaSnapshot::Create(std::vector<PrefabValuePropertySchema> properties,
                                                                        const PrefabLimitProfile &limits,
                                                                        std::vector<PrefabComponentOverridePolicy> components,
                                                                        std::vector<PrefabBehaviorComponentPolicy> behaviors) {
        try {
            auto revision = ValidateSchemas(properties, limits);
            if (revision.HasError())
                return Result<PrefabValueSchemaSnapshot>::Failure(revision.ErrorValue());
            auto complete = CaptureComponentSchemas(components, behaviors, properties, revision.Value(), limits);
            if (complete.HasError())
                return Result<PrefabValueSchemaSnapshot>::Failure(complete.ErrorValue());
            return Result<PrefabValueSchemaSnapshot>::Success(
                PrefabValueSchemaSnapshot{std::move(properties), complete.Value(), std::move(components), std::move(behaviors)});
        } catch (const std::bad_alloc &) {
            return Result<PrefabValueSchemaSnapshot>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PrefabValueSchemaSnapshot::Properties */
    std::span<const PrefabValuePropertySchema> PrefabValueSchemaSnapshot::Properties() const noexcept {
        return properties_;
    }

    /** @copydoc PrefabValueSchemaSnapshot::Revision */
    Sha256Digest PrefabValueSchemaSnapshot::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc PrefabValueSchemaSnapshot::Find */
    const PrefabValuePropertySchema *PrefabValueSchemaSnapshot::Find(const PrefabPropertyAddress &target) const noexcept {
        const auto key = std::pair{std::string_view(target.ComponentType().Value()), target.Property().Value()};
        const auto found = std::ranges::lower_bound(properties_, key, {}, Key);
        return found != properties_.end() && Key(*found) == key ? std::to_address(found) : nullptr;
    }

    /** @copydoc PrefabValueSchemaSnapshot::Components */
    std::span<const PrefabComponentOverridePolicy> PrefabValueSchemaSnapshot::Components() const noexcept {
        return components_;
    }

    /** @copydoc PrefabValueSchemaSnapshot::Behaviors */
    std::span<const PrefabBehaviorComponentPolicy> PrefabValueSchemaSnapshot::Behaviors() const noexcept {
        return behaviors_;
    }

    /** @copydoc PrefabValueSchemaSnapshot::FindComponent */
    const PrefabComponentOverridePolicy *PrefabValueSchemaSnapshot::FindComponent(const Gameplay::ComponentTypeId &type) const noexcept {
        const auto found = std::ranges::lower_bound(components_, type, {}, [](const auto &entry) -> const auto & {
            return entry.component.typeId;
        });
        return found != components_.end() && found->component.typeId == type ? std::to_address(found) : nullptr;
    }

    /** @copydoc PrefabValueSchemaSnapshot::FindBehavior */
    const PrefabBehaviorComponentPolicy *PrefabValueSchemaSnapshot::FindBehavior(const Gameplay::BehaviorTypeId &type) const noexcept {
        const auto found = std::ranges::lower_bound(behaviors_, type, {}, &PrefabBehaviorComponentPolicy::type);
        return found != behaviors_.end() && found->type == type ? std::to_address(found) : nullptr;
    }
}  // namespace Horo::Prefab
