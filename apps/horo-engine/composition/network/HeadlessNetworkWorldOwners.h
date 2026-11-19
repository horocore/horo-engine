#pragma once
/** @file HeadlessNetworkWorldOwners.h
 * @brief Target-private generation-owned Scene, Physics and fixed-participant lifetimes.
 */
#include "HeadlessNavigationPathScope.h"
#include "HeadlessNetworkServices.h"
#include "HeadlessPrefabReplica.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"
#include "Horo/Gameplay/GameplayVfxScene.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "MountedWorldHost.h"
#include "PhysicsStateHostLifecycle.h"
#include "ProjectSceneNavigationComposition.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Application::Internal {
    class HeadlessDecisionQueries;
}

namespace Horo::Application::Internal::HeadlessNetworkDetail {
    class HeadlessFixedParticipant;

    /** @brief One exact prepared committed capture shared by the current authority world's initial recipients. */
    struct InitialTransformCapture final {
        Network::ReplicationWorldActivationDescriptor activation;
        std::uint64_t mappingRevision{};
        std::shared_ptr<const Network::ReplicationSerializerRegistry> serializers;
        std::unique_ptr<Network::ReplicationStateCapture> capture;
        std::uint64_t attemptedTick{};
        std::optional<Result<void>> sample;
    };

    /** @brief One generation-owned Scene/Physics pair; equal Scene IDs never imply shared storage. */
    struct WorldRecord final {
        explicit WorldRecord(const Network::NetworkModeServiceRequest &input) : request(input), native({input.scene, input.scene}) {}

        Network::NetworkModeServiceRequest request;
        std::uint64_t generation{};
        std::uint64_t decisionTick{}; /**< Last completed AI dispatch; a downstream same-tick retry does not replay it. */
        std::shared_ptr<Runtime::RuntimeScene> scene;
        std::weak_ptr<const Gameplay::GameplayPhysicsContext> gameplay;
        GameplayWorldComposition *activeGameplay{};
        std::shared_ptr<Gameplay::GameplayVfxScene> vfxScene;
        CancellationToken sceneCancellation;
        ScenePhysicsHost native;
        std::unique_ptr<Runtime::SceneAnimationRuntime> animation;
        bool fixedReserved{};
        bool closeRequested{};
        bool closed{};
        std::unique_ptr<AI::AiSceneRuntime> ai;
        std::shared_ptr<Network::SceneReplicationCommitSource> publicTransforms;
        std::optional<InitialTransformCapture> initialTransformCapture;
        std::shared_ptr<HeadlessDecisionQueries> decisionQueries; /**< Keeps task binding and phase inventory scoped to this world. */
        AI::PerceptionSpatialBroadphase perception;
        std::unique_ptr<ProjectSceneNavigationWorld> navigation;
        std::unique_ptr<HeadlessNavigationPathScope>
            navigationPath; /**< Revoked before Scene shutdown, drained before releasing its coordinator. */
        std::unique_ptr<Gameplay::PreparedGameModuleImage>
            preparedGameplay;                                        /**< Retained through metadata qualification and scoped activation. */
        std::vector<AI::AiControllerDescriptor> admittedControllers; /**< Outlives the Scene participant's immutable borrow. */
        std::shared_ptr<Runtime::RuntimeSceneService> scenes;      /**< World owns shutdown; apply candidates observe through a weak pin. */
        std::unique_ptr<MountedWorld> mountedWorld;                /**< Retires before the Scene service it binds. */
        std::unique_ptr<Prefab::PrefabTemplateProvider> templates; /**< Retired before Scene, borrowing its real loader. */
        std::optional<HeadlessPrefabReplica> prefabReplica;        /**< Scene-side network operations for this generation only. */
    };

    struct WorldOwners final {
        WorldOwners(std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition,
                    const std::optional<GameplayWorldSelection> &gameplay,
                    const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &admission)
            : definition(std::move(definition)), gameplay(gameplay), replicationAdmission(admission) {}

        ~WorldOwners() {
            try {
                auto retired = PollRetirement(64);
                if (retired.HasError() || !retired.Value())
                    std::terminate();
            } catch (const std::exception &) {
                std::terminate();
            }
        }

        /** @brief Binds the actual process before any native World/resource can be created. */
        Result<void> PreparePhysicsRuntime(Physics::PhysicsRuntimeMode mode) {
            if (std::this_thread::get_id() != owner)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
            if (retirementStarted)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
            if (physics)
                return Result<void>::Success();
            auto created = Physics::PhysicsRuntime::Create(mode);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            auto runtime = std::move(created).Value();
            auto bound = PrepareProductPhysicsState(*runtime);
            if (bound.HasError())
                return Result<void>::Failure(bound.ErrorValue());
            stateUnavailable = std::move(bound).Value();
            physics = std::move(runtime);
            return Result<void>::Success();
        }

        /** @brief Reserves bounded retention before any module callback can submit a path. */
        Result<void> HoldNavigationRetirement(const std::shared_ptr<WorldRecord> &record) {
            if (auto retired = PollNavigationRetirement(navigationRetirements.size()); retired.HasError())
                return Result<void>::Failure(retired.ErrorValue());
            const auto available = std::ranges::find_if(navigationRetirements, [](const auto &retained) {
                return !retained;
            });
            if (available == navigationRetirements.end())
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
            *available = record;
            return Result<void>::Success();
        }

        /** @brief Drains only closed worlds' owned query leases; active paths are never reconciled here. */
        Result<bool> PollNavigationRetirement(const std::size_t maximumRecords) {
            bool complete = true;
            std::size_t observed{};
            for (auto &record : navigationRetirements) {
                if (!record)
                    continue;
                if (!record->closed || observed == maximumRecords) {
                    complete = false;
                    continue;
                }
                ++observed;
                auto retired = record->navigationPath->PollRetirement();
                if (retired.HasError())
                    return Result<bool>::Failure(retired.ErrorValue());
                if (!retired.Value()) {
                    complete = false;
                    continue;
                }
                record->navigationPath.reset();
                record.reset();
            }
            return Result<bool>::Success(complete);
        }

        /** @brief Called only after actual product services stop; preserves SAME owner on pending/error. */
        Result<bool> PollRetirement(std::uint32_t maximumRecords) {
            if (std::this_thread::get_id() != owner)
                return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
            retirementStarted = true;
            auto paths = PollNavigationRetirement(maximumRecords);
            if (paths.HasError())
                return paths;
            auto physicsOwners = PollProductPhysicsRetirement(physics, retirement, maximumRecords);
            if (physicsOwners.HasError())
                return physicsOwners;
            return Result<bool>::Success(paths.Value() && physicsOwners.Value());
        }

        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition;
        std::unique_ptr<Physics::PhysicsRuntime> physics;
        std::optional<Physics::PhysicsRuntimeRetirement> retirement;
        std::optional<Error> stateUnavailable;
        bool retirementStarted{};
        std::optional<GameplayWorldSelection> gameplay;
        std::optional<Gameplay::GameplayReplicationAdmissionPolicy> replicationAdmission;
        // Factory calls are serialized on the host owner thread. Scene is immediately followed
        // by its Physics factory in NetworkModeComposition, including travel candidates.
        std::array<std::weak_ptr<WorldRecord>, 4> unpaired;
        std::uint64_t nextWorldGeneration{1};
        const std::thread::id owner{std::this_thread::get_id()};
        std::optional<HeadlessSceneActivationInput> configured;
        Physics::PhysicsSceneActivationAuthority authority;
        // NetworkModePlan admits at most two current roles plus two replacement roles.
        // Weak receipts retain known IDs while any actual service still owns that record;
        // retired slots are reusable without keeping an unbounded historical ID set.
        std::array<std::weak_ptr<WorldRecord>, 4> known;
        std::array<std::shared_ptr<WorldRecord>, 4>
            navigationRetirements;          /**< Keeps revoked world/SDK scopes alive through actual worker fences. */
        HeadlessFixedParticipant *fixed{};  // Borrowed only while its lifecycle owner retains these owners.
        bool fixedBound{};                  // Permanent one-shot binding, even after the participant retires.
    };

    /** @brief Closes this generation's dependent owners after its fixed reservation retires. */
    inline void CloseRecord(WorldRecord &record) noexcept {
        if (record.closed)
            return;
        record.closeRequested = true;
        if (record.fixedReserved)
            return;
        record.closed = true;
        record.initialTransformCapture.reset();
        record.publicTransforms.reset();
        if (record.animation)
            record.animation->Shutdown();
        if (record.templates)
            record.templates->Shutdown();
        if (record.navigationPath)
            record.navigationPath->Revoke();
        if (record.navigation)
            record.navigation->coordinator.BeginShutdown();
        record.mountedWorld.reset();
        if (record.scenes)
            record.scenes->Shutdown();
        if (record.ai)
            record.ai->BeginShutdown();
        if (record.preparedGameplay) {
            record.templates.reset();
            record.scenes.reset();
            record.admittedControllers.clear();
            record.preparedGameplay.reset();
        }
    }

}  // namespace Horo::Application::Internal::HeadlessNetworkDetail
