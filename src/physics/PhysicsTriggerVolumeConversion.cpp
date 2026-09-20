#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsSceneActivation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace {
        constexpr std::size_t kMaximumTriggerVolumeBindings = 65'536;
        constexpr float kTransformRoundTripEpsilon = 0.0001F;

        // These identities are a temporary, explicit bridge for the legacy shape-only component. They
        // are inert descriptors until the canonical collision schema migration supplies project-owned IDs.
        constexpr CollisionLayerId kLegacyTriggerVolumeLayer =
            CollisionLayerId::FromBytes({0x48, 0x6f, 0x72, 0x6f, 0x50, 0x68, 0x79, 0x73, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01});
        constexpr CollisionProfileId kLegacyTriggerVolumeProfile =
            CollisionProfileId::FromBytes({0x48, 0x6f, 0x72, 0x6f, 0x50, 0x68, 0x79, 0x73, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02});
        constexpr PhysicsQueryChannelId kLegacyTriggerVolumeChannel = PhysicsQueryChannelId::FromBytes(
            {0x48, 0x6f, 0x72, 0x6f, 0x50, 0x68, 0x79, 0x73, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x03});

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &code, std::string message = {}) {
            return Result<T>::Failure(MakeError(code, std::move(message)));
        }

        [[nodiscard]] bool NearlyEqualMatrix(const Math::Mat4 &lhs, const Math::Mat4 &rhs) noexcept {
            for (std::size_t index = 0; index < lhs.values.size(); ++index) {
                const float scale = std::max({1.0F, std::fabs(lhs.values[index]), std::fabs(rhs.values[index])});
                if (std::fabs(lhs.values[index] - rhs.values[index]) > kTransformRoundTripEpsilon * scale)
                    return false;
            }
            return true;
        }

        /** @brief Resolves one definition hierarchy into finite world matrices without native access. */
        class WorldTransformResolver final {
        public:
            WorldTransformResolver(const std::span<const Runtime::RuntimeEntityDefinition> entities,
                                   const std::unordered_map<std::uint64_t, std::size_t> &indices)
                : entities_(entities), indices_(indices), worldMatrices_(entities.size()), states_(entities.size()) {}

            [[nodiscard]] Result<Math::Mat4> Resolve(const std::size_t index) {
                using enum State;
                if (index >= entities_.size())
                    return Failure<Math::Mat4>(PhysicsErrors::DescriptorInvalid, "Trigger volume entity index is out of range.");
                if (states_[index] == Complete)
                    return Result<Math::Mat4>::Success(worldMatrices_[index]);
                if (states_[index] == Visiting)
                    return Failure<Math::Mat4>(PhysicsErrors::DescriptorInvalid, "Trigger volume hierarchy contains a cycle.");

                states_[index] = Visiting;
                auto local = entities_[index].localTransform.TryToMatrix();
                if (local.HasError()) {
                    states_[index] = Unvisited;
                    return Result<Math::Mat4>::Failure(
                        WrapError(PhysicsErrors::DescriptorInvalid, std::move(local).ErrorValue(),
                                  "Trigger volume local transform cannot be converted to a finite affine matrix."));
                }

                Math::Mat4 world = local.Value();
                if (entities_[index].parent) {
                    const auto parent = indices_.find(entities_[index].parent->value);
                    if (parent == indices_.end()) {
                        states_[index] = Unvisited;
                        return Failure<Math::Mat4>(PhysicsErrors::DescriptorInvalid,
                                                   "Trigger volume parent identity is not present in the definition.");
                    }
                    auto parentWorld = Resolve(parent->second);
                    if (parentWorld.HasError()) {
                        states_[index] = Unvisited;
                        return Result<Math::Mat4>::Failure(std::move(parentWorld).ErrorValue());
                    }
                    world = Math::Multiply(parentWorld.Value(), world);
                }

                if (!Math::IsFinite(world)) {
                    states_[index] = Unvisited;
                    return Failure<Math::Mat4>(PhysicsErrors::DescriptorInvalid,
                                               "Trigger volume world transform overflowed finite affine bounds.");
                }
                worldMatrices_[index] = world;
                states_[index] = Complete;
                return Result<Math::Mat4>::Success(world);
            }

        private:
            enum class State : std::uint8_t {
                Unvisited,
                Visiting,
                Complete,
            };

            std::span<const Runtime::RuntimeEntityDefinition> entities_;
            const std::unordered_map<std::uint64_t, std::size_t> &indices_;
            std::vector<Math::Mat4> worldMatrices_;
            std::vector<State> states_;
        };

        [[nodiscard]] Result<PhysicsShapeDescriptor> MakeTriggerGeometry(const Runtime::ColliderShapeType shape) {
            using enum Runtime::ColliderShapeType;
            switch (shape) {
                case Box:
                    return Result<PhysicsShapeDescriptor>::Success(PhysicsBoxShape{});
                case Sphere:
                    return Result<PhysicsShapeDescriptor>::Success(PhysicsSphereShape{});
                case Capsule:
                    return Result<PhysicsShapeDescriptor>::Success(PhysicsCapsuleShape{});
                case StaticPlane:
                    return Result<PhysicsShapeDescriptor>::Success(PhysicsStaticPlaneShape{});
                default:
                    return Failure<PhysicsShapeDescriptor>(PhysicsErrors::OperationUnsupported,
                                                           "Trigger volume shape is not part of the analytic runtime vocabulary.");
            }
        }

        [[nodiscard]] Result<PhysicsTriggerVolumeBinding> MakeTriggerBinding(const Runtime::RuntimeEntityDefinition &entity,
                                                                             const std::size_t index, WorldTransformResolver &transforms) {
            auto worldMatrix = transforms.Resolve(index);
            if (worldMatrix.HasError())
                return Result<PhysicsTriggerVolumeBinding>::Failure(std::move(worldMatrix).ErrorValue());
            auto worldTransform = Math::TryDecomposeAffineTRS(worldMatrix.Value());
            if (worldTransform.HasError())
                return Result<PhysicsTriggerVolumeBinding>::Failure(
                    WrapError(PhysicsErrors::DescriptorInvalid, std::move(worldTransform).ErrorValue(),
                              "Trigger volume world transform is singular or cannot be represented as translation, rotation and scale."));
            if (!NearlyEqualMatrix(worldMatrix.Value(), worldTransform.Value().ToMatrix()))
                return Failure<PhysicsTriggerVolumeBinding>(PhysicsErrors::OperationUnsupported,
                                                            "Trigger volume world transform contains shear that the analytic Physics "
                                                            "contract cannot represent.");

            auto geometry = MakeTriggerGeometry(entity.components.triggerVolume->shape);
            if (geometry.HasError())
                return Result<PhysicsTriggerVolumeBinding>::Failure(std::move(geometry).ErrorValue());
            const PhysicsPrimitiveShapeRequest request{
                .geometry = std::move(geometry).Value(),
                .localPose = PhysicsPose{.translation = {}, .rotation = Math::Quaternion::Identity()},
                .scale = PhysicsShapeScale{.factors = worldTransform.Value().scale},
            };
            auto resolved = ResolvePhysicsPrimitiveShape(request);
            if (resolved.HasError())
                return Result<PhysicsTriggerVolumeBinding>::Failure(std::move(resolved).ErrorValue());

            PhysicsQueryFixtureDescriptor fixture{
                .shape = resolved.Value().geometry,
                .pose = PhysicsPose{.translation = worldTransform.Value().translation, .rotation = worldTransform.Value().rotation},
                .layer = kLegacyTriggerVolumeLayer,
                .profile = kLegacyTriggerVolumeProfile,
                .channel = kLegacyTriggerVolumeChannel,
            };
            fixture.response = PhysicsQueryFixtureResponse::Overlap;
            fixture.trigger = true;
            return Result<PhysicsTriggerVolumeBinding>::Success({.object = entity.object, .fixture = std::move(fixture)});
        }
    }  // namespace

    /** @copydoc BuildPhysicsTriggerVolumeBindings */
    Result<std::vector<PhysicsTriggerVolumeBinding>> BuildPhysicsTriggerVolumeBindings(const Runtime::RuntimeSceneDefinition &definition) {
        try {
            const std::span<const Runtime::RuntimeEntityDefinition> entities = definition.Entities();
            std::size_t enabledCount = 0;
            for (const Runtime::RuntimeEntityDefinition &entity : entities) {
                if (!entity.components.triggerVolume || !entity.components.triggerVolume->enabled)
                    continue;
                ++enabledCount;
                if (enabledCount > kMaximumTriggerVolumeBindings)
                    return Failure<std::vector<PhysicsTriggerVolumeBinding>>(PhysicsErrors::CapacityExceeded,
                                                                             "Scene exceeds the bounded trigger volume admission limit.");
            }
            if (enabledCount == 0)
                return Result<std::vector<PhysicsTriggerVolumeBinding>>::Success(std::vector<PhysicsTriggerVolumeBinding>{});

            std::unordered_map<std::uint64_t, std::size_t> indices;
            indices.reserve(entities.size());
            for (std::size_t index = 0; index < entities.size(); ++index) {
                if (!indices.emplace(entities[index].object.value, index).second)
                    return Failure<
                        std::vector<PhysicsTriggerVolumeBinding>>(PhysicsErrors::DescriptorInvalid,
                                                                  "Trigger volume conversion requires unique authored object identities.");
            }

            WorldTransformResolver transforms{entities, indices};
            std::vector<PhysicsTriggerVolumeBinding> bindings;
            bindings.reserve(enabledCount);
            for (std::size_t index = 0; index < entities.size(); ++index) {
                const Runtime::RuntimeEntityDefinition &entity = entities[index];
                if (!entity.components.triggerVolume || !entity.components.triggerVolume->enabled)
                    continue;

                auto binding = MakeTriggerBinding(entity, index, transforms);
                if (binding.HasError())
                    return Result<std::vector<PhysicsTriggerVolumeBinding>>::Failure(std::move(binding).ErrorValue());
                bindings.push_back(std::move(binding).Value());
            }

            std::ranges::sort(bindings, {}, [](const PhysicsTriggerVolumeBinding &binding) {
                return binding.object.value;
            });
            return Result<std::vector<PhysicsTriggerVolumeBinding>>::Success(std::move(bindings));
        } catch (const std::bad_alloc &) {
            return Failure<
                std::vector<PhysicsTriggerVolumeBinding>>(PhysicsErrors::CapacityExceeded,
                                                          "Trigger volume conversion exceeded available bounded preparation storage.");
        }
    }
}  // namespace Horo::Physics
