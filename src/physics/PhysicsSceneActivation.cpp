#include "Horo/Physics/PhysicsSceneActivation.h"

#include "Horo/Physics/CharacterWorld.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <new>
#include <thread>
#include <utility>

namespace Horo::Physics {
    namespace {
        struct ResidentTriggerVolume final {
            Runtime::SceneObjectId object;
            PhysicsQueryFixture fixture;
        };

        /** @brief Releases native trigger fixtures before retiring their owning Physics world. */
        void DestroyTriggerFixtures(const PhysicsWorld &physics, std::vector<ResidentTriggerVolume> &fixtures) noexcept {
            for (const ResidentTriggerVolume &fixture : fixtures)
                (void)physics.DestroyQueryFixture(fixture.fixture);
            fixtures.clear();
        }

        /** @brief Admits every converted trigger fixture and rolls back partial admission on failure. */
        [[nodiscard]] Result<std::vector<ResidentTriggerVolume>> CreateTriggerFixtures(
            const PhysicsWorld &physics, const std::vector<PhysicsTriggerVolumeBinding> &bindings) {
            std::vector<ResidentTriggerVolume> fixtures;
            try {
                fixtures.reserve(bindings.size());
                for (const PhysicsTriggerVolumeBinding &binding : bindings) {
                    auto fixture = physics.CreateQueryFixture(binding.fixture);
                    if (fixture.HasError()) {
                        DestroyTriggerFixtures(physics, fixtures);
                        return Result<std::vector<ResidentTriggerVolume>>::Failure(std::move(fixture).ErrorValue());
                    }
                    fixtures.push_back({.object = binding.object, .fixture = std::move(fixture).Value()});
                }
                return Result<std::vector<ResidentTriggerVolume>>::Success(std::move(fixtures));
            } catch (const std::bad_alloc &) {
                DestroyTriggerFixtures(physics, fixtures);
                return Result<std::vector<ResidentTriggerVolume>>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            }
        }

        /** @brief Owns the dependency-ordered Physics and Character resources prepared for one candidate. */
        struct PreparedPhysicsResources final {
            std::unique_ptr<PhysicsWorld> physics;
            std::unique_ptr<Character::CharacterWorld> character;
        };

        /** @brief Prepares, pairs, and activates the Physics and Character resources without publishing scene state. */
        [[nodiscard]] Result<PreparedPhysicsResources> PreparePhysicsResources(PhysicsRuntime &runtime,
                                                                               const PhysicsSceneActivationSettings &settings,
                                                                               const Runtime::RuntimeSceneView scene,
                                                                               const PhysicsSceneActivationEvidence evidence,
                                                                               const PhysicsWorldId identity) {
            auto preparedPhysics = runtime.PrepareWorld(settings.physics);
            if (preparedPhysics.HasError())
                return Result<PreparedPhysicsResources>::Failure(std::move(preparedPhysics).ErrorValue());
            auto physics = std::move(preparedPhysics).Value();
            const Character::CharacterWorldPreparationDescriptor characterDescriptor{
                .sceneGeneration = scene.RuntimeId().value,
                .physicsWorld = identity,
                .collisionFilterGeneration = evidence.collisionFilterGeneration,
                .originGeneration = evidence.originGeneration,
            };
            auto preparedCharacter = Character::CharacterWorld::Prepare(characterDescriptor, settings.character);
            if (preparedCharacter.HasError()) {
                physics->Shutdown();
                return Result<PreparedPhysicsResources>::Failure(std::move(preparedCharacter).ErrorValue());
            }
            auto character = std::move(preparedCharacter).Value();
            if (const Result<void> activated = physics->Activate(identity); activated.HasError()) {
                character->Shutdown();
                physics->Shutdown();
                return Result<PreparedPhysicsResources>::Failure(activated.ErrorValue());
            }
            return Result<PreparedPhysicsResources>::Success(
                PreparedPhysicsResources{.physics = std::move(physics), .character = std::move(character)});
        }

        /** @brief Owns one unpublished or active scene's dependency-ordered Physics resources. */
        class PhysicsSceneCandidate final : public Runtime::SceneActivationCandidate {
        public:
            PhysicsSceneCandidate(std::unique_ptr<PhysicsWorld> physics, std::unique_ptr<Character::CharacterWorld> character,
                                  const PhysicsSceneActivationAuthority &authority, const PhysicsSceneActivationEvidence evidence,
                                  const Runtime::RuntimeSceneView scene, std::vector<ResidentTriggerVolume> triggerVolumes) noexcept
                : physics_(std::move(physics)), character_(std::move(character)), authority_(&authority), evidence_(evidence),
                  scene_(scene), triggerVolumes_(std::move(triggerVolumes)) {}

            [[nodiscard]] Result<void> ValidatePublication() const override {
                if (!scene_.IsCurrent() || !authority_->IsCurrent(evidence_))
                    return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                DestroyTriggerFixtures(*physics_, triggerVolumes_);
                character_->Shutdown();
                physics_->Shutdown();
            }

        private:
            std::unique_ptr<PhysicsWorld> physics_;
            std::unique_ptr<Character::CharacterWorld> character_;
            const PhysicsSceneActivationAuthority *authority_{};
            PhysicsSceneActivationEvidence evidence_;
            Runtime::RuntimeSceneView scene_;
            std::vector<ResidentTriggerVolume> triggerVolumes_;
        };
    }  // namespace

    /** @copydoc PhysicsSceneActivationAuthority::PhysicsSceneActivationAuthority */
    PhysicsSceneActivationAuthority::PhysicsSceneActivationAuthority() noexcept : ownerThread_(std::this_thread::get_id()) {}

    /** @copydoc PhysicsSceneActivationAuthority::Capture */
    PhysicsSceneActivationEvidence PhysicsSceneActivationAuthority::Capture() const noexcept {
        return current_;
    }

    /** @copydoc PhysicsSceneActivationAuthority::IsCurrent */
    bool PhysicsSceneActivationAuthority::IsCurrent(const PhysicsSceneActivationEvidence evidence) const noexcept {
        return std::this_thread::get_id() == ownerThread_ && evidence == current_;
    }

    Result<void> PhysicsSceneActivationAuthority::Advance(std::uint64_t &generation) const {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (generation == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        ++generation;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneActivationAuthority::AdvanceCollisionFilterGeneration */
    Result<void> PhysicsSceneActivationAuthority::AdvanceCollisionFilterGeneration() {
        return Advance(current_.collisionFilterGeneration);
    }

    /** @copydoc PhysicsSceneActivationAuthority::AdvanceOriginGeneration */
    Result<void> PhysicsSceneActivationAuthority::AdvanceOriginGeneration() {
        return Advance(current_.originGeneration);
    }

    /** @copydoc PhysicsSceneActivationParticipant::PhysicsSceneActivationParticipant */
    PhysicsSceneActivationParticipant::PhysicsSceneActivationParticipant(PhysicsRuntime &runtime,
                                                                         PhysicsSceneActivationAuthority &authority,
                                                                         PhysicsSceneActivationSettings settings) noexcept
        : runtime_(&runtime), authority_(&authority), settings_(std::move(settings)) {}

    /** @copydoc PhysicsSceneActivationParticipant::Prepare */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> PhysicsSceneActivationParticipant::Prepare(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView scene) {
        if (const std::array valid{runtime_->State() == PhysicsRuntimeState::Ready, scene.IsCurrent(), scene.RuntimeId().IsValid()};
            !std::ranges::all_of(valid, std::identity{})) {
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        }

        auto triggerBindings = BuildPhysicsTriggerVolumeBindings(definition);
        if (triggerBindings.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(std::move(triggerBindings).ErrorValue());
        if (triggerBindings.Value().size() > settings_.physics.Values().budgets.maximumShapes)
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Trigger volume count exceeds the configured Physics shape budget."));

        const PhysicsSceneActivationEvidence evidence = authority_->Capture();
        const auto identity = runtime_->IssueWorldIdentity();
        if (identity.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(identity.ErrorValue());
        auto preparedResources = PreparePhysicsResources(*runtime_, settings_, scene, evidence, identity.Value());
        if (preparedResources.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(std::move(preparedResources).ErrorValue());
        auto resources = std::move(preparedResources).Value();
        auto physics = std::move(resources.physics);
        auto character = std::move(resources.character);
        const auto shutdownPrepared = [&]() noexcept {
            character->Shutdown();
            physics->Shutdown();
        };

        auto residentTriggerVolumes = CreateTriggerFixtures(*physics, triggerBindings.Value());
        if (residentTriggerVolumes.HasError()) {
            shutdownPrepared();
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(std::move(residentTriggerVolumes).ErrorValue());
        }
        auto resident = std::move(residentTriggerVolumes).Value();

        if (const Result<void> activated = character->Activate(); activated.HasError()) {
            DestroyTriggerFixtures(*physics, resident);
            shutdownPrepared();
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(activated.ErrorValue());
        }
        try {
            auto candidate = std::make_unique<PhysicsSceneCandidate>(std::move(physics), std::move(character), *authority_, evidence, scene,
                                                                     std::move(resident));
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            DestroyTriggerFixtures(*physics, resident);
            shutdownPrepared();
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Physics
