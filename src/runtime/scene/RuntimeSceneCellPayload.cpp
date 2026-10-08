#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Charges owned logical bytes with overflow-safe arithmetic before copying. */
        class StorageCharge final {
        public:
            explicit StorageCharge(const std::size_t maximum) noexcept : maximum_(maximum) {}

            bool Add(const std::size_t count, const std::size_t width = 1) noexcept {
                if (width == 0 || count > (maximum_ - bytes_) / width)
                    return false;
                bytes_ += count * width;
                return true;
            }

            template <typename T> bool Vector(const std::vector<T> &values) noexcept {
                return Add(values.size(), sizeof(T));
            }

            [[nodiscard]] std::size_t Bytes() const noexcept {
                return bytes_;
            }

        private:
            std::size_t maximum_;
            std::size_t bytes_{};
        };

        /** @brief Charges one canonical dependency record and its owned type name. */
        bool ChargeDependency(StorageCharge &charge, const SceneAssetDependency &dependency) {
            return charge.Add(1, sizeof(SceneAssetDependency)) && charge.Add(dependency.expectedType.Value().size());
        }

        /** @brief Accounts for owned behavior fields and nested string values. */
        bool ChargeBehavior(StorageCharge &charge, const Gameplay::BehaviorComponent &behavior) {
            if (!charge.Add(behavior.typeId.Value().size()) || !charge.Vector(behavior.fields))
                return false;
            for (const auto &field : behavior.fields) {
                if (!charge.Add(field.name.size()))
                    return false;
                const auto *text = std::get_if<std::string>(&field.value);
                if (text && !charge.Add(text->size()))
                    return false;
            }
            return true;
        }

        /** @brief Accounts for project-owned opaque components and behavior data without interpreting bytes. */
        bool ChargeGameplay(StorageCharge &charge, const RuntimeComponentSet &components) {
            if (!charge.Vector(components.behaviors) || !charge.Vector(components.gameplayComponents))
                return false;
            for (const auto &component : components.gameplayComponents)
                if (!charge.Add(component.typeId.Value().size()) || !charge.Vector(component.payload))
                    return false;
            for (const auto &behavior : components.behaviors)
                if (!ChargeBehavior(charge, behavior))
                    return false;
            return true;
        }

        /** @brief Accounts for every nested owning vector and string in the existing Runtime component model. */
        bool ChargeComponents(StorageCharge &charge, const RuntimeComponentSet &components) {
            if (!charge.Vector(components.colliders) || !charge.Vector(components.physicsConstraints))
                return false;
            for (const auto &collider : components.colliders)
                if (!charge.Vector(collider.materials))
                    return false;
            if (components.navigationSurface && !charge.Vector(components.navigationSurface->profiles))
                return false;
            if (components.navigationLink && !charge.Vector(components.navigationLink->profiles))
                return false;
            return ChargeGameplay(charge, components);
        }

        /** @brief Rejects core component modes outside the runtime's typed capabilities. */
        bool SupportedCoreComponents(const RuntimeComponentSet &components) {
            return (!components.camera || components.camera->projection <= CameraProjection::Orthographic) &&
                   (!components.light || components.light->kind <= LightKind::Spot);
        }

        /** @brief Rejects missing or mismatched required project schemas without weakening opaque payload semantics. */
        bool SupportedComponents(const RuntimeComponentSet &components, const SceneCellPayloadSource &source) {
            if (!SupportedCoreComponents(components))
                return false;
            for (const auto &component : components.gameplayComponents)
                if (std::ranges::none_of(source.componentSchemas, [&](const auto &schema) {
                    return schema.type == component.typeId && schema.version == component.schemaVersion;
                }))
                    return false;
            for (const auto &behavior : components.behaviors)
                if (std::ranges::none_of(source.behaviorSchemas, [&](const auto &schema) {
                    return schema.type == behavior.typeId && schema.version == behavior.schemaVersion;
                }))
                    return false;
            return true;
        }

        /** @brief Checks durable source identity independently from the captured publication comparison. */
        bool ValidIdentity(const SceneCellPayloadIdentity &identity) {
            return identity.partition.IsValid() && identity.cell.IsValid() && identity.scene.IsValid() && identity.revision.value != 0;
        }

        /** @brief Requires explicit positive ceilings before any ownership allocation. */
        bool ValidLimits(const SceneCellPayloadLimits limits) {
            return limits.maximumEntities != 0 && limits.maximumDependencies != 0 && limits.maximumRetainedBytes != 0;
        }

        /** @brief Validates exact topology/content identity and mandatory count ceilings before allocating storage. */
        Result<void> ValidateSource(const WorldStreaming::WorldPartitionDescriptor &partition, const SceneCellPayloadSource &source,
                                    const SceneCellPayloadIdentity &expected, const SceneCellPayloadLimits limits,
                                    const CancellationToken &cancellation) {
            const auto failure = [](const ErrorCodeDescriptor &code) {
                return Result<void>::Failure(MakeError(code));
            };
            if (cancellation.IsCancellationRequested())
                return failure(SceneCellPayloadErrors::Cancelled);
            if (!ValidIdentity(source.identity) || !ValidLimits(limits))
                return failure(SceneCellPayloadErrors::Invalid);
            if (source.identity != expected || source.identity.partition != partition.Partition())
                return failure(SceneCellPayloadErrors::Stale);
            if (std::ranges::none_of(partition.Cells(), [&](const auto &cell) {
                return cell.id == source.identity.cell;
            }))
                return failure(SceneCellPayloadErrors::Invalid);
            if (source.entities.size() > limits.maximumEntities || source.dependencies.size() > limits.maximumDependencies)
                return failure(SceneCellPayloadErrors::CapacityExceeded);

            return Result<void>::Success();
        }

        /** @brief Charges the complete borrowed baseline and validates strict required gameplay support before copying. */
        Result<void> ChargeSource(StorageCharge &charge, const SceneCellPayloadSource &source, const CancellationToken &cancellation) {
            if (!charge.Add(source.entities.size(), sizeof(RuntimeEntityDefinition)))
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
            std::unordered_set<std::uint64_t> behaviorIds;
            for (const auto &entity : source.entities) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                if (!ChargeComponents(charge, entity.components))
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
                for (const auto &behavior : entity.components.behaviors)
                    if (!behaviorIds.insert(behavior.instanceId.value).second)
                        return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
                if (!SupportedComponents(entity.components, source))
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Unsupported));
            }
            return Result<void>::Success();
        }

        /** @brief Charges each owned dependency once, matching the Scene builder's canonical deduplication. */
        Result<void> ChargeDependencies(StorageCharge &charge, const std::span<const SceneAssetDependency> dependencies,
                                        const CancellationToken &cancellation) {
            std::unordered_set<Assets::AssetId, Assets::AssetIdHash> charged;
            for (const auto &dependency : dependencies) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                if (charged.insert(dependency.id).second && !ChargeDependency(charge, dependency))
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
            }
            return Result<void>::Success();
        }

        /** @brief Copies the complete source through authoritative Scene validation with bounded cancellation checkpoints. */
        Result<RuntimeSceneDefinition> BuildDefinition(const SceneCellPayloadSource &source, const CancellationToken &cancellation) {
            SceneDefinitionBuilder builder{source.identity.scene, source.identity.revision};
            for (const auto &entity : source.entities) {
                if (cancellation.IsCancellationRequested())
                    return Result<RuntimeSceneDefinition>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                builder.Add(entity);
            }
            for (const auto &dependency : source.dependencies) {
                if (cancellation.IsCancellationRequested())
                    return Result<RuntimeSceneDefinition>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                if (const auto required = builder.RequireAsset(dependency); required.HasError())
                    return Result<RuntimeSceneDefinition>::Failure(required.ErrorValue());
            }
            return std::move(builder).Build();
        }

        /** @brief Includes authoritative builder-projected dependencies in the complete retained charge and count ceiling. */
        Result<void> ChargeProjectedDependencies(StorageCharge &charge, const RuntimeSceneDefinition &definition,
                                                 const SceneCellPayloadSource &source, const SceneCellPayloadLimits limits) {
            if (definition.AssetDependencies().size() > limits.maximumDependencies)
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
            for (const auto &dependency : definition.AssetDependencies()) {
                if (std::ranges::none_of(source.dependencies,
                                         [&](const auto &value) {
                    return value.id == dependency.id;
                }) &&
                    !ChargeDependency(charge, dependency))
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
            }
            return Result<void>::Success();
        }

        /** @brief Retains a cancellation observer and owned authority lease through deferred Scene publication. */
        class CellPublicationCheck final : public ScenePublicationCheck {
        public:
            CellPublicationCheck(SceneCellPayloadIdentity identity, WorldStreaming::StreamingFence fence,
                                 std::shared_ptr<const SceneCellPayloadAuthority> authority, CancellationToken cancellation)
                : identity_(std::move(identity)), fence_(std::move(fence)), authority_(std::move(authority)),
                  cancellation_(std::move(cancellation)) {}

            Result<void> ValidatePublication() const override {
                if (cancellation_.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                return authority_->ValidatePublication(identity_, fence_);
            }

        private:
            SceneCellPayloadIdentity identity_;
            WorldStreaming::StreamingFence fence_;
            std::shared_ptr<const SceneCellPayloadAuthority> authority_;
            CancellationToken cancellation_;
        };
    }  // namespace

    /** @copydoc RuntimeSceneCellPayload::RuntimeSceneCellPayload */
    RuntimeSceneCellPayload::RuntimeSceneCellPayload(SceneCellPayloadIdentity identity, RuntimeSceneDefinition definition,
                                                     const std::size_t retainedBytes) noexcept
        : identity_(std::move(identity)), definition_(std::move(definition)), retainedBytes_(retainedBytes) {}

    /** @copydoc RuntimeSceneCellPayload::Identity */
    const SceneCellPayloadIdentity &RuntimeSceneCellPayload::Identity() const noexcept {
        return identity_;
    }

    /** @copydoc RuntimeSceneCellPayload::Definition */
    const RuntimeSceneDefinition &RuntimeSceneCellPayload::Definition() const noexcept {
        return definition_;
    }

    /** @copydoc RuntimeSceneCellPayload::RetainedBytes */
    std::size_t RuntimeSceneCellPayload::RetainedBytes() const noexcept {
        return retainedBytes_;
    }

    /** @copydoc CookRuntimeSceneCellPayload */
    Result<RuntimeSceneCellPayload> CookRuntimeSceneCellPayload(const WorldStreaming::WorldPartitionDescriptor &partition,
                                                                const SceneCellPayloadSource &source,
                                                                const SceneCellPayloadIdentity &expected,
                                                                const SceneCellPayloadLimits limits,
                                                                const CancellationToken &cancellation) {
        if (const auto valid = ValidateSource(partition, source, expected, limits, cancellation); valid.HasError())
            return Result<RuntimeSceneCellPayload>::Failure(valid.ErrorValue());
        StorageCharge charge{limits.maximumRetainedBytes};
        if (const auto charged = ChargeSource(charge, source, cancellation); charged.HasError())
            return Result<RuntimeSceneCellPayload>::Failure(charged.ErrorValue());
        if (const auto charged = ChargeDependencies(charge, source.dependencies, cancellation); charged.HasError())
            return Result<RuntimeSceneCellPayload>::Failure(charged.ErrorValue());
        auto built = BuildDefinition(source, cancellation);
        if (built.HasError())
            return Result<RuntimeSceneCellPayload>::Failure(built.ErrorValue());
        if (const auto projected = ChargeProjectedDependencies(charge, built.Value(), source, limits); projected.HasError())
            return Result<RuntimeSceneCellPayload>::Failure(projected.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<RuntimeSceneCellPayload>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
        return Result<RuntimeSceneCellPayload>::Success(RuntimeSceneCellPayload{source.identity, std::move(built).Value(), charge.Bytes()});
    }

    /** @copydoc QueueRuntimeSceneCellPayload */
    Result<void> QueueRuntimeSceneCellPayload(RuntimeSceneService &service, const RuntimeSceneCellPayload &payload,
                                              const WorldStreaming::StreamingFence &fence,
                                              std::shared_ptr<const SceneCellPayloadAuthority> authority,
                                              const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
        if (!authority || !fence.IsValid())
            return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
        if (fence.partition != payload.Identity().partition || fence.cell != payload.Identity().cell)
            return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
        auto queued =
            service.QueuePreparationWithPublicationCheck(payload.Definition(),
                                                         std::make_unique<CellPublicationCheck>(payload.Identity(), fence,
                                                                                                std::move(authority), cancellation));
        return queued.HasError() ? Result<void>::Failure(queued.ErrorValue()) : Result<void>::Success();
    }
}  // namespace Horo::Runtime
