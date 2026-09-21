#include "Horo/Prefab/PrefabDocument.h"

#include "Horo/Foundation/Utf8.h"
#include "PrefabDocumentSerializationInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace Horo::Prefab {
    namespace {
        using Detail::AddPayloadBytes;

        /** @brief Counts dynamic bytes held by a portable behavior field value. */
        [[nodiscard]] std::size_t DynamicValueBytes(const Gameplay::BehaviorFieldValue &value) noexcept {
            return std::visit([]<typename Value>(const Value &typed) -> std::size_t {
                if constexpr (std::is_same_v<Value, std::string>)
                    return typed.size();
                return 0;
            }, value);
        }

        /** @brief Checks finite numeric values in a portable behavior field. */
        [[nodiscard]] bool IsFiniteBehaviorValue(const Gameplay::BehaviorFieldValue &value) noexcept {
            return std::visit([]<typename Value>(const Value &typed) {
                if constexpr (std::is_same_v<Value, double>)
                    return std::isfinite(typed);
                if constexpr (std::is_same_v<Value, Math::Vec2> || std::is_same_v<Value, Math::Vec3> ||
                              std::is_same_v<Value, Math::Quaternion>)
                    return Math::IsFinite(typed);
                return true;
            }, value);
        }

        /** @brief Checks that one transform contains only finite values before matrix validation. */
        [[nodiscard]] bool IsFiniteTransform(const Math::Transform &transform) noexcept {
            return Math::IsFinite(transform.translation) && Math::IsFinite(transform.rotation) && Math::IsFinite(transform.scale);
        }

        /** @brief Validates one exact source revision without introducing a parallel schema counter. */
        [[nodiscard]] bool IsValidRevision(const PrefabSourceRevision &revision) {
            return Detail::IsCanonicalProjectVersion(revision.projectVersion);
        }

        /** @brief Checks whether an AssetId occurs in a validated unique dependency list. */
        [[nodiscard]] bool ContainsAsset(const std::vector<Assets::AssetId> &assets, const Assets::AssetId &asset) {
            return std::ranges::find(assets, asset) != assets.end();
        }

        [[nodiscard]] PrefabProviderStatus ToPrefabStatus(const Gameplay::ComponentInspectionStatus status) noexcept {
            using enum Gameplay::ComponentInspectionStatus;
            switch (status) {
                case Current:
                    return PrefabProviderStatus::Current;
                case MigrationRequired:
                    return PrefabProviderStatus::MigrationRequired;
                case MissingDescriptor:
                    return PrefabProviderStatus::Missing;
                case UnsupportedOlderSchema:
                case NewerSchema:
                    return PrefabProviderStatus::IncompatibleSchema;
            }
            return PrefabProviderStatus::IncompatibleSchema;
        }

        [[nodiscard]] PrefabProviderStatus ToPrefabStatus(const Gameplay::GameAssetInspectionStatus status) noexcept {
            using enum Gameplay::GameAssetInspectionStatus;
            switch (status) {
                case Current:
                    return PrefabProviderStatus::Current;
                case MissingDescriptor:
                    return PrefabProviderStatus::Missing;
                case OlderSchema:
                case NewerSchema:
                    return PrefabProviderStatus::IncompatibleSchema;
            }
            return PrefabProviderStatus::IncompatibleSchema;
        }

        [[nodiscard]] const Gameplay::BehaviorDescriptor *FindBehaviorDescriptor(
            const std::span<const Gameplay::BehaviorDescriptor> descriptors, const Gameplay::BehaviorTypeId &type) noexcept {
            const auto found = std::ranges::find(descriptors, type, &Gameplay::BehaviorDescriptor::typeId);
            return found == descriptors.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] bool BehaviorFieldsMatch(const Gameplay::BehaviorComponent &component,
                                               const Gameplay::BehaviorDescriptor &descriptor) noexcept {
            return std::ranges::all_of(component.fields, [&descriptor](const Gameplay::BehaviorField &field) {
                const auto found = std::ranges::find(descriptor.fields, field.name, &Gameplay::BehaviorFieldDescriptor::name);
                return found != descriptor.fields.end() && found->defaultValue.index() == field.value.index();
            });
        }

        [[nodiscard]] Result<void> ValidateBehaviorDescriptors(const std::span<const Gameplay::BehaviorDescriptor> descriptors) {
            std::unordered_set<std::string_view> types;
            types.reserve(descriptors.size());
            for (const Gameplay::BehaviorDescriptor &descriptor : descriptors) {
                if (!descriptor.typeId.IsValid() || descriptor.schemaVersion == 0 || descriptor.displayName.empty() ||
                    !types.emplace(descriptor.typeId.Value()).second)
                    return Result<void>::Failure(MakeError(PrefabErrors::DocumentInvalid));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] PrefabProviderStatus InspectBehavior(const Gameplay::BehaviorComponent &component,
                                                           const Gameplay::BehaviorDescriptor *descriptor,
                                                           std::span<const Gameplay::BehaviorComponent> siblings) noexcept {
            using enum PrefabProviderStatus;
            if (descriptor == nullptr)
                return Missing;
            if (const std::size_t occurrenceCount = std::ranges::count(siblings, component.typeId, &Gameplay::BehaviorComponent::typeId);
                component.schemaVersion != descriptor->schemaVersion || !BehaviorFieldsMatch(component, *descriptor) ||
                (!descriptor->allowMultiple && occurrenceCount > 1))
                return IncompatibleSchema;
            return Current;
        }

        [[nodiscard]] Result<void> InspectObjectProviders(const PrefabObjectNode &object, const Gameplay::ComponentRegistry &components,
                                                          const std::span<const Gameplay::BehaviorDescriptor> behaviors,
                                                          PrefabProviderInspection &result) {
            for (const RawComponentPayload &component : object.components) {
                auto inspected = components.Inspect(component.component);
                if (inspected.HasError())
                    return Result<void>::Failure(inspected.ErrorValue());
                result.components.emplace_back(object.localId, component.instance, component.component.typeId,
                                               ToPrefabStatus(inspected.Value().status));
            }
            for (const Gameplay::BehaviorComponent &behavior : object.behaviors) {
                result.behaviors.emplace_back(object.localId, behavior.instanceId, behavior.typeId,
                                              InspectBehavior(behavior, FindBehaviorDescriptor(behaviors, behavior.typeId),
                                                              object.behaviors));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateReferencedGameAssets(const PrefabDocumentData &document,
                                                                const Assets::AssetRegistrySnapshot &assets,
                                                                const std::span<const PrefabReferencedGameAsset> referencedGameAssets) {
            std::vector<Assets::AssetId> identities;
            identities.reserve(referencedGameAssets.size());
            for (const PrefabReferencedGameAsset &asset : referencedGameAssets) {
                if (const Assets::AssetRecord *record = assets.Find(asset.assetId);
                    !asset.assetId.IsValid() || record == nullptr || !record->type.Value().starts_with("game.") ||
                    !ContainsAsset(document.referencedAssets, asset.assetId) || ContainsAsset(identities, asset.assetId) ||
                    (asset.payload != nullptr && asset.payload->typeId.Value() != record->type.Value()))
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceInvalid));
                identities.emplace_back(asset.assetId);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] const PrefabReferencedGameAsset *FindReferencedGameAsset(const std::span<const PrefabReferencedGameAsset> assets,
                                                                               const Assets::AssetId assetId) noexcept {
            const auto found = std::ranges::find(assets, assetId, &PrefabReferencedGameAsset::assetId);
            return found == assets.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] Result<void> InspectGameAssetProviders(const PrefabDocumentData &document,
                                                             const Assets::AssetRegistrySnapshot &assets,
                                                             const Gameplay::GameAssetTypeRegistry &gameAssetTypes,
                                                             const std::span<const PrefabReferencedGameAsset> referencedGameAssets,
                                                             PrefabProviderInspection &result) {
            result.assets.reserve(document.referencedAssets.size());
            for (const Assets::AssetId assetId : document.referencedAssets) {
                const Assets::AssetRecord *record = assets.Find(assetId);
                if (record == nullptr) {
                    result.assets.emplace_back(assetId, std::nullopt, PrefabProviderStatus::Missing);
                    continue;
                }
                if (!record->type.Value().starts_with("game.")) {
                    result.assets.emplace_back(assetId, record->type, PrefabProviderStatus::Current);
                    continue;
                }
                const PrefabReferencedGameAsset *asset = FindReferencedGameAsset(referencedGameAssets, assetId);
                if (asset == nullptr || asset->payload == nullptr) {
                    result.assets.emplace_back(assetId, record->type, PrefabProviderStatus::Missing);
                    continue;
                }
                auto inspected = gameAssetTypes.Inspect(*asset->payload);
                if (inspected.HasError())
                    return Result<void>::Failure(inspected.ErrorValue());
                result.assets.emplace_back(assetId, record->type, ToPrefabStatus(inspected.Value().status));
            }
            return Result<void>::Success();
        }

        /** @brief Validates unique component occurrences and accounts for their dynamic bytes. */
        [[nodiscard]] Result<void> ValidateComponents(const std::vector<RawComponentPayload> &components, std::size_t &payloadBytes,
                                                      const std::size_t maximumPayloadBytes) {
            std::unordered_set<std::uint64_t> componentInstances;
            componentInstances.reserve(components.size());
            for (const RawComponentPayload &component : components) {
                if (const auto validation = ValidateRawComponentPayload(component); validation.HasError())
                    return validation;
                if (!componentInstances.emplace(component.instance.Value()).second)
                    return Result<void>::Failure(MakeError(PrefabErrors::DocumentInvalid));
                if (!AddPayloadBytes(payloadBytes, component.component.typeId.Value().size(), maximumPayloadBytes) ||
                    !AddPayloadBytes(payloadBytes, component.component.payload.size(), maximumPayloadBytes))
                    return Result<void>::Failure(MakeError(PrefabErrors::PayloadTooLarge));
            }
            return Result<void>::Success();
        }

        /** @brief Validates unique behavior occurrences and accounts for their dynamic bytes. */
        [[nodiscard]] Result<void> ValidateBehaviors(const std::vector<Gameplay::BehaviorComponent> &behaviors, std::size_t &payloadBytes,
                                                     const std::size_t maximumPayloadBytes) {
            std::unordered_set<std::uint64_t> behaviorInstances;
            behaviorInstances.reserve(behaviors.size());
            for (const Gameplay::BehaviorComponent &behavior : behaviors) {
                if (const auto validation = Gameplay::ValidateBehaviorComponent(behavior); validation.HasError())
                    return validation;
                if (!behaviorInstances.emplace(behavior.instanceId.value).second)
                    return Result<void>::Failure(MakeError(PrefabErrors::DocumentInvalid));
                if (!AddPayloadBytes(payloadBytes, behavior.typeId.Value().size(), maximumPayloadBytes))
                    return Result<void>::Failure(MakeError(PrefabErrors::PayloadTooLarge));
                for (const Gameplay::BehaviorField &field : behavior.fields) {
                    if (const auto *text = std::get_if<std::string>(&field.value); !IsValidUtf8ScalarSequence(field.name) ||
                                                                                   !IsFiniteBehaviorValue(field.value) ||
                                                                                   (text != nullptr && !IsValidUtf8ScalarSequence(*text)))
                        return Result<void>::Failure(MakeError(PrefabErrors::DocumentInvalid));
                    if (!AddPayloadBytes(payloadBytes, field.name.size(), maximumPayloadBytes) ||
                        !AddPayloadBytes(payloadBytes, DynamicValueBytes(field.value), maximumPayloadBytes))
                        return Result<void>::Failure(MakeError(PrefabErrors::PayloadTooLarge));
                }
            }
            return Result<void>::Success();
        }

        /** @brief Validates and accounts for one hierarchy object's portable data. */
        [[nodiscard]] Result<void> ValidateObject(const PrefabObjectNode &object, std::size_t &payloadBytes,
                                                  const PrefabProjectPolicy &limits) {
            if (object.name.size() > MaximumPrefabObjectNameBytes || !IsValidUtf8ScalarSequence(object.name) ||
                !IsFiniteTransform(object.localTransform) || !object.localTransform.TryToMatrix().HasValue())
                return Result<void>::Failure(MakeError(PrefabErrors::DocumentInvalid));
            if (object.components.size() > limits.maximumComponentsPerObject ||
                object.behaviors.size() > limits.maximumComponentsPerObject - object.components.size())
                return Result<void>::Failure(MakeError(PrefabErrors::ComponentCountExceeded));
            if (!AddPayloadBytes(payloadBytes, object.name.size(), limits.maximumSourcePayloadBytes))
                return Result<void>::Failure(MakeError(PrefabErrors::PayloadTooLarge));

            if (const auto componentValidation = ValidateComponents(object.components, payloadBytes, limits.maximumSourcePayloadBytes);
                componentValidation.HasError())
                return componentValidation;
            return ValidateBehaviors(object.behaviors, payloadBytes, limits.maximumSourcePayloadBytes);
        }

        /** @brief Registers one non-root node and computes its root-inclusive depth. */
        [[nodiscard]] Result<void> RegisterChild(const PrefabObjectNode &object, std::unordered_map<std::uint32_t, std::size_t> &depths,
                                                 const std::size_t maximumHierarchyDepth) {
            if (object.localId.IsRoot() || !object.parentLocalId)
                return Result<void>::Failure(MakeError(PrefabErrors::HierarchyInvalid));
            const auto parent = depths.find(object.parentLocalId->value);
            if (parent == depths.end() || !depths.emplace(object.localId.value, parent->second + 1).second)
                return Result<void>::Failure(MakeError(PrefabErrors::HierarchyInvalid));
            if (parent->second + 1 > maximumHierarchyDepth)
                return Result<void>::Failure(MakeError(PrefabErrors::HierarchyDepthExceeded));
            return Result<void>::Success();
        }

        using ObjectDepths = std::unordered_map<std::uint32_t, std::size_t>;

        /** @brief Validates root-first hierarchy ordering and returns its computed object depths. */
        [[nodiscard]] Result<ObjectDepths> ValidateHierarchy(const std::vector<PrefabObjectNode> &objects, std::size_t &payloadBytes,
                                                             const PrefabProjectPolicy &limits) {
            if (objects.empty() || !objects.front().localId.IsRoot() || objects.front().parentLocalId)
                return Result<ObjectDepths>::Failure(MakeError(PrefabErrors::HierarchyInvalid));

            ObjectDepths depths;
            depths.reserve(objects.size());
            depths.emplace(0, 1);
            for (std::size_t index = 0; index < objects.size(); ++index) {
                const PrefabObjectNode &object = objects[index];
                if (index != 0) {
                    if (const auto registration = RegisterChild(object, depths, limits.maximumHierarchyDepth); registration.HasError())
                        return Result<ObjectDepths>::Failure(registration.ErrorValue());
                }

                if (const auto objectValidation = ValidateObject(object, payloadBytes, limits); objectValidation.HasError())
                    return Result<ObjectDepths>::Failure(objectValidation.ErrorValue());
            }
            return Result<ObjectDepths>::Success(std::move(depths));
        }

        /** @brief Validates one exclusive variant source. */
        [[nodiscard]] Result<void> ValidateVariant(const PrefabDocumentData &candidate, const PrefabComposition &composition) {
            if (!candidate.objects.empty() || !composition.nestedPlacements.empty() || !composition.variantAuthoredAgainst ||
                composition.variantParent->Asset() == candidate.assetId || !IsValidRevision(*composition.variantAuthoredAgainst) ||
                !ContainsAsset(candidate.referencedAssets, composition.variantParent->Asset()))
                return Result<void>::Failure(MakeError(PrefabErrors::CompositionInvalid));
            return Result<void>::Success();
        }

        /** @brief Validates concrete nested placements and their occupied local slots. */
        [[nodiscard]] bool IsValidNestedPlacement(const PrefabDocumentData &candidate, const ObjectDepths &objectDepths,
                                                  const NestedPrefabPlacement &placement) {
            const LocalObjectId parent = placement.parentLocalId.value_or(LocalObjectId{});
            return !placement.placementLocalId.IsRoot() && objectDepths.contains(parent.value) && placement.sourcePrefab.IsValid() &&
                   placement.sourcePrefab.Asset() != candidate.assetId && IsValidRevision(placement.authoredAgainst) &&
                   IsFiniteTransform(placement.localRootTransform) && placement.localRootTransform.TryToMatrix().HasValue() &&
                   ContainsAsset(candidate.referencedAssets, placement.sourcePrefab.Asset());
        }

        /** @brief Validates concrete nested placements and their occupied local slots. */
        [[nodiscard]] Result<void> ValidateNestedPlacements(const PrefabDocumentData &candidate, const ObjectDepths &objectDepths,
                                                            const PrefabComposition &composition, const PrefabProjectPolicy &limits) {
            if (composition.nestedPlacements.size() > limits.maximumDirectNestedPlacements)
                return Result<void>::Failure(MakeError(PrefabErrors::NestedPlacementCountExceeded));
            std::unordered_set<std::uint32_t> placementIds;
            placementIds.reserve(composition.nestedPlacements.size());
            for (const NestedPrefabPlacement &placement : composition.nestedPlacements) {
                if (objectDepths.contains(placement.placementLocalId.value) ||
                    !placementIds.emplace(placement.placementLocalId.value).second ||
                    !IsValidNestedPlacement(candidate, objectDepths, placement))
                    return Result<void>::Failure(MakeError(PrefabErrors::CompositionInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Validates optional concrete nesting or exclusive variant composition. */
        [[nodiscard]] Result<void> ValidateComposition(const PrefabDocumentData &candidate, const ObjectDepths &objectDepths,
                                                       const PrefabProjectPolicy &limits) {
            if (!candidate.composition)
                return Result<void>::Success();

            const PrefabComposition &composition = *candidate.composition;
            if (composition.variantParent)
                return ValidateVariant(candidate, composition);
            if (composition.variantAuthoredAgainst || composition.nestedPlacements.empty())
                return Result<void>::Failure(MakeError(PrefabErrors::CompositionInvalid));
            return ValidateNestedPlacements(candidate, objectDepths, composition, limits);
        }

        /** @brief Validates a unique bounded dependency list and returns its initial payload byte count. */
        [[nodiscard]] Result<std::size_t> ValidateReferences(const std::vector<Assets::AssetId> &references,
                                                             const PrefabProjectPolicy &limits) {
            if (references.size() > limits.maximumReferencedAssets)
                return Result<std::size_t>::Failure(MakeError(PrefabErrors::ReferenceCountExceeded));

            std::vector<Assets::AssetId> uniqueAssets;
            uniqueAssets.reserve(references.size());
            std::size_t payloadBytes{};
            for (const Assets::AssetId &asset : references) {
                if (!asset.IsValid() || ContainsAsset(uniqueAssets, asset))
                    return Result<std::size_t>::Failure(MakeError(PrefabErrors::ReferenceInvalid));
                uniqueAssets.push_back(asset);
                if (!AddPayloadBytes(payloadBytes, asset.Bytes().size(), limits.maximumSourcePayloadBytes))
                    return Result<std::size_t>::Failure(MakeError(PrefabErrors::PayloadTooLarge));
            }
            return Result<std::size_t>::Success(payloadBytes);
        }
    }  // namespace

    /** @copydoc PrefabDocument::Create */
    Result<PrefabDocument> PrefabDocument::Create(PrefabDocumentData candidate, const PrefabLimitProfile &limits) {
        const PrefabProjectPolicy &policy = limits.Policy();
        if (!candidate.assetId.IsValid() || !Detail::IsCanonicalProjectVersion(candidate.projectVersion))
            return Result<PrefabDocument>::Failure(MakeError(PrefabErrors::DocumentInvalid));
        if (candidate.objects.size() > policy.maximumObjectCount)
            return Result<PrefabDocument>::Failure(MakeError(PrefabErrors::ObjectCountExceeded));
        const auto referenceValidation = ValidateReferences(candidate.referencedAssets, policy);
        if (referenceValidation.HasError())
            return Result<PrefabDocument>::Failure(referenceValidation.ErrorValue());
        std::size_t payloadBytes = referenceValidation.Value();

        ObjectDepths objectDepths;
        if (const bool isVariant = candidate.composition && candidate.composition->variantParent; !isVariant) {
            auto hierarchyValidation = ValidateHierarchy(candidate.objects, payloadBytes, policy);
            if (hierarchyValidation.HasError())
                return Result<PrefabDocument>::Failure(hierarchyValidation.ErrorValue());
            objectDepths = std::move(hierarchyValidation).Value();
        }

        if (const auto compositionValidation = ValidateComposition(candidate, objectDepths, policy); compositionValidation.HasError())
            return Result<PrefabDocument>::Failure(compositionValidation.ErrorValue());

        return Result<PrefabDocument>::Success(PrefabDocument{std::move(candidate)});
    }

    /** @copydoc PrefabDocument::Data */
    const PrefabDocumentData &PrefabDocument::Data() const noexcept {
        return data_;
    }

    /** @copydoc PrefabProviderInspection::IsDegraded */
    bool PrefabProviderInspection::IsDegraded() const noexcept {
        const auto degraded = [](const auto &entry) {
            return entry.status != PrefabProviderStatus::Current;
        };
        return std::ranges::any_of(components, degraded) || std::ranges::any_of(behaviors, degraded) ||
               std::ranges::any_of(assets, degraded);
    }

    /** @copydoc PrefabDocument::InspectProviders */
    Result<PrefabProviderInspection> PrefabDocument::InspectProviders(
        const Assets::AssetRegistrySnapshot &assets, const Gameplay::ComponentRegistry &components,
        const std::span<const Gameplay::BehaviorDescriptor> behaviors, const Gameplay::GameAssetTypeRegistry &gameAssetTypes,
        const std::span<const PrefabReferencedGameAsset> referencedGameAssets) const {
        if (const auto validDescriptors = ValidateBehaviorDescriptors(behaviors); validDescriptors.HasError())
            return Result<PrefabProviderInspection>::Failure(validDescriptors.ErrorValue());
        if (const auto validAssets = ValidateReferencedGameAssets(data_, assets, referencedGameAssets); validAssets.HasError())
            return Result<PrefabProviderInspection>::Failure(validAssets.ErrorValue());

        PrefabProviderInspection result;
        std::size_t componentCount{};
        std::size_t behaviorCount{};
        for (const PrefabObjectNode &object : data_.objects) {
            componentCount += object.components.size();
            behaviorCount += object.behaviors.size();
        }
        result.components.reserve(componentCount);
        result.behaviors.reserve(behaviorCount);
        for (const PrefabObjectNode &object : data_.objects) {
            if (const auto inspectedObject = InspectObjectProviders(object, components, behaviors, result); inspectedObject.HasError())
                return Result<PrefabProviderInspection>::Failure(inspectedObject.ErrorValue());
        }
        if (const auto inspectedAssets = InspectGameAssetProviders(data_, assets, gameAssetTypes, referencedGameAssets, result);
            inspectedAssets.HasError())
            return Result<PrefabProviderInspection>::Failure(inspectedAssets.ErrorValue());
        return Result<PrefabProviderInspection>::Success(std::move(result));
    }
}  // namespace Horo::Prefab
