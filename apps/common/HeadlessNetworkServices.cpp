#include "HeadlessNetworkServices.h"

#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "PhysicsStateHostLifecycle.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        class HeadlessFixedParticipant;

        /** @brief One generation-owned Scene/Physics pair; equal Scene IDs never imply shared storage. */
        struct WorldRecord final {
            explicit WorldRecord(const Network::NetworkModeServiceRequest &input) : request(input), native({input.scene, input.scene}) {}

            Network::NetworkModeServiceRequest request;
            std::uint64_t generation{};
            std::shared_ptr<Runtime::RuntimeScene> scene;
            std::weak_ptr<const Gameplay::GameplayPhysicsContext> gameplay;
            GameplayWorldComposition *activeGameplay{};
            CancellationToken sceneCancellation;
            ScenePhysicsHost native;
            std::unique_ptr<Runtime::SceneAnimationRuntime> animation;
            bool fixedReserved{};
            bool closeRequested{};
            bool closed{};
            std::unique_ptr<AI::AiSceneRuntime> ai;
            std::unique_ptr<Runtime::RuntimeSceneService> scenes;
            std::unique_ptr<Prefab::PrefabTemplateProvider> templates; /**< Retired before Scene, borrowing its real loader. */
        };

        struct WorldOwners final {
            WorldOwners(std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition,
                        const std::optional<GameplayWorldSelection> &gameplay,
                        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &admission)
                : definition(std::move(definition)), gameplay(gameplay), replicationAdmission(admission) {}

            ~WorldOwners() {
                auto retired = PollRetirement(64);
                if (retired.HasError() || !retired.Value())
                    std::terminate();
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

            /** @brief Called only after actual product services stop; preserves SAME owner on pending/error. */
            Result<bool> PollRetirement(std::uint32_t maximumRecords) {
                if (std::this_thread::get_id() != owner)
                    return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
                retirementStarted = true;
                return PollProductPhysicsRetirement(physics, retirement, maximumRecords);
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
            HeadlessFixedParticipant *fixed{};  // Borrowed only while its lifecycle owner retains these owners.
            bool fixedBound{};                  // Permanent one-shot binding, even after the participant retires.
        };

        void CloseRecord(WorldRecord &record) noexcept {
            if (record.closed)
                return;
            record.closeRequested = true;
            if (record.fixedReserved)
                return;
            record.closed = true;
            if (record.animation)
                record.animation->Shutdown();
            if (record.templates)
                record.templates->Shutdown();
            if (record.scenes)
                record.scenes->Shutdown();
            if (record.ai)
                record.ai->BeginShutdown();
        }

#include "HeadlessAnimationFixed.h"

        class WorldService final : public Network::INetworkModeService {
        public:
            [[nodiscard]] std::shared_ptr<const Gameplay::GameplayPhysicsContext> PhysicsContext() const noexcept {
                return gameplay_ ? gameplay_->PhysicsContext() : nullptr;
            }

            const Runtime::SceneAnimationRuntime *AnimationOwner() const noexcept {
                return std::this_thread::get_id() == owners_->owner && record_ ? record_->animation.get() : nullptr;
            }

            std::optional<Runtime::EntityRef> AnimationEntity(Runtime::SceneObjectId object) const noexcept {
                if (std::this_thread::get_id() != owners_->owner || !record_ || !record_->scenes)
                    return {};
                const auto scene = record_->scenes->ActiveScene();
                return scene ? scene->Find(object) : std::nullopt;
            }

            WorldService(std::shared_ptr<WorldOwners> owners, const Network::NetworkModeServiceRequest &request,
                         std::shared_ptr<WorldRecord> record)
                : owners_(std::move(owners)), request_(request), record_(std::move(record)) {}

            ~WorldService() override {
                Shutdown();
            }

            Result<void> Prepare() override {
                if (const auto lane = CheckOwnerLane(); lane.HasError())
                    return lane;
                switch (request_.service) {
                    case Network::NetworkModeServiceKind::Scene: {
                        if (owners_->definition == nullptr)
                            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeUnavailable,
                                                                   "No cooked runtime Scene definition was supplied."));
                        if (owners_->configured)
                            return PrepareConfiguredScene();
                        auto scene = Runtime::RuntimeScene::Create(*owners_->definition, request_.scene);
                        if (scene.HasError())
                            return Result<void>::Failure(scene.ErrorValue());
                        record_->scene = std::move(scene).Value();
                        return Result<void>::Success();
                    }
                    case Network::NetworkModeServiceKind::Physics: {
                        if (owners_->configured) {
                            if (!record_ || !record_->scenes || !record_->scenes->ActiveScene() ||
                                record_->scenes->ActiveScene()->RuntimeId() != request_.scene)
                                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
                            auto borrowed = record_->native.Borrow();
                            return borrowed.HasError() ? Result<void>::Failure(borrowed.ErrorValue()) : Result<void>::Success();
                        }
                        return PreparePhysics();
                    }
                    case Network::NetworkModeServiceKind::Replication:
                        // NetworkModeComposition owns the canonical Stage/Commit/Acquire lifecycle.
                        // This process-scoped facade never creates a competing empty world owner.
                        return Result<void>::Success();
                    default:
                        return Result<void>::Failure(
                            MakeError(Network::NetworkErrors::NetworkModeInvalid, "Headless world factory received a non-world service."));
                }
            }

            Result<void> Activate() override {
                if (const auto lane = CheckOwnerLane(); lane.HasError())
                    return lane;
                if (request_.service != Network::NetworkModeServiceKind::Physics)
                    return Result<void>::Success();
                if (auto admitted = PrepareGameplaySelection(); admitted.HasError())
                    return admitted;
                if (owners_->configured) {
                    if (configuredActivated_)
                        return Result<void>::Failure(
                            MakeError(Network::NetworkErrors::NetworkModeInvalid, "This paired Physics service was already activated."));
                    auto activated = ActivateConfiguredGameplay();
                    configuredActivated_ = activated.HasValue();
                    return activated;
                }
                auto identity = owners_->physics->IssueWorldIdentity();
                if (identity.HasError())
                    return Result<void>::Failure(identity.ErrorValue());
                if (auto activated = physicsWorld_->Activate(identity.Value()); activated.HasError())
                    return activated;
                if (selectedGameplay_) {
                    auto gameplay =
                        GameplayWorldComposition::Create(*record_->scene, physicsWorld_.get(), record_->generation, *selectedGameplay_);
                    if (gameplay.HasError())
                        return Result<void>::Failure(gameplay.ErrorValue());
                    gameplay_ = std::move(gameplay).Value();
                    record_->gameplay = gameplay_->PhysicsContext();
                }
                return Result<void>::Success();
            }

            Result<void> RunPhase(const Runtime::RuntimePhase phase) override {
                if (const auto lane = CheckOwnerLane(); lane.HasError())
                    return lane;
                if (owners_->gameplay && owners_->gameplay->moduleRetirements)
                    (void)owners_->gameplay->moduleRetirements->Drain();
                if (owners_->configured && request_.service == Network::NetworkModeServiceKind::Physics && gameplay_) {
                    const Runtime::FrameContext frame{.frameNumber = ++phaseFrame_, .cancellation = record_->sceneCancellation};
                    return gameplay_->OnPhase(phase, frame);
                }
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &context) override {
                if (const auto lane = CheckOwnerLane(); lane.HasError())
                    return lane;
                if (request_.service != Network::NetworkModeServiceKind::Physics)
                    return Result<void>::Success();
                if (owners_->configured)
                    return RunConfiguredTick(context);
                if (gameplay_) {
                    if (auto advanced = gameplay_->FixedTick(context); advanced.HasError())
                        return advanced;
                }
                // Gameplay submits against the previous completed publication. Execute queued
                // queries on this owner-thread safe point before the next tick replaces it.
                if (auto processed = physicsWorld_->ProcessQueryBatch(); processed.HasError()) {
                    if (gameplay_)
                        gameplay_->Shutdown();
                    return processed;
                }
                const auto stepped = physicsWorld_->AdvanceFixedTick(
                    {.simulationTick = ++localTick_, .sceneGeneration = record_->generation, .fixedDelta = context.fixedDelta});
                if (stepped.HasError() && gameplay_)
                    gameplay_->Shutdown();
                return stepped;
            }

            void Shutdown() noexcept override {
                RevokeGameplayContext();
                RetireGameplay();
                if (IsSceneOwner())
                    CloseRecord(*record_);
                DrainModuleRetirements();
                if (physicsWorld_)
                    physicsWorld_->Shutdown();
                physicsWorld_.reset();
                record_.reset();
            }

        private:
            bool IsSceneOwner() const noexcept {
                return record_ && owners_->configured && request_.service == Network::NetworkModeServiceKind::Scene;
            }

            void RevokeGameplayContext() noexcept {
                if (record_)
                    if (const auto context = record_->gameplay.lock())
                        context->Revoke();
            }

            void RetireGameplay() noexcept {
                if (IsSceneOwner() && record_->activeGameplay) {
                    record_->activeGameplay->Shutdown();
                    record_->activeGameplay = nullptr;
                }
                if (record_ && record_->activeGameplay == gameplay_.get())
                    record_->activeGameplay = nullptr;
                if (gameplay_)
                    gameplay_->Shutdown();
                gameplay_.reset();
            }

            void DrainModuleRetirements() noexcept {
                if (owners_->gameplay && owners_->gameplay->moduleRetirements)
                    (void)owners_->gameplay->moduleRetirements->Drain();
            }

            /** @brief Resolves admission only from the actual world role, retaining the complete owned project inventory. */
            Result<void> PrepareGameplaySelection() {
                selectedGameplay_ = owners_->gameplay;
                if ((request_.mode == Network::NetworkProjectRole::Standalone) !=
                    (request_.world == Network::NetworkModeWorldKind::Standalone))
                    return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
                if (request_.world == Network::NetworkModeWorldKind::Standalone) {
                    if (selectedGameplay_)
                        selectedGameplay_->replicationAdmission.reset();
                    return Result<void>::Success();
                }
                if (!owners_->replicationAdmission ||
                    owners_->replicationAdmission->inventory.completeness != Network::NetworkReplicationInventoryCompleteness::Complete)
                    return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
                const auto use = request_.world == Network::NetworkModeWorldKind::AuthorityServer
                                     ? Network::ReplicationDeclarationUse::ServerActivation
                                     : Network::ReplicationDeclarationUse::ClientActivation;
                if (request_.world != Network::NetworkModeWorldKind::AuthorityServer &&
                    request_.world != Network::NetworkModeWorldKind::Client)
                    return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
                auto policy = *owners_->replicationAdmission;
                if (policy.use != Network::ReplicationDeclarationUse::Count && policy.use != use)
                    return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
                policy.use = use;
                if (!selectedGameplay_)
                    return policy.RequireAdmission({}, {});
                selectedGameplay_->replicationAdmission = std::move(policy);
                return Result<void>::Success();
            }

            /** @brief Rejects foreign configured-host access before reading mutable record/native state. */
            Result<void> CheckOwnerLane() const {
                if (owners_->configured && std::this_thread::get_id() != owners_->owner)
                    return Result<void>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
                return Result<void>::Success();
            }

            /** @brief Creates one service at the exact request identity, before any derived gameplay activation. */
            Result<void> PrepareConfiguredScene() {
                if (record_->scenes)
                    return Result<void>::Failure(
                        MakeError(Network::NetworkErrors::NetworkModeInvalid, "Each world record admits exactly one Scene candidate."));
                const auto &input = *owners_->configured;
                const auto &boundary = input.load;
                record_->sceneCancellation = boundary.cancellation;
                record_->scenes = std::make_unique<Runtime::RuntimeSceneService>(input.registry, input.loads,
                                                                                 Runtime::RuntimeSceneAssetLimits{}, request_.scene);
                if (auto native = PrepareConfiguredNative(); native.HasError())
                    return native;
                if (auto ai = PrepareConfiguredAi(); ai.HasError())
                    return ai;
                if (auto animation = PrepareConfiguredAnimation(*owners_, *record_); animation.HasError())
                    return animation;
                if (auto prepared = PrepareHeadlessSceneAtLoadBoundary(*record_->scenes, *owners_->definition, boundary);
                    prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                return PrepareConfiguredTemplates();
            }

            Result<void> PrepareConfiguredNative() {
                const auto &input = *owners_->configured;
                auto participant = record_->native.MakeParticipant(*owners_->physics, owners_->authority, input.settings);
                if (participant.HasError())
                    return Result<void>::Failure(participant.ErrorValue());
                if (auto added = record_->scenes->AddActivationParticipant(std::move(participant).Value()); added.HasError())
                    return added;
                auto structural = record_->native.MakeStructuralParticipant();
                if (structural.HasError())
                    return Result<void>::Failure(structural.ErrorValue());
                if (auto added = record_->scenes->AddStructuralParticipant(std::move(structural).Value()); added.HasError())
                    return added;
                if (auto added = record_->scenes->AddStructuralParticipant(MakeGameplayStructuralParticipant(record_->activeGameplay));
                    added.HasError())
                    return added;
                return Result<void>::Success();
            }

            Result<void> PrepareConfiguredAi() {
                const auto &input = *owners_->configured;
                auto ai = AI::AiSceneRuntime::Create(input.aiSettings);
                if (ai.HasError())
                    return Result<void>::Failure(ai.ErrorValue());
                record_->ai = std::make_unique<AI::AiSceneRuntime>(std::move(ai).Value());
                if (auto added = record_->scenes->AddActivationParticipant(
                        std::make_unique<AI::AiSceneActivationParticipant>(*record_->ai, input.aiControllers));
                    added.HasError())
                    return added;
                if (auto added = record_->scenes->AddStructuralParticipant(record_->ai->MakeStructuralParticipant(input.aiControllers));
                    added.HasError())
                    return added;
                return Result<void>::Success();
            }

            Result<void> PrepareConfiguredTemplates() {
                const auto &input = *owners_->configured;
                const auto &boundary = input.load;
                if (owners_->gameplay && owners_->gameplay->prefabPermission == GameplayPrefabPermission::Granted) {
                    auto limits = Prefab::PrefabLimitProfile::Create(input.prefabPolicy);
                    if (limits.HasError())
                        return Result<void>::Failure(limits.ErrorValue());
                    record_->templates =
                        std::make_unique<Prefab::PrefabTemplateProvider>(input.registry, *input.prefabLoads, *record_->scenes,
                                                                         std::move(limits).Value(), Prefab::PrefabTemplateProviderLimits{},
                                                                         input.prefabMetrics);
                    return record_->templates->Startup(boundary.cancellation);
                }
                return Result<void>::Success();
            }

            /** @brief Admits gameplay from the already published private aggregate, without reactivating either native owner. */
            Result<void> ActivateConfiguredGameplay() {
                auto scope = record_->native.Borrow();
                if (scope.HasError())
                    return Result<void>::Failure(scope.ErrorValue());
                if (!selectedGameplay_)
                    return Result<void>::Success();
                auto gameplay =
                    record_->templates
                        ? GameplayWorldComposition::Create(*record_->scenes, *record_->templates, &scope.Value().physics,
                                                           request_.scene.value, *selectedGameplay_, nullptr, &scope.Value().character)
                        : GameplayWorldComposition::Create(*record_->scenes, &scope.Value().physics, request_.scene.value,
                                                           *selectedGameplay_, &scope.Value().character);
                if (gameplay.HasError())
                    return Result<void>::Failure(gameplay.ErrorValue());
                gameplay_ = std::move(gameplay).Value();
                record_->activeGameplay = gameplay_.get();
                record_->gameplay = gameplay_->PhysicsContext();
                return Result<void>::Success();
            }

            /** @brief Uses ordinary fixed stages only after readiness; load progress never dispatches a tick. */
            Result<void> RunConfiguredTick(const Runtime::FixedStepContext &context) {
                if (!configuredActivated_ || !record_ || record_->closeRequested)
                    return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
                if (gameplay_) {
                    if (const auto advanced = gameplay_->FixedTick(context); advanced.HasError())
                        return advanced;
                }
                if (record_->animation) {
                    auto animation = owners_->fixed->Evaluate(record_, context);
                    if (animation.HasError())
                        return animation;
                }
                auto advanced = record_->native.AdvanceFixedTick(context.fixedDelta);
                if (advanced.HasError() && gameplay_)
                    gameplay_->Shutdown();
                return advanced;
            }

            /** @brief Prepare only canonical Physics for this exact paired Scene; no fallback or ambient lookup. */
            Result<void> PreparePhysics() {
                if (auto prepared = owners_->PreparePhysicsRuntime(Physics::PhysicsRuntimeMode::Canonical); prepared.HasError())
                    return prepared;
                Physics::PhysicsWorldSettingsDescriptor descriptor;
                descriptor.world.capacity = {16, 32, 16, 4096};
                descriptor.budgets.maximumContactPairs = 32;
                descriptor.budgets.maximumContactConstraints = 16;
                descriptor.budgets.maximumInFlightPairs = 8;
                descriptor.budgets.scratchBytes = 1024 * 1024;
                auto settings = Physics::PhysicsWorldSettings::Capture(descriptor);
                if (settings.HasError())
                    return Result<void>::Failure(settings.ErrorValue());
                auto world = owners_->physics->PrepareWorld(settings.Value());
                if (world.HasError())
                    return Result<void>::Failure(world.ErrorValue());
                physicsWorld_ = std::move(world).Value();
                if (!record_ || !record_->scene)
                    return Result<void>::Failure(MakeError(Gameplay::GameplayErrors::PhysicsUnavailable));
                return Result<void>::Success();
            }

            std::shared_ptr<WorldOwners> owners_;
            std::optional<GameplayWorldSelection> selectedGameplay_;
            Network::NetworkModeServiceRequest request_;
            std::shared_ptr<WorldRecord> record_;
            std::unique_ptr<Physics::PhysicsWorld> physicsWorld_;
            std::unique_ptr<GameplayWorldComposition> gameplay_;
            std::uint64_t localTick_{};
            std::uint64_t phaseFrame_{};
            bool configuredActivated_{};
        };

        /** @brief Allocate one non-reused Scene activation epoch with a bounded unpaired-factory slot. */
        Result<std::shared_ptr<WorldRecord>> CreateRecord(WorldOwners &owners, const Network::NetworkModeServiceRequest &request) {
            const auto free = std::ranges::find_if(owners.unpaired, [](const auto &record) {
                return record.expired();
            });
            if (free == owners.unpaired.end() || owners.nextWorldGeneration == 0)
                return Result<std::shared_ptr<WorldRecord>>::Failure(MakeError(Physics::PhysicsErrors::GenerationExhausted));
            std::weak_ptr<WorldRecord> *knownSlot{};
            if (owners.configured) {
                if (!request.scene.IsValid())
                    return Result<std::shared_ptr<WorldRecord>>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
                for (auto &known : owners.known) {
                    const auto retained = known.lock();
                    if (retained && retained->request.scene == request.scene)
                        return Result<std::shared_ptr<WorldRecord>>::Failure(
                            MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                      "Scene identity is already reserved by a current, pending or retained world record."));
                    if (!retained && !knownSlot)
                        knownSlot = &known;
                }
                if (!knownSlot)
                    return Result<std::shared_ptr<WorldRecord>>::Failure(
                        MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                  "The bounded current/replacement role records have not retired."));
            }
            auto record = std::make_shared<WorldRecord>(request);
            record->request = request;
            record->generation = owners.nextWorldGeneration++;
            *free = record;
            if (knownSlot)
                *knownSlot = record;
            return Result<std::shared_ptr<WorldRecord>>::Success(std::move(record));
        }

        /** @brief Match the complete world-instance request, not merely its reusable Scene ID. */
        bool MatchesRequest(const std::shared_ptr<WorldRecord> &record, const Network::NetworkModeServiceRequest &request) {
            return record && record->request.world == request.world && record->request.scene == request.scene &&
                   record->request.mode == request.mode;
        }

        /** @brief Host factories borrow only their actual owner lane and cannot reopen retired native admission. */
        Result<void> ValidateFactoryOwner(const WorldOwners &owners) {
            if (std::this_thread::get_id() != owners.owner)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
            if (owners.retirementStarted)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
            return Result<void>::Success();
        }

        /** @brief Pair factories before preparation, preserving exact candidate ownership even for repeated Scene IDs. */
        [[nodiscard]] Result<std::shared_ptr<WorldRecord>> SelectRecord(WorldOwners &owners,
                                                                        const Network::NetworkModeServiceRequest &request) {
            if (auto valid = ValidateFactoryOwner(owners); valid.HasError())
                return Result<std::shared_ptr<WorldRecord>>::Failure(valid.ErrorValue());
            using Kind = Network::NetworkModeServiceKind;
            if (request.service != Kind::Scene && request.service != Kind::Physics)
                return Result<std::shared_ptr<WorldRecord>>::Success({});
            if (request.service == Kind::Scene)
                return CreateRecord(owners, request);
            std::shared_ptr<WorldRecord> selected;
            std::weak_ptr<WorldRecord> *slot{};
            for (auto &candidate : owners.unpaired) {
                auto record = candidate.lock();
                if (!MatchesRequest(record, request))
                    continue;
                if (selected)
                    return Result<std::shared_ptr<WorldRecord>>::Failure(
                        MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                  "Ambiguous unpaired Scene candidates; construct each Scene/Physics pair together."));
                selected = std::move(record);
                slot = &candidate;
            }
            if (!selected)
                return Result<std::shared_ptr<WorldRecord>>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Physics has no exact unpaired Scene candidate."));
            slot->reset();
            return Result<std::shared_ptr<WorldRecord>>::Success(std::move(selected));
        }
    }  // namespace

    namespace {
        /** @brief Registers the same exact pair factories for synthetic and explicit project compositions. */
        Network::NetworkModeFactories InstallWorldFactories(const std::shared_ptr<WorldOwners> &owners,
                                                            Network::NetworkModeFactories selectedFactories) {
            using enum Network::NetworkModeServiceKind;
            for (const auto service : {Scene, Physics, Replication}) {
                selectedFactories.services[static_cast<std::size_t>(service)] =
                    [owners](const Network::NetworkModeServiceRequest &request) {
                    auto record = SelectRecord(*owners, request);
                    if (record.HasError())
                        return Result<std::unique_ptr<Network::INetworkModeService>>::Failure(record.ErrorValue());
                    return Result<std::unique_ptr<Network::INetworkModeService>>::Success(
                        std::make_unique<WorldService>(owners, request, std::move(record).Value()));
                };
            }
            return selectedFactories;
        }
    }  // namespace

    Network::NetworkModeFactories ComposeHeadlessNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> sceneDefinition, Network::NetworkModeFactories selectedFactories,
        const std::optional<GameplayWorldSelection> &gameplay,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission, NetworkNativeRetirementPoll *retirement) {
        const auto owners = std::make_shared<WorldOwners>(std::move(sceneDefinition), gameplay, replicationAdmission);
        if (retirement)
            *retirement = [owners](std::uint32_t maximumRecords) {
                return owners->PollRetirement(maximumRecords);
            };
        return InstallWorldFactories(owners, std::move(selectedFactories));
    }

    namespace {
        Result<void> ValidateConfiguredInput(const std::shared_ptr<const Runtime::RuntimeSceneDefinition> &definition,
                                             const HeadlessSceneActivationInput &input,
                                             const std::optional<GameplayWorldSelection> &gameplay) {
            if (const auto policy = Prefab::PrefabLimitProfile::Create(input.prefabPolicy); policy.HasError())
                return Result<void>::Failure(policy.ErrorValue());
            if (!definition || input.load.deadline == std::chrono::steady_clock::time_point{} ||
                input.load.deadline == std::chrono::steady_clock::time_point::max())
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if (gameplay && gameplay->prefabPermission == GameplayPrefabPermission::Granted && !input.prefabLoads)
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Prefab grant requires an explicit committed artifact loader."));
            return Result<void>::Success();
        }

        Result<void> ValidateAnimationSelection(const HeadlessSceneActivationInput &input) {
            if (input.animationTarget.IsValid() != static_cast<bool>(input.animationIdentities))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if (input.animationInputs && !input.animationIdentities)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ComposeHeadlessSceneNetworkServices */
    Result<NetworkProductComposition> ComposeHeadlessSceneNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition, HeadlessSceneActivationInput input,
        Network::NetworkModeFactories selectedFactories, const std::optional<GameplayWorldSelection> &gameplay,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission) {
        if (auto valid = ValidateConfiguredInput(definition, input, gameplay); valid.HasError())
            return Result<NetworkProductComposition>::Failure(std::move(valid).ErrorValue());
        if (auto valid = ValidateAnimationSelection(input); valid.HasError())
            return Result<NetworkProductComposition>::Failure(std::move(valid).ErrorValue());
        auto owners = std::make_shared<WorldOwners>(std::move(definition), gameplay, replicationAdmission);
        if (auto prepared = owners->PreparePhysicsRuntime(input.mode); prepared.HasError())
            return Result<NetworkProductComposition>::Failure(prepared.ErrorValue());
        owners->configured.emplace(std::move(input));
        NetworkProductComposition composition{InstallWorldFactories(owners, std::move(selectedFactories)),
                                              {},
                                              [owners](std::uint32_t maximumRecords) {
            return owners->PollRetirement(maximumRecords);
        }};
        if (owners->configured->animationIdentities) {
            if (!owners->configured->animationTarget.IsValid())
                return Result<NetworkProductComposition>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            composition.fixedParticipant = [owners](Runtime::RuntimeDispatchSource source) {
                return HeadlessFixedParticipant::Create(owners, std::move(source));
            };
        }
        return Result<NetworkProductComposition>::Success(std::move(composition));
    }

    const Runtime::SceneAnimationRuntime *InspectHeadlessAnimation(const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->AnimationOwner() : nullptr;
    }

    std::optional<Runtime::EntityRef> InspectHeadlessAnimationEntity(const Network::INetworkModeService &service,
                                                                     Runtime::SceneObjectId object) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->AnimationEntity(object) : std::nullopt;
    }

    /** @copydoc InspectHeadlessGameplayContext */
    std::shared_ptr<const Gameplay::GameplayPhysicsContext> InspectHeadlessGameplayContext(
        const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->PhysicsContext() : nullptr;
    }
}  // namespace Horo::Application::Internal
