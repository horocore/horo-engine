#pragma once
#include "../network/NetworkModeHostTestSupport.h"
#include "AnimationGraphInstanceTestFixtures.h"
#include "CharacterSceneProductFixture.h"
#include "CommittedSceneAssets.h"
#include "Horo/Animation/AnimationAssetSource.h"
#include "Horo/Animation/AnimationGraphSource.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace Horo::Network::AnimationProduct {
    namespace App = Application::Internal;

    template <typename T> T Take(Result<T> value) {
        INFO((value.HasError() ? value.ErrorValue().code.Value() + ": " + value.ErrorValue().message : "success"));
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    struct Inputs final : Runtime::SceneAnimationInputSource {
        mutable std::size_t calls{};
        std::size_t badAt{};

        Runtime::SceneAnimationInputSnapshot Read(Runtime::EntityRef, const Runtime::FixedStepContext &) const noexcept override {
            const auto call = ++calls;
            return {.playbackRate = {call == badAt ? std::numeric_limits<std::int32_t>::max() : 1'000'000}};
        }
    };

    struct Trace final {
        struct Entry {
            const INetworkModeService *service{};
            Runtime::SceneRuntimeId scene;
            bool live{};
        };

        std::array<Entry, 4> entries{};
        std::size_t count{};
        bool failAfterTick{};
        Runtime::SceneRuntimeId failScene;
        Runtime::SceneRuntimeId expectedScene{71};
        bool cancelAfterTick{};
        bool closeAfterTick{};
        bool closeDeferred{};
        bool closeAtPrepare{};
        bool observeCommitted{};
        std::optional<Animation::PoseHandle> observedCommitted;
        INetworkModeService *sceneFacade{};
        bool observedOldPublication{};
        std::optional<Animation::PoseHandle> expected;
        App::NetworkProductHost *host{};

        const Runtime::SceneAnimationRuntime *Owner(Runtime::SceneRuntimeId selected = {}) const {
            if (selected.IsValid()) {
                for (const auto &entry : entries)
                    if (entry.live && entry.scene == selected)
                        return App::InspectHeadlessAnimation(*entry.service);
                return nullptr;
            }
            for (const auto &entry : entries)
                if (entry.live)
                    return App::InspectHeadlessAnimation(*entry.service);
            return nullptr;
        }

        Runtime::EntityRef Entity() const {
            for (const auto &entry : entries)
                if (entry.live) {
                    const auto entity = App::InspectHeadlessAnimationEntity(*entry.service, {3});
                    REQUIRE(entity);
                    return *entity;
                }
            FAIL("No actual published headless world");
            return {};
        }

        Runtime::SceneAnimationObservation Observe() const {
            const auto *owner = Owner();
            REQUIRE(owner);
            return Take(owner->Query(Entity()));
        }
    };

    class ObservedWorld final : public INetworkModeService {
    public:
        ObservedWorld(std::unique_ptr<INetworkModeService> service, std::shared_ptr<Trace> trace, Runtime::SceneRuntimeId scene)
            : service_(std::move(service)), trace_(std::move(trace)), index_(trace_->count++) {
            REQUIRE(index_ < trace_->entries.size());
            trace_->entries[index_] = {service_.get(), scene, true};
        }

        Result<void> Prepare() override {
            return service_->Prepare();
        }

        Result<void> Activate() override {
            return service_->Activate();
        }

        Result<void> RunPhase(Runtime::RuntimePhase phase) override {
            return service_->RunPhase(phase);
        }

        Result<void> RunFixedTick(const Runtime::FixedStepContext &context) override {
            auto result = service_->RunFixedTick(context);
            CaptureOldPublication();
            if (result.HasError())
                return result;
            CloseRequestedScene();
            if (trace_->cancelAfterTick)
                trace_->host->RequestShutdown();
            if (FailThisWorld())
                return Result<void>::Failure({ErrorCode{"test.headless.native_after_tick"},
                                              ErrorDomainId{"test.headless"},
                                              ErrorSeverity::Error,
                                              "Injected error after the actual native fixed callback.",
                                              {}});
            return result;
        }

        void Shutdown() noexcept override {
            service_->Shutdown();
            trace_->entries[index_].live = false;
        }

    private:
        void CaptureOldPublication() {
            if (!trace_->expected || trace_->entries[index_].scene != trace_->expectedScene)
                return;
            const auto entity = App::InspectHeadlessAnimationEntity(*service_, {3});
            REQUIRE(entity);
            const auto *owner = App::InspectHeadlessAnimation(*service_);
            REQUIRE(owner);
            const auto observed = owner->Query(*entity);
            trace_->observedOldPublication =
                observed.HasValue() && observed.Value().component && observed.Value().component->currentPose == *trace_->expected;
        }

        void CloseRequestedScene() {
            if (!std::exchange(trace_->closeAfterTick, false))
                return;
            trace_->sceneFacade->Shutdown();
            const auto *owner = App::InspectHeadlessAnimation(*service_);
            const auto entity = App::InspectHeadlessAnimationEntity(*service_, {3});
            trace_->closeDeferred = owner && entity && owner->Query(*entity).HasValue();
        }

        bool FailThisWorld() noexcept {
            if (trace_->failScene.IsValid() && trace_->failScene != trace_->entries[index_].scene)
                return false;
            return std::exchange(trace_->failAfterTick, false);
        }

        std::unique_ptr<INetworkModeService> service_;
        std::shared_ptr<Trace> trace_;
        std::size_t index_{};
    };

    class ObservedFixed final : public Runtime::RuntimeLifecycleParticipant {
    public:
        ObservedFixed(std::unique_ptr<Runtime::RuntimeLifecycleParticipant> owner, std::shared_ptr<Trace> trace)
            : owner_(std::move(owner)), trace_(std::move(trace)) {}

        Result<void> Startup(const CancellationToken &token) override {
            return owner_->Startup(token);
        }

        Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
            return owner_->OnPhase(phase, context);
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
            return owner_->OnFixedUpdate(context);
        }

        Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &context) override {
            auto prepared = owner_->PrepareFixedCommit(context);
            if (prepared.HasValue() && std::exchange(trace_->closeAtPrepare, false))
                trace_->sceneFacade->Shutdown();
            return prepared;
        }

        void CommitFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            owner_->CommitFixedUpdate(context);
            if (trace_->observeCommitted) {
                const auto *adapter = trace_->Owner();
                const auto observed = adapter->Query(*App::InspectHeadlessAnimationEntity(*trace_->entries[0].service, {3}));
                if (observed.HasValue() && observed.Value().component)
                    trace_->observedCommitted = observed.Value().component->currentPose;
            }
        }

        void AbortFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            owner_->AbortFixedUpdate(context);
        }

        void Shutdown() noexcept override {
            owner_->Shutdown();
        }

    private:
        std::unique_ptr<Runtime::RuntimeLifecycleParticipant> owner_;
        std::shared_ptr<Trace> trace_;
    };

    struct Fixture final {
        Application::CharacterSceneProductTest::Fixture project;
        bool listenPair{};
        std::shared_ptr<ProductOwners> productOwners = std::make_shared<ProductOwners>();
        Animation::Test::Instance::Prepared native;
        std::shared_ptr<Inputs> inputs = std::make_shared<Inputs>();
        std::shared_ptr<Trace> trace = std::make_shared<Trace>();
        std::shared_ptr<App::AnimationRuntimeIdentitySource> identities =
            std::make_shared<App::AnimationRuntimeIdentitySource>(App::AnimationRuntimeIdentityRange{1001, 1100});
        std::unique_ptr<App::CommittedSceneAssets> committed;
        std::unique_ptr<Assets::AssetLoadService> loads;
        CancellationSource lifetime;
        App::HeadlessSceneLoadBoundary load{lifetime.Token(), std::chrono::steady_clock::now() + std::chrono::seconds(10)};
        DeterministicClock clock;
        std::unique_ptr<App::NetworkProductHost> host;
        App::NetworkFixedParticipantFactory retainedFactory;

        explicit Fixture(bool directClip = false, bool pair = false) : listenPair(pair) {
            WriteAnimation(directClip);
            project.Cook();
            committed = Take(App::CommittedSceneAssets::Capture(project.project.files,
                                                                {project.project.cooked.path, project.project.request.assets.target},
                                                                project.project.registry.Snapshot()));
            loads = std::make_unique<Assets::AssetLoadService>(project.project.jobs, committed->SceneProvider());
            // The Scene admission deadline starts after cold asset cooking, not before fixture setup.
            load.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
            auto composition = Compose();
            retainedFactory = composition.fixedParticipant;
            composition.fixedParticipant =
                [original = std::move(composition.fixedParticipant),
                 trace = trace](Runtime::RuntimeDispatchSource source) -> Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>> {
                auto owner = original(std::move(source));
                if (owner.HasError())
                    return owner;
                return Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>>::Success(
                    std::make_unique<ObservedFixed>(std::move(owner).Value(), trace));
            };
            ObserveWorlds(composition.factories);
            CreateHost(std::move(composition));
        }

        ~Fixture() {
            if (host)
                host->Shutdown();
            if (loads)
                loads->Shutdown();
        }

        void Add(std::vector<Assets::AssetRecord> &records, std::string_view name, Assets::AssetId id, std::string_view type,
                 const std::string &source) {
            const auto path = "assets/" + std::string(name) + ".horoasset";
            project.project.WriteText(path, source);
            project.project.WriteText(path + ".horo", Application::PrefabCookTestSupport::SidecarJson(id.ToString(), type));
            records.push_back(Application::CharacterSceneProductTest::Fixture::Record(id, type, path));
        }

        std::string ClipSource() const {
            auto data = native.compileClips.front().Data();
            data.descriptor.duration = {1'000'000'000};
            data.tracks.front().keys.back().time = {1'000'000'000};
            auto clip = Take(Animation::AnimationClipAsset::Create(std::move(data), *native.skeleton));
            return Take(Animation::SerializeClipAnimationSource(clip));
        }

        void WriteAnimation(bool directClip) {
            auto records = std::vector<Assets::AssetRecord>(project.project.registry.Snapshot().Records().begin(),
                                                            project.project.registry.Snapshot().Records().end());
            Add(records, "skeleton", native.skeleton->Data().skeleton.Asset(), "core.animation.skeleton",
                Take(Animation::SerializeSkeletonAnimationSource(*native.skeleton)));
            Add(records, "clip", native.compileClips.front().Data().descriptor.id.Asset(), "core.animation.clip", ClipSource());
            Add(records, "graph", native.program->Id().Asset(), "core.animation.graph",
                Take(Animation::SerializeAnimationGraphSource(Animation::Test::Graph::Simple())));
            REQUIRE(project.project.registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            Animation::AnimationAuthoringComponent binding;
            binding.component = Animation::Test::Id<Animation::AnimationComponentId>(7);
            binding.skeleton = native.skeleton->Data().skeleton;
            binding.source.kind = directClip ? Animation::AnimationSourceKind::Clip : Animation::AnimationSourceKind::Graph;
            if (directClip)
                binding.source.clip = native.compileClips.front().Data().descriptor.id;
            else
                binding.source.graph = native.program->Id();
            project.source.objects.push_back({.id = {3}, .name = "Real animated actor A", .components = {.animation = binding}});
            binding.component = Animation::Test::Id<Animation::AnimationComponentId>(8);
            project.source.objects.push_back({.id = {4}, .name = "Real animated actor B", .components = {.animation = binding}});
            project.project.WriteText("assets/level.horo",
                                      SceneSource::EncodeSceneSource({project.source.objects, project.source.prefabInstances}));
        }

        App::NetworkProductComposition Compose(Physics::PhysicsRuntimeMode physicsMode = Physics::PhysicsRuntimeMode::Canonical) {
            auto collision = Take(Application::LoadProjectCollisionSettings(project.project.project.dir.path, "p1"));
            auto simulation = Take(collision.settings->Capture(1));
            auto physics = Take(Physics::PhysicsWorldSettings::Capture({}));
            auto character = Take(Character::CharacterWorldSettings::Capture({}));
            auto definition =
                std::make_shared<const Runtime::RuntimeSceneDefinition>(Take(committed->DecodeScene(project.project.sceneId)));
            App::HeadlessSceneActivationInput input{physicsMode,
                                                    {std::move(physics), std::move(character), std::move(simulation)},
                                                    project.project.registry,
                                                    *loads,
                                                    load};
            input.animationTarget = project.project.request.assets.target;
            input.animationIdentities = identities;
            input.animationInputs = inputs;
            std::optional<Gameplay::GameplayReplicationAdmissionPolicy> admission;
            if (listenPair)
                admission = Gameplay::GameplayReplicationAdmissionPolicy{
                    .inventory = {.completeness = NetworkReplicationInventoryCompleteness::Complete}};
            return Take(App::ComposeHeadlessSceneNetworkServices(std::move(definition), std::move(input),
                                                                 listenPair ? ProductFactories(productOwners) : NetworkModeFactories{},
                                                                 std::nullopt, admission));
        }

        void ObserveWorlds(NetworkModeFactories &factories) {
            auto &scene = factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)];
            scene = [original = std::move(scene), trace = trace](const NetworkModeServiceRequest &request) {
                auto created = original(request);
                if (created.HasValue())
                    trace->sceneFacade = created.Value().get();
                return created;
            };
            auto &factory = factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Physics)];
            factory = [original = std::move(factory),
                       trace = trace](const NetworkModeServiceRequest &request) -> Result<std::unique_ptr<INetworkModeService>> {
                auto result = original(request);
                if (result.HasError())
                    return result;
                return Result<std::unique_ptr<INetworkModeService>>::Success(
                    std::make_unique<ObservedWorld>(std::move(result).Value(), trace, request.scene));
            };
        }

        void CreateHost(App::NetworkProductComposition composition) {
            auto settings = Take(DefaultNetworkProjectSettings(Id<NetworkProjectSettingsId>(10)));
            settings.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::ListenServer;
            settings.replication.completeness = NetworkReplicationInventoryCompleteness::Complete;
            auto projectSettings = Take(NetworkProjectSettings::Create(settings));
            const auto mode = listenPair ? NetworkProjectRole::ListenServer : NetworkProjectRole::Standalone;
            const auto facts = ProductFacts(projectSettings, mode, false);
            const std::array worlds =
                listenPair ? std::array{World(NetworkModeWorldKind::AuthorityServer, 71), World(NetworkModeWorldKind::Client, 72)}
                           : std::array{World(NetworkModeWorldKind::Standalone, 71), NetworkModeWorld{}};
            const auto selected = std::span{worlds}.first(listenPair ? 2 : 1);
            host = Take(App::NetworkProductHost::Create(clock,
                                                        {projectSettings,
                                                         facts.product,
                                                         facts.inventory,
                                                         facts.host,
                                                         facts.requirements,
                                                         facts.selection,
                                                         6,
                                                         selected,
                                                         {.localPlayer = listenPair},
                                                         listenPair ? productOwners->sessions : nullptr,
                                                         listenPair ? NetworkWorldCompositionPurpose::TransportQualification
                                                                    : NetworkWorldCompositionPurpose::Gameplay,
                                                         {},
                                                         std::move(composition.fixedParticipant)},
                                                        std::move(composition.factories)));
            trace->host = host.get();
            REQUIRE(host->Startup().HasValue());
            REQUIRE(host->RunFrame().HasValue());
        }

        Result<void> Tick() {
            inputs->calls = 0;
            clock.Advance(Duration::FromNanoseconds(16'666'667));
            return host->RunFrame();
        }
    };
}  // namespace Horo::Network::AnimationProduct
