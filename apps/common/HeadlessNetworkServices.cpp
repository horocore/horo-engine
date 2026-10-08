#include "HeadlessNetworkServices.h"

#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        /** @brief One generation-owned Scene/Physics pair; equal Scene IDs never imply shared storage. */
        struct WorldRecord final {
            Network::NetworkModeServiceRequest request;
            std::uint64_t generation{};
            std::shared_ptr<Runtime::RuntimeScene> scene;
            std::weak_ptr<const Gameplay::GameplayPhysicsContext> gameplay;
        };

        struct WorldOwners final {
            WorldOwners(std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition,
                        const std::optional<GameplayWorldSelection> &gameplay)
                : definition(std::move(definition)), gameplay(gameplay) {}

            ~WorldOwners() {
                if (physics != nullptr)
                    physics->Shutdown();
            }

            std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition;
            std::unique_ptr<Physics::PhysicsRuntime> physics;
            std::optional<GameplayWorldSelection> gameplay;
            // Factory calls are serialized on the host owner thread. Scene is immediately followed
            // by its Physics factory in NetworkModeComposition, including travel candidates.
            std::array<std::weak_ptr<WorldRecord>, 4> unpaired;
            std::uint64_t nextWorldGeneration{1};
        };

        class WorldService final : public Network::INetworkModeService {
        public:
            [[nodiscard]] std::shared_ptr<const Gameplay::GameplayPhysicsContext> PhysicsContext() const noexcept {
                return gameplay_ ? gameplay_->PhysicsContext() : nullptr;
            }

            WorldService(std::shared_ptr<WorldOwners> owners, const Network::NetworkModeServiceRequest &request,
                         std::shared_ptr<WorldRecord> record)
                : owners_(std::move(owners)), request_(request), record_(std::move(record)) {}

            Result<void> Prepare() override {
                switch (request_.service) {
                    case Network::NetworkModeServiceKind::Scene: {
                        if (owners_->definition == nullptr)
                            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeUnavailable,
                                                                   "No cooked runtime Scene definition was supplied."));
                        auto scene = Runtime::RuntimeScene::Create(*owners_->definition, request_.scene);
                        if (scene.HasError())
                            return Result<void>::Failure(scene.ErrorValue());
                        record_->scene = std::move(scene).Value();
                        return Result<void>::Success();
                    }
                    case Network::NetworkModeServiceKind::Physics: {
                        return PreparePhysics();
                    }
                    case Network::NetworkModeServiceKind::Replication: {
                        auto lifecycle = Network::ReplicationWorldLifecycle::Create();
                        if (lifecycle.HasError())
                            return Result<void>::Failure(lifecycle.ErrorValue());
                        replication_.emplace(std::move(lifecycle).Value());
                        return Result<void>::Success();
                    }
                    default:
                        return Result<void>::Failure(
                            MakeError(Network::NetworkErrors::NetworkModeInvalid, "Headless world factory received a non-world service."));
                }
            }

            Result<void> Activate() override {
                if (request_.service != Network::NetworkModeServiceKind::Physics)
                    return Result<void>::Success();
                auto identity = owners_->physics->IssueWorldIdentity();
                if (identity.HasError())
                    return Result<void>::Failure(identity.ErrorValue());
                if (auto activated = physicsWorld_->Activate(identity.Value()); activated.HasError())
                    return activated;
                if (owners_->gameplay) {
                    auto gameplay =
                        GameplayWorldComposition::Create(*record_->scene, physicsWorld_.get(), record_->generation, *owners_->gameplay);
                    if (gameplay.HasError())
                        return Result<void>::Failure(gameplay.ErrorValue());
                    gameplay_ = std::move(gameplay).Value();
                    record_->gameplay = gameplay_->PhysicsContext();
                }
                return Result<void>::Success();
            }

            Result<void> RunPhase(Runtime::RuntimePhase) override {
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &context) override {
                if (request_.service != Network::NetworkModeServiceKind::Physics)
                    return Result<void>::Success();
                if (gameplay_) {
                    if (auto advanced = gameplay_->FixedUpdate({static_cast<double>(context.fixedDelta.ToNanoseconds()) / 1'000'000'000.0});
                        advanced.HasError())
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
                if (record_) {
                    if (const auto context = record_->gameplay.lock())
                        context->Revoke();
                }
                if (gameplay_)
                    gameplay_->Shutdown();
                gameplay_.reset();
                if (replication_.has_value())
                    replication_->BeginShutdown();
                if (physicsWorld_ != nullptr)
                    physicsWorld_->Shutdown();
                physicsWorld_.reset();
                record_.reset();
                replication_.reset();
            }

        private:
            /** @brief Prepare only canonical Physics for this exact paired Scene; no fallback or ambient lookup. */
            Result<void> PreparePhysics() {
                if (owners_->physics == nullptr) {
                    auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical);
                    if (runtime.HasError())
                        return Result<void>::Failure(runtime.ErrorValue());
                    owners_->physics = std::move(runtime).Value();
                }
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
            Network::NetworkModeServiceRequest request_;
            std::shared_ptr<WorldRecord> record_;
            std::unique_ptr<Physics::PhysicsWorld> physicsWorld_;
            std::unique_ptr<GameplayWorldComposition> gameplay_;
            std::optional<Network::ReplicationWorldLifecycle> replication_;
            std::uint64_t localTick_{};
        };

        /** @brief Allocate one non-reused Scene activation epoch with a bounded unpaired-factory slot. */
        Result<std::shared_ptr<WorldRecord>> CreateRecord(WorldOwners &owners, const Network::NetworkModeServiceRequest &request) {
            const auto free = std::ranges::find_if(owners.unpaired, [](const auto &record) {
                return record.expired();
            });
            if (free == owners.unpaired.end() || owners.nextWorldGeneration == 0)
                return Result<std::shared_ptr<WorldRecord>>::Failure(MakeError(Physics::PhysicsErrors::GenerationExhausted));
            auto record = std::make_shared<WorldRecord>();
            record->request = request;
            record->generation = owners.nextWorldGeneration++;
            *free = record;
            return Result<std::shared_ptr<WorldRecord>>::Success(std::move(record));
        }

        /** @brief Match the complete world-instance request, not merely its reusable Scene ID. */
        bool MatchesRequest(const std::shared_ptr<WorldRecord> &record, const Network::NetworkModeServiceRequest &request) {
            return record && record->request.world == request.world && record->request.scene == request.scene &&
                   record->request.mode == request.mode;
        }

        /** @brief Pair factories before preparation, preserving exact candidate ownership even for repeated Scene IDs. */
        [[nodiscard]] Result<std::shared_ptr<WorldRecord>> SelectRecord(WorldOwners &owners,
                                                                        const Network::NetworkModeServiceRequest &request) {
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

    Network::NetworkModeFactories ComposeHeadlessNetworkServices(std::shared_ptr<const Runtime::RuntimeSceneDefinition> sceneDefinition,
                                                                 Network::NetworkModeFactories selectedFactories,
                                                                 const std::optional<GameplayWorldSelection> &gameplay) {
        const auto owners = std::make_shared<WorldOwners>(std::move(sceneDefinition), gameplay);
        using enum Network::NetworkModeServiceKind;
        for (const auto service : {Scene, Physics, Replication}) {
            selectedFactories.services[static_cast<std::size_t>(service)] = [owners](const Network::NetworkModeServiceRequest &request) {
                auto record = SelectRecord(*owners, request);
                if (record.HasError())
                    return Result<std::unique_ptr<Network::INetworkModeService>>::Failure(record.ErrorValue());
                return Result<std::unique_ptr<Network::INetworkModeService>>::Success(
                    std::make_unique<WorldService>(owners, request, std::move(record).Value()));
            };
        }
        return selectedFactories;
    }

    /** @copydoc InspectHeadlessGameplayContext */
    std::shared_ptr<const Gameplay::GameplayPhysicsContext> InspectHeadlessGameplayContext(
        const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->PhysicsContext() : nullptr;
    }
}  // namespace Horo::Application::Internal
