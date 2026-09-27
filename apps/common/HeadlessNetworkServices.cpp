#include "HeadlessNetworkServices.h"

#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <optional>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        struct WorldOwners final {
            explicit WorldOwners(std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition) : definition(std::move(definition)) {}

            ~WorldOwners() {
                if (physics != nullptr)
                    physics->Shutdown();
            }

            std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition;
            std::unique_ptr<Physics::PhysicsRuntime> physics;
        };

        class WorldService final : public Network::INetworkModeService {
        public:
            WorldService(std::shared_ptr<WorldOwners> owners, const Network::NetworkModeServiceRequest &request)
                : owners_(std::move(owners)), request_(request) {}

            Result<void> Prepare() override {
                switch (request_.service) {
                    case Network::NetworkModeServiceKind::Scene: {
                        if (owners_->definition == nullptr)
                            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeUnavailable,
                                                                   "No cooked runtime Scene definition was supplied."));
                        auto scene = Runtime::RuntimeScene::Create(*owners_->definition, request_.scene);
                        if (scene.HasError())
                            return Result<void>::Failure(scene.ErrorValue());
                        scene_ = std::move(scene).Value();
                        return Result<void>::Success();
                    }
                    case Network::NetworkModeServiceKind::Physics: {
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
                        return Result<void>::Success();
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
                auto identity = Physics::PhysicsWorldId::Create(request_.scene.value);
                if (identity.HasError())
                    return Result<void>::Failure(identity.ErrorValue());
                return physicsWorld_->Activate(identity.Value());
            }

            Result<void> RunPhase(Runtime::RuntimePhase) override {
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &context) override {
                if (request_.service != Network::NetworkModeServiceKind::Physics)
                    return Result<void>::Success();
                return physicsWorld_->AdvanceFixedTick(
                    {.simulationTick = ++localTick_, .sceneGeneration = request_.scene.value, .fixedDelta = context.fixedDelta});
            }

            void Shutdown() noexcept override {
                if (replication_.has_value())
                    replication_->BeginShutdown();
                if (physicsWorld_ != nullptr)
                    physicsWorld_->Shutdown();
                physicsWorld_.reset();
                scene_.reset();
                replication_.reset();
            }

        private:
            std::shared_ptr<WorldOwners> owners_;
            Network::NetworkModeServiceRequest request_;
            std::unique_ptr<Runtime::RuntimeScene> scene_;
            std::unique_ptr<Physics::PhysicsWorld> physicsWorld_;
            std::optional<Network::ReplicationWorldLifecycle> replication_;
            std::uint64_t localTick_{};
        };
    }  // namespace

    Network::NetworkModeFactories ComposeHeadlessNetworkServices(std::shared_ptr<const Runtime::RuntimeSceneDefinition> sceneDefinition,
                                                                 Network::NetworkModeFactories selectedFactories) {
        const auto owners = std::make_shared<WorldOwners>(std::move(sceneDefinition));
        using enum Network::NetworkModeServiceKind;
        for (const auto service : {Scene, Physics, Replication}) {
            selectedFactories.services[static_cast<std::size_t>(service)] = [owners](const Network::NetworkModeServiceRequest &request) {
                return Result<std::unique_ptr<Network::INetworkModeService>>::Success(std::make_unique<WorldService>(owners, request));
            };
        }
        return selectedFactories;
    }
}  // namespace Horo::Application::Internal
