#include "../../integration/network/NetworkModeHostTestSupport.h"
#include "../runtime/physics/PhysicsSceneActivationTestSupport.h"
#include "HeadlessSceneLoadBoundary.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "MountedWorldHost.h"
#include "WorldCookPublicationTestSupport.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <iostream>
#include <thread>

namespace Horo::WorldCook {
    namespace {
        using namespace Application::Internal;
        using namespace PublicationTestSupport;
        using Stage = MountedWorldLiveCellState;

        /** @brief Runs the real mount and Scene cutoff in their ordinary owner order under scheduler-issued evidence. */
        class MountedCellDriver final : public S::RuntimeLifecycleParticipant {
        public:
            MountedCellDriver(MountedWorld &world, S::RuntimeSceneService &scene) : world_(world), scene_(scene) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {}

            Result<void> OnFixedUpdate(const S::FixedStepContext &context) override {
                return scene_.OnFixedUpdate(context);
            }

            Result<void> OnPhase(const S::RuntimePhase phase, const S::FrameContext &context) override {
                if (phase == S::RuntimePhase::CommitDeferredLifecycleChanges) {
                    last = world_.AdvanceLiveCell(context.dispatchEvidence);
                    if (probeDuplicate) {
                        probeDuplicate = false;
                        duplicate = world_.AdvanceLiveCell(context.dispatchEvidence);
                    }
                }
                return scene_.OnPhase(phase, context);
            }

            Result<Stage> last{Result<Stage>::Success(Stage::Idle)};
            std::optional<Result<Stage>> duplicate;
            bool probeDuplicate{};

        private:
            MountedWorld &world_;
            S::RuntimeSceneService &scene_;
        };

        /** @brief Composes the ordinary standalone product over a durable cooked World and observes its exact Scene facade. */
        class MountedCellProduct final {
        public:
            explicit MountedCellProduct(const bool enableLive, const bool firstDemand) {
                namespace N = Network;
                const auto published = fixture_.Publish();
                REQUIRE(published.HasValue());
                REQUIRE(published.Value().publication);
                const auto release = fixture_.Release();
                identity_ = fixture_.inputs.front().expected;
                demand_ = {identity_.cell, {900}, identity_.scene, identity_.revision};
                CommittedWorldSelection selection{release.targetRoot,
                                                  release.target,
                                                  published.Value().publication->manifestDigest,
                                                  release.indexArtifact,
                                                  release.dependencyArtifact,
                                                  Math::WorldCoordinate64::FromMillimeters(0, 0, 0)};
                if (enableLive)
                    selection.liveCell =
                        CommittedWorldLiveSelection{{20'000'000, 1'000'000, 4}, firstDemand ? std::optional{demand_} : std::nullopt};
                S::SceneDefinitionBuilder root{{99}, {1}};
                root.Add({.object = parentObject_});
                auto built = std::move(root).Build();
                REQUIRE(built.HasValue());
                HeadlessSceneActivationInput input{.mode = Physics::PhysicsRuntimeMode::Null,
                                                   .settings = Physics::Settings(),
                                                   .registry = registry_,
                                                   .loads = loads_,
                                                   .load = load_,
                                                   .mountedWorld = std::make_shared<const CommittedWorldSelection>(std::move(selection)),
                                                   .worldJobs = &jobs_};
                auto composition =
                    ComposeHeadlessSceneNetworkServices(std::make_shared<const S::RuntimeSceneDefinition>(std::move(built).Value()),
                                                        std::move(input));
                REQUIRE(composition.HasValue());
                auto services = std::move(composition).Value();
                auto &scene = services.factories.services[static_cast<std::size_t>(N::NetworkModeServiceKind::Scene)];
                scene = [factory = std::move(scene), this](const N::NetworkModeServiceRequest &request) {
                    auto owned = factory(request);
                    if (owned.HasValue())
                        scene_ = owned.Value().get();
                    return owned;
                };

                auto settings = N::DefaultNetworkProjectSettings(N::Id<N::NetworkProjectSettingsId>(10));
                REQUIRE(settings.HasValue());
                auto project = N::NetworkProjectSettings::Create(settings.Value());
                REQUIRE(project.HasValue());
                constexpr auto role = N::NetworkProjectRole::Standalone;
                const auto facts = N::ProductFacts(project.Value(), role, false);
                const std::array worlds{N::World(N::NetworkModeWorldKind::Standalone, runtime_.value)};
                const auto inputs = NetworkProductHostInputs{.project = project.Value(),
                                                             .product = facts.product,
                                                             .inventory = facts.inventory,
                                                             .hostFacts = facts.host,
                                                             .requirements = facts.requirements,
                                                             .selection = facts.selection,
                                                             .hostGeneration = 5,
                                                             .worlds = worlds,
                                                             .fixedParticipant = std::move(services.fixedParticipant),
                                                             .pollRetirement = std::move(services.pollRetirement)};
                auto created = NetworkProductHost::Create(clock_, inputs, std::move(services.factories));
                REQUIRE(created.HasValue());
                host_ = std::move(created).Value();
                Check(host_->Startup());
                REQUIRE(scene_);
                const auto origin = InspectHeadlessWorldOrigin(*scene_);
                REQUIRE(origin);
                const auto frame = origin->Get();
                REQUIRE(frame.HasValue());
                epoch_ = IdentityFrom<W::PartitionEpoch>(frame.Value().Binding().identity.Value());
            }

            void Request() {
                Check(RequestHeadlessWorldCell(*scene_, demand_));
            }

            void Withdraw() {
                Check(WithdrawHeadlessWorldCell(*scene_));
            }

            Result<void> ProbeRequest() const {
                return RequestHeadlessWorldCell(*scene_, demand_);
            }

            void CheckTravelRefusal() {
                const auto *original = scene_;
                const std::array replacements{Network::World(Network::NetworkModeWorldKind::Standalone, runtime_.value + 1)};
                Check(host_->RequestTravel(replacements));
                static_cast<void>(RunFrame());
                const auto rejected = host_->TakeTravelFailure();
                REQUIRE(rejected);
                INFO(rejected->message);
                CHECK(rejected->code.Value() == W::WorldArtifactErrors::Unsupported.code.Value());
                REQUIRE(scene_ == original);
                REQUIRE(host_->Role(Network::NetworkModeWorldKind::Standalone, runtime_).HasValue());
                REQUIRE(host_->Role(Network::NetworkModeWorldKind::Standalone, replacements.front().scene).HasError());
            }

            bool Attached() const {
                const auto scene = InspectHeadlessScene(*scene_);
                REQUIRE(scene);
                REQUIRE(scene->RuntimeId() == runtime_);
                REQUIRE(scene->Find(parentObject_));
                return S::FindStreamedSceneInstance(*scene,
                                                    {runtime_, demand_.occurrence, demand_.definition, S::SceneInstanceMode::Streamed},
                                                    identity_, epoch_)
                    .has_value();
            }

            std::int64_t RunFrame() {
                clock_.Advance(Duration::FromMilliseconds(17));
                const auto start = std::chrono::steady_clock::now();
                const auto advanced = host_->RunFrame();
                const auto elapsed = std::chrono::steady_clock::now() - start;
                Check(advanced);
                return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
            }

            std::int64_t UntilAttached(const bool expected) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do {
                    const auto sample = RunFrame();
                    if (Attached() == expected)
                        return sample;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (std::chrono::steady_clock::now() < deadline);
                FAIL("Standalone mounted frame did not publish the expected residency before the fixture deadline");
                return 0;
            }

        private:
            static void Check(const Result<void> &result) {
                if (result.HasError())
                    UNSCOPED_INFO(result.ErrorValue().message);
                REQUIRE(result.HasValue());
            }

            static constexpr S::SceneRuntimeId runtime_{71};
            static constexpr S::SceneObjectId parentObject_{42};
            Fixture fixture_{false, false};
            S::SceneCellPayloadIdentity identity_;
            W::PartitionEpoch epoch_;
            CommittedWorldInitialCell demand_;
            DeterministicClock clock_;
            JobSystem jobs_{{1, 4}};
            A::MemoryAssetProvider provider_;
            A::AssetRegistry registry_;
            A::AssetLoadService loads_{jobs_, provider_};
            HeadlessSceneLoadBoundary load_{{}, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
            const Network::INetworkModeService *scene_{};
            std::unique_ptr<NetworkProductHost> host_;
        };

        /** @brief Prints observed frame quantiles without filtering out outliers or imposing machine-specific thresholds. */
        void PrintFrameSamples(const std::string_view stage, std::span<std::int64_t> samples) {
            std::ranges::sort(samples);
            const auto percentile = [&](const std::size_t percent) {
                return samples[(percent * samples.size() + 99) / 100 - 1];
            };
            std::cout << stage << ",samples=" << samples.size() << ",p50_ns=" << percentile(50) << ",p95_ns=" << percentile(95)
                      << ",p99_ns=" << percentile(99) << ",max_ns=" << samples.back() << '\n';
        }
    }  // namespace

    TEST_CASE("Mounted World streams a verified cell through the real structural cutoff and detaches only its occurrence",
              "[world_cook][native][mounted_world][live_cell]") {
        Fixture fixture{false, false};
        const auto published = fixture.Publish();
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().publication);
        const auto release = fixture.Release();
        const auto identity = fixture.inputs.front().expected;
        const CommittedWorldInitialCell request{identity.cell, {900}, identity.scene, identity.revision};
        const CommittedWorldSelection selection{release.targetRoot,
                                                release.target,
                                                published.Value().publication->manifestDigest,
                                                release.indexArtifact,
                                                release.dependencyArtifact,
                                                Math::WorldCoordinate64::FromMillimeters(0, 0, 0)};

        JobSystem jobs{JobSystemConfig{1, 4}};
        A::MemoryAssetProvider provider;
        A::AssetLoadService loads{jobs, provider};
        A::AssetRegistry registry;
        S::RuntimeSceneService scene{registry, loads, S::RuntimeSceneAssetLimits{}, S::SceneRuntimeId{101}};
        S::SceneDefinitionBuilder startup{{99}, {1}};
        startup.Add({.object = {42}});
        auto definition = std::move(startup).Build();
        REQUIRE(definition.HasValue());
        const HeadlessSceneLoadBoundary boundary{{}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
        REQUIRE(PrepareHeadlessSceneAtLoadBoundary(scene, std::move(definition).Value(), boundary).HasValue());
        REQUIRE(scene.ActiveScene());
        const auto root = scene.ActiveScene()->RuntimeId();
        const S::SceneInstanceAddress address{root, request.occurrence, request.definition, S::SceneInstanceMode::Streamed};
        const auto epoch = IdentityFrom<W::PartitionEpoch>(5);
        const auto attached = [&] {
            REQUIRE(scene.ActiveScene());
            REQUIRE(scene.ActiveScene()->RuntimeId() == root);
            REQUIRE(scene.ActiveScene()->Find({42}));
            return S::FindStreamedSceneInstance(*scene.ActiveScene(), address, identity, epoch).has_value();
        };

        auto prepared = MountedWorld::Prepare(NativeDurableFileSystem{}, selection, 5, scene);
        REQUIRE(prepared.HasValue());
        auto mount = std::move(prepared).Value();
        DeterministicClock clock;
        auto created = S::RuntimeHost::Create(clock);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        auto driver = std::make_unique<MountedCellDriver>(*mount, scene);
        auto &progress = *driver;
        REQUIRE(host->AddParticipant(std::move(driver)).HasValue());
        REQUIRE(mount->BindLiveCell(host->DispatchSource(), jobs, {20'000'000, 1'000'000, 4}).HasValue());
        REQUIRE(host->Startup().HasValue());
        W::TestSupport::RequireError(mount->AdvanceLiveCell({}), W::WorldArtifactErrors::Stale);
        const auto frame = [&] {
            clock.Advance(Duration::FromMilliseconds(17));
            REQUIRE(host->RunFrame().HasValue());
        };
        const auto until = [&](auto done) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            do {
                frame();
                if (done(progress.last))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("Mounted cell failed to settle before the fixture deadline");
        };
        const auto stage = [&](const Stage expected) {
            REQUIRE(progress.last.HasValue());
            REQUIRE(progress.last.Value() == expected);
        };

        REQUIRE_FALSE(attached());
        REQUIRE(mount->RequestLiveCell(request).HasValue());
        progress.probeDuplicate = true;
        frame();
        stage(Stage::Loading);
        REQUIRE(progress.duplicate);
        W::TestSupport::RequireError(*progress.duplicate, W::WorldArtifactErrors::Stale);
        REQUIRE_FALSE(attached());

        SECTION("Cold teardown drains an in-flight preparation without shutting down the unrelated parent") {
            host->Shutdown();
            mount.reset();
            REQUIRE_FALSE(attached());
            return;
        }

        SECTION("Cancelling submitted preparation cannot attach a Scene or block fresh demand") {
            REQUIRE(mount->WithdrawLiveCell().HasValue());
            until([&](const Result<Stage> &last) {
                return last.HasError();
            });
            W::TestSupport::RequireError(progress.last, W::WorldArtifactErrors::Cancelled);
            REQUIRE_FALSE(attached());
            REQUIRE(mount->RequestLiveCell(request).HasValue());
            frame();
            stage(Stage::Loading);
        }

        until([](const Result<Stage> &last) {
            return last.HasError() || last.Value() != Stage::Loading;
        });
        stage(Stage::Prepared);
        REQUIRE_FALSE(attached());
        frame();
        stage(Stage::Attaching);
        REQUIRE(attached());
        frame();
        stage(Stage::Active);
        REQUIRE(mount->WithdrawLiveCell().HasValue());
        frame();
        stage(Stage::Retiring);
        REQUIRE_FALSE(attached());
        frame();
        stage(Stage::Idle);
        REQUIRE_FALSE(attached());
    }

    TEST_CASE("Standalone mounted CoreEcs demand starts in the product frame and withdraws through the same Scene owner",
              "[world_cook][native][mounted_world][live_cell][product]") {
        SECTION("Legacy mounted worlds stay idle and refuse live demand") {
            MountedCellProduct product{false, false};
            W::TestSupport::RequireError(product.ProbeRequest(), W::WorldArtifactErrors::Stale);
            static_cast<void>(product.RunFrame());
            REQUIRE_FALSE(product.Attached());
        }
        SECTION("An explicitly selected first demand is deferred until real frames, with retirement and fresh demand") {
            MountedCellProduct product{true, true};
            REQUIRE_FALSE(product.Attached());
            static_cast<void>(product.UntilAttached(true));
            static_cast<void>(product.RunFrame());  // Reconcile the exact positive Scene receipt.
            product.Withdraw();
            static_cast<void>(product.UntilAttached(false));
            static_cast<void>(product.RunFrame());  // Acknowledge retirement before issuing a fresh generation.
            product.Request();
            static_cast<void>(product.UntilAttached(true));
        }
        SECTION("An in-frame replacement cannot drain a live mounted worker or revoke its parent") {
            MountedCellProduct product{true, true};
            REQUIRE_FALSE(product.Attached());
            product.CheckTravelRefusal();
            static_cast<void>(product.UntilAttached(true));
        }
    }

    TEST_CASE("Measure mounted CoreEcs frame impact through the standalone product", "[.mounted_world_benchmark]") {
        constexpr std::size_t warmup = 16;
        constexpr std::size_t sampleCount = 128;
        MountedCellProduct product{true, false};
        std::array<std::int64_t, sampleCount> idle{};
        std::array<std::int64_t, sampleCount> attach{};
        std::array<std::int64_t, sampleCount> detach{};
        for (std::size_t index = 0; index < warmup + sampleCount; ++index) {
            const auto idleFrame = product.RunFrame();
            product.Request();
            const auto attachFrame = product.UntilAttached(true);
            static_cast<void>(product.RunFrame());
            product.Withdraw();
            const auto detachFrame = product.UntilAttached(false);
            static_cast<void>(product.RunFrame());
            if (index >= warmup) {
                idle[index - warmup] = idleFrame;
                attach[index - warmup] = attachFrame;
                detach[index - warmup] = detachFrame;
            }
        }
        std::cout << "mounted_coreecs,entities_per_cell=1,worker_threads=1,physics=null,provider=memory,clock=steady_clock\n";
        PrintFrameSamples("idle_frame", idle);
        PrintFrameSamples("attach_frame", attach);
        PrintFrameSamples("detach_frame", detach);
    }
}  // namespace Horo::WorldCook
