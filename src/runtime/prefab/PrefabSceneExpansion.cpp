#include "Horo/Prefab/PrefabSceneExpansion.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo::Prefab {
    namespace {
        /** @brief Charges dynamic bytes before copying any payload into the detached result. */
        [[nodiscard]] Result<void> ChargeBytes(std::size_t &used, const std::size_t bytes, const std::size_t maximum) {
            if (bytes > maximum - used)
                return Result<void>::Failure(MakeError(PrefabErrors::PayloadTooLarge));
            used += bytes;
            return Result<void>::Success();
        }

        /** @brief Counts every typed component occurrence without interpreting payload semantics. */
        [[nodiscard]] std::size_t ComponentCount(const Runtime::RuntimeComponentSet &components) noexcept {
            const std::array present{components.camera.has_value(),           components.light.has_value(),
                                     components.audioSource.has_value(),      components.audioListener.has_value(),
                                     components.uiCanvas.has_value(),         components.navigationSurface.has_value(),
                                     components.navigationRegion.has_value(), components.navigationModifier.has_value(),
                                     components.navigationLink.has_value(),   components.navigationAgent.has_value(),
                                     components.aiAgent.has_value(),          components.aiController.has_value(),
                                     components.rigidBody.has_value()};
            return static_cast<std::size_t>(std::ranges::count(present, true)) + components.colliders.size() +
                   components.physicsConstraints.size() + components.behaviors.size() + components.gameplayComponents.size();
        }

        /** @brief Bounds the dynamic name and optional text value of each behavior field. */
        [[nodiscard]] Result<void> ChargeBehaviorFields(const std::span<const Gameplay::BehaviorField> fields, std::size_t &bytes,
                                                        const std::size_t maximum) {
            for (const auto &field : fields) {
                if (auto charge = ChargeBytes(bytes, field.name.size(), maximum); charge.HasError())
                    return charge;
                if (const auto *text = std::get_if<std::string>(&field.value)) {
                    if (auto charge = ChargeBytes(bytes, text->size(), maximum); charge.HasError())
                        return charge;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Accounts for all behavior-owned text, including variable field values. */
        [[nodiscard]] Result<void> ChargeBehaviors(const std::span<const Gameplay::BehaviorComponent> behaviors, std::size_t &bytes,
                                                   const std::size_t maximum, PrefabExpansionBudget &budget) {
            for (const auto &behavior : behaviors) {
                if (behavior.fields.size() > Gameplay::MaximumBehaviorFields)
                    return Result<void>::Failure(MakeError(PrefabErrors::DocumentInvalid));
                if (auto work = budget.Consume(behavior.fields.size()); work.HasError())
                    return work;
                if (auto charge = ChargeBytes(bytes, behavior.typeId.Value().size(), maximum); charge.HasError())
                    return charge;
                if (auto charge = ChargeBehaviorFields(behavior.fields, bytes, maximum); charge.HasError())
                    return charge;
            }
            return Result<void>::Success();
        }

        /** @brief Charges navigation profile arrays before their typed values are copied. */
        [[nodiscard]] Result<void> ChargeNavigationProfiles(const Runtime::RuntimeComponentSet &components, std::size_t &bytes,
                                                            const std::size_t maximum) {
            const std::size_t profiles = (components.navigationSurface ? components.navigationSurface->profiles.size() : 0) +
                                         (components.navigationLink ? components.navigationLink->profiles.size() : 0);
            if (profiles > maximum / sizeof(Navigation::NavigationAgentProfileId))
                return Result<void>::Failure(MakeError(PrefabErrors::PayloadTooLarge));
            if (auto charge = ChargeBytes(bytes, profiles * sizeof(Navigation::NavigationAgentProfileId), maximum); charge.HasError())
                return charge;
            return Result<void>::Success();
        }

        /** @brief Bounds typed dynamic storage independently from the source encoding. */
        [[nodiscard]] Result<void> ChargeComponents(const Runtime::RuntimeComponentSet &components, std::size_t &bytes,
                                                    const std::size_t maximum, PrefabExpansionBudget &budget) {
            for (const auto &component : components.gameplayComponents) {
                if (auto charge = ChargeBytes(bytes, component.typeId.Value().size(), maximum); charge.HasError())
                    return charge;
                if (auto charge = ChargeBytes(bytes, component.payload.size(), maximum); charge.HasError())
                    return charge;
            }
            for (const auto &collider : components.colliders) {
                if (collider.materials.size() > maximum / sizeof(Runtime::PhysicsColliderMaterialBinding))
                    return Result<void>::Failure(MakeError(PrefabErrors::PayloadTooLarge));
                if (auto charge = ChargeBytes(bytes, collider.materials.size() * sizeof(Runtime::PhysicsColliderMaterialBinding), maximum);
                    charge.HasError())
                    return charge;
            }
            if (auto charge = ChargeNavigationProfiles(components, bytes, maximum); charge.HasError())
                return charge;
            return ChargeBehaviors(components.behaviors, bytes, maximum, budget);
        }

        /** @brief Looks up one complete projection by stable key, rejecting missing or duplicate occurrences. */
        [[nodiscard]] Result<const PrefabRuntimeComponentProjection *> FindProjection(
            const std::span<const PrefabRuntimeComponentProjection> projections, const ExpandedPrefabObjectKey &key) {
            const PrefabRuntimeComponentProjection *found = nullptr;
            for (const auto &projection : projections) {
                if (projection.object != key)
                    continue;
                if (found != nullptr)
                    return Result<const PrefabRuntimeComponentProjection *>::Failure(MakeError(PrefabErrors::DocumentInvalid));
                found = &projection;
            }
            if (found == nullptr)
                return Result<const PrefabRuntimeComponentProjection *>::Failure(MakeError(PrefabErrors::DocumentInvalid));
            return Result<const PrefabRuntimeComponentProjection *>::Success(found);
        }

        /** @brief Computes root-inclusive depth only through already admitted source parents. */
        [[nodiscard]] Result<std::size_t> ObjectDepth(const ResolvedPrefabObject &object,
                                                      const std::span<const ResolvedPrefabObject> preceding,
                                                      const std::span<const std::size_t> depths) {
            if (!object.parent)
                return Result<std::size_t>::Success(1);
            const auto parent = std::ranges::find(preceding, *object.parent, &ResolvedPrefabObject::key);
            if (parent == preceding.end())
                return Result<std::size_t>::Failure(MakeError(PrefabErrors::HierarchyInvalid));
            return Result<std::size_t>::Success(depths[static_cast<std::size_t>(parent - preceding.begin())] + 1);
        }

        /** @brief Maps one already bounded object and composes only its outer placement root. */
        [[nodiscard]] Result<Runtime::RuntimeEntityDefinition> ProjectEntity(const ResolvedPrefabObject &object,
                                                                             const PrefabSceneIdentityMap &identities,
                                                                             const Runtime::RuntimeComponentSet &components,
                                                                             const PrefabRuntimePlacement &placement,
                                                                             const std::size_t index) {
            using EntityResult = Result<Runtime::RuntimeEntityDefinition>;
            const auto identity = identities.Find(object.key);
            if (!identity)
                return EntityResult::Failure(MakeError(PrefabErrors::IdentityCollision));
            if (const auto mapping = std::ranges::find(identities.Mappings(), object.key, &PrefabSceneIdentityMapping::source);
                mapping->sourcePrefab != object.sourcePrefab)
                return EntityResult::Failure(MakeError(PrefabErrors::IdentityCollision));
            auto parent = placement.parent;
            if (object.parent) {
                const auto mapped = identities.Find(*object.parent);
                if (!mapped)
                    return EntityResult::Failure(MakeError(PrefabErrors::HierarchyInvalid));
                parent = Runtime::SceneObjectId{mapped->value};
            } else if (index != 0)
                return EntityResult::Failure(MakeError(PrefabErrors::HierarchyInvalid));
            Math::Transform transform = object.effectiveLocalTransform;
            if (!object.parent) {
                auto composed = Math::TryDecomposeAffineTRS(Math::Multiply(placement.rootTransform.ToMatrix(), transform.ToMatrix()));
                if (composed.HasError())
                    return EntityResult::Failure(composed.ErrorValue());
                transform = composed.Value();
            }
            Runtime::RuntimeEntityDefinition entity{.object = {identity->value},
                                                    .parent = parent,
                                                    .localTransform = transform,
                                                    .components = components};
            if (auto valid = Runtime::ValidateRuntimeEntityDefinition(entity); valid.HasError())
                return EntityResult::Failure(valid.ErrorValue());
            return EntityResult::Success(std::move(entity));
        }

        /** @brief Checks count ceilings before iterating or copying dynamic component payloads. */
        [[nodiscard]] Result<void> ValidateCounts(const ResolvedPrefabObject &object, const Runtime::RuntimeComponentSet &components,
                                                  const PrefabLimitProfile &limits) {
            if (const auto maximum = limits.Policy().maximumComponentsPerObject;
                object.object.components.size() + object.object.behaviors.size() > maximum || ComponentCount(components) > maximum)
                return Result<void>::Failure(MakeError(PrefabErrors::ComponentCountExceeded));
            return Result<void>::Success();
        }

        /** @brief Rechecks per-object work, component and complete hierarchy admission. */
        [[nodiscard]] Result<std::size_t> ValidateObject(const ResolvedPrefabObject &object, const Runtime::RuntimeComponentSet &typed,
                                                         const std::span<const ResolvedPrefabObject> preceding,
                                                         const std::span<const std::size_t> depths, const PrefabLimitProfile &limits,
                                                         PrefabExpansionBudget &budget) {
            if (auto count = ValidateCounts(object, typed, limits); count.HasError())
                return Result<std::size_t>::Failure(count.ErrorValue());
            if (auto charge = budget.Consume(ComponentCount(typed)); charge.HasError())
                return Result<std::size_t>::Failure(charge.ErrorValue());
            auto depth = ObjectDepth(object, preceding, depths);
            if (depth.HasError())
                return Result<std::size_t>::Failure(depth.ErrorValue());
            if (depth.Value() > limits.Policy().maximumHierarchyDepth)
                return Result<std::size_t>::Failure(MakeError(PrefabErrors::HierarchyDepthExceeded));
            return depth;
        }

        /** @brief Validates complete snapshot pairing and placement admission before allocation. */
        [[nodiscard]] Result<void> ValidateInputs(const EffectivePrefabCandidate &candidate, const PrefabSceneIdentityMap &identities,
                                                  const std::span<const PrefabRuntimeComponentProjection> components,
                                                  const PrefabRuntimePlacement &placement, const PrefabLimitProfile &limits) {
            if (candidate.Revision() != identities.Revision())
                return Result<void>::Failure(MakeError(PrefabErrors::ResolutionStale));
            const auto objects = candidate.Objects();
            if (objects.size() > limits.Policy().maximumObjectCount)
                return Result<void>::Failure(MakeError(PrefabErrors::ObjectCountExceeded));
            if (objects.empty() || components.size() != objects.size() || identities.Mappings().size() != objects.size())
                return Result<void>::Failure(MakeError(PrefabErrors::DocumentInvalid));
            if (placement.parent && !placement.parent->IsValid())
                return Result<void>::Failure(MakeError(PrefabErrors::InvalidParent));
            if (auto matrix = placement.rootTransform.TryToMatrix(); matrix.HasError())
                return Result<void>::Failure(matrix.ErrorValue());

            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ExpandedPrefabSceneSubtree::ExpandedPrefabSceneSubtree */
    ExpandedPrefabSceneSubtree::ExpandedPrefabSceneSubtree(PrefabResolutionRevision revision,
                                                           std::vector<Runtime::RuntimeEntityDefinition> entities) noexcept
        : revision_(std::move(revision)), entities_(std::move(entities)) {}

    /** @copydoc ExpandedPrefabSceneSubtree::Entities */
    std::span<const Runtime::RuntimeEntityDefinition> ExpandedPrefabSceneSubtree::Entities() const noexcept {
        return entities_;
    }

    /** @copydoc ExpandedPrefabSceneSubtree::Revision */
    const PrefabResolutionRevision &ExpandedPrefabSceneSubtree::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc ExpandedPrefabSceneSubtree::ValidatePublication */
    Result<void> ExpandedPrefabSceneSubtree::ValidatePublication(const Assets::AssetId rootAsset,
                                                                 const PrefabSourceResolverSnapshot &current,
                                                                 const std::span<const Assets::AssetId> changedAssets,
                                                                 const PrefabLimitProfile &limits) const {
        return current.ValidateRevisionPublication(rootAsset, revision_, changedAssets, limits);
    }

    /** @copydoc ExpandPrefabSceneSubtree */
    Result<ExpandedPrefabSceneSubtree> ExpandPrefabSceneSubtree(const EffectivePrefabCandidate &candidate,
                                                                const PrefabSceneIdentityMap &identities,
                                                                const std::span<const PrefabRuntimeComponentProjection> components,
                                                                const PrefabRuntimePlacement &placement, const PrefabLimitProfile &limits) {
        using Output = Result<ExpandedPrefabSceneSubtree>;
        if (auto valid = ValidateInputs(candidate, identities, components, placement, limits); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        const auto objects = candidate.Objects();

        PrefabExpansionBudget budget{limits};
        std::vector<Runtime::RuntimeEntityDefinition> entities;
        std::vector<std::size_t> depths;
        entities.reserve(objects.size());
        depths.reserve(objects.size());
        std::size_t payloadBytes{};
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const auto &object = objects[index];
            if (auto charge = budget.Consume(1 + components.size() + index); charge.HasError())
                return Output::Failure(charge.ErrorValue());
            auto projection = FindProjection(components, object.key);
            if (projection.HasError())
                return Output::Failure(projection.ErrorValue());
            const auto &typed = projection.Value()->components;
            auto depth = ValidateObject(object, typed, objects.first(index), depths, limits, budget);
            if (depth.HasError())
                return Output::Failure(depth.ErrorValue());
            if (auto charge = ChargeComponents(typed, payloadBytes, limits.Policy().maximumExpandedPayloadBytes, budget); charge.HasError())
                return Output::Failure(charge.ErrorValue());
            auto entity = ProjectEntity(object, identities, typed, placement, index);
            if (entity.HasError())
                return Output::Failure(entity.ErrorValue());
            entities.push_back(std::move(entity).Value());
            depths.push_back(depth.Value());
        }
        return Output::Success(ExpandedPrefabSceneSubtree{candidate.Revision(), std::move(entities)});
    }
}  // namespace Horo::Prefab
