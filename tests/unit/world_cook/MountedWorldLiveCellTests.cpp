#include "../../integration/network/NetworkModeHostTestSupport.h"
#include "../runtime/physics/PhysicsSceneActivationTestSupport.h"
#include "HeadlessSceneLoadBoundary.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
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
            explicit MountedCellProduct(const bool enableLive, const bool firstDemand, const std::size_t maximumResidents = 1) {
                namespace N = Network;
                const auto published = fixture_.Publish();
                REQUIRE(published.HasValue());
                REQUIRE(published.Value().publication);
                const auto release = fixture_.Release();
                identity_ = fixture_.inputs.front().expected;
                demand_ = {identity_.cell, {900}, identity_.scene, identity_.revision};
                siblingIdentity_ = fixture_.inputs.back().expected;
                sibling_ = {siblingIdentity_.cell, {901}, siblingIdentity_.scene, siblingIdentity_.revision};
                CommittedWorldSelection selection{release.targetRoot,
                                                  release.target,
                                                  published.Value().publication->manifestDigest,
                                                  release.indexArtifact,
                                                  release.dependencyArtifact,
                                                  Math::WorldCoordinate64::FromMillimeters(0, 0, 0)};
                if (enableLive) {
                    const MountedWorldLiveLimits limits{.maximumFrameNanoseconds = 20'000'000,
                                                        .maximumUnitNanoseconds = 1'000'000,
                                                        .maximumCellEntities = 4,
                                                        .maximumResidentCells = maximumResidents};
                    selection.liveCell = CommittedWorldLiveSelection{limits, firstDemand ? std::optional{demand_} : std::nullopt};
                }
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

            void RequestSibling() const {
                Check(RequestHeadlessWorldCell(*scene_, sibling_));
            }

            void WithdrawFirstOccurrence() const {
                Check(WithdrawHeadlessWorldCell(*scene_, demand_.occurrence));
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

            bool Attached(const bool sibling = false) const {
                const auto scene = InspectHeadlessScene(*scene_);
                REQUIRE(scene);
                REQUIRE(scene->RuntimeId() == runtime_);
                REQUIRE(scene->Find(parentObject_));
                const auto &selected = sibling ? sibling_ : demand_;
                return S::FindStreamedSceneInstance(*scene,
                                                    {runtime_, selected.occurrence, selected.definition, S::SceneInstanceMode::Streamed},
                                                    sibling ? siblingIdentity_ : identity_, epoch_)
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

            std::int64_t UntilAttached(const bool expected, const bool sibling = false) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do {
                    const auto sample = RunFrame();
                    if (Attached(sibling) == expected)
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
            S::SceneCellPayloadIdentity siblingIdentity_;
            W::PartitionEpoch epoch_;
            CommittedWorldInitialCell demand_;
            CommittedWorldInitialCell sibling_;
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

        /** @brief Gives the two cells independent native colliders backed by the same verified material envelope. */
        void PopulateCellMaterial(Fixture &fixture, const A::AssetDependency &material) {
            for (std::size_t index = 0; index < fixture.entities.size(); ++index) {
                auto &entity = fixture.entities[index];
                const auto body = 100 + index;
                entity.components.rigidBody = S::RigidBodyComponent{.id = {10 + index}, .body = {body}};
                entity.components.colliders = {
                    Physics::Collider(20 + index, 200 + index, entity.object.value, body, material.id,
                                      S::PhysicsColliderSource{S::PhysicsAnalyticCollider{S::PhysicsSphereCollider{0.5F}}})};
                fixture.inputs[index].source.dependencies = {&material, 1};
            }
            const auto descriptor = Physics::PhysicsMaterialAsset::Create(
                {.id = Physics::PhysicsMaterialAssetId::FromAssetId(material.id), .sourceRevision = 1});
            REQUIRE(descriptor.HasValue());
            const auto encoded = Physics::EncodePhysicsMaterialAsset(descriptor.Value());
            REQUIRE(encoded.HasValue());
            const auto digest = ComputeSha256(encoded.Value());
            const auto bytes = A::EncodeCookedArtifact(
                {material.id, material.expectedType, fixture.Release().target, digest, digest, digest,
                 std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t *>(encoded.Value().data()),
                                           reinterpret_cast<const std::uint8_t *>(encoded.Value().data() + encoded.Value().size()))});
            REQUIRE(bytes.HasValue());
            fixture.baseArtifacts.push_back(bytes.Value());
            fixture.dependencyArtifacts.push_back({.id = material.id, .content = T::DigestOf(bytes.Value()), .revision = 1});
            const std::array chunks{A::AssetChunkDefinition{A::AssetChunkId::Parse("base").Value(),
                                                            A::AssetChunkKind::Base,
                                                            {Asset(4), Asset(5), Asset(6), Asset(7), material.id}}};
            auto packages = A::AssetChunkPlan::Create(chunks);
            REQUIRE(packages.HasValue());
            fixture.packages = std::move(packages).Value();
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
        const auto siblingIdentity = fixture.inputs.back().expected;
        const CommittedWorldInitialCell sibling{siblingIdentity.cell, {901}, siblingIdentity.scene, siblingIdentity.revision};
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
        REQUIRE(mount
                    ->BindLiveCell(host->DispatchSource(), jobs,
                                   {.maximumFrameNanoseconds = 20'000'000,
                                    .maximumUnitNanoseconds = 1'000'000,
                                    .maximumCellEntities = 4,
                                    .maximumResidentCells = 2})
                    .HasValue());
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

        const S::SceneInstanceAddress siblingAddress{root, sibling.occurrence, sibling.definition, S::SceneInstanceMode::Streamed};
        const auto siblingAttached = [&] {
            REQUIRE(scene.ActiveScene());
            return S::FindStreamedSceneInstance(*scene.ActiveScene(), siblingAddress, siblingIdentity, epoch).has_value();
        };
        REQUIRE(mount->RequestLiveCell(request).HasError());
        REQUIRE(mount->RequestLiveCell(sibling).HasValue());
        REQUIRE(mount->WithdrawLiveCell().HasError());
        until([](const Result<Stage> &last) {
            return last.HasError() || last.Value() == Stage::Active;
        });
        stage(Stage::Active);
        REQUIRE(attached());
        REQUIRE(siblingAttached());
        REQUIRE(mount->WithdrawLiveCell(request.occurrence).HasValue());
        frame();
        stage(Stage::Retiring);
        REQUIRE_FALSE(attached());
        REQUIRE(siblingAttached());
        frame();
        stage(Stage::Active);
        REQUIRE(mount->WithdrawLiveCell(request.occurrence).HasError());
        REQUIRE(mount->WithdrawLiveCell().HasValue());
        frame();
        stage(Stage::Retiring);
        REQUIRE_FALSE(siblingAttached());
        frame();
        stage(Stage::Idle);
        REQUIRE_FALSE(attached());
        REQUIRE_FALSE(siblingAttached());
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
        SECTION("Configured standalone hosts can withdraw an exact occurrence while its sibling stays published") {
            MountedCellProduct product{true, true, 2};
            static_cast<void>(product.UntilAttached(true));
            static_cast<void>(product.RunFrame());  // Reconcile the first positive Scene receipt.
            product.RequestSibling();
            static_cast<void>(product.UntilAttached(true, true));
            static_cast<void>(product.RunFrame());
            product.WithdrawFirstOccurrence();
            static_cast<void>(product.UntilAttached(false));
            REQUIRE(product.Attached(true));
        }
    }

    TEST_CASE("Mounted live cells charge one committed allocation and retire it after the last native detach",
              "[world_cook][native][mounted_world][live_cell]") {
        auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical);
        if (runtime.HasError() && runtime.ErrorValue().code.Value() == Physics::PhysicsErrors::CapabilityUnavailable.code.Value())
            SKIP("The native Physics provider is omitted from this test build.");
        REQUIRE(runtime.HasValue());
        const A::AssetDependency material{Asset(9), Physics::AssetType()};
        Fixture fixture{false, false};
        PopulateCellMaterial(fixture, material);
        const auto published = fixture.Publish();
        if (published.HasError())
            UNSCOPED_INFO(published.ErrorValue().code.Value() << ": " << published.ErrorValue().message);
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().publication);
        const auto release = fixture.Release();
        const auto identity = fixture.inputs.front().expected;
        const CommittedWorldInitialCell request{identity.cell, {900}, identity.scene, identity.revision};
        const auto siblingIdentity = fixture.inputs.back().expected;
        const CommittedWorldInitialCell sibling{siblingIdentity.cell, {901}, siblingIdentity.scene, siblingIdentity.revision};
        const auto bytes = fixture.baseArtifacts.front().size();
        MountedWorldLiveLimits limits{20'000'000, 1'000'000, 4, 1, bytes, 2};
        bool expectCapacity{};
        bool retainReader{};
        SECTION("One byte less than the exact envelope never publishes resources or native bodies") {
            --limits.maximumCellAssetBytes;
            expectCapacity = true;
        }
        SECTION("Both live cells share the complete envelope through exact native detach receipts") {
            // The configured budget and reader lifetime are used as-is for the normal detach sequence.
        }
        SECTION("A remaining payload pin delays budget credit after the last native detach") {
            retainReader = true;
        }
        const CommittedWorldSelection selection{release.targetRoot,
                                                release.target,
                                                published.Value().publication->manifestDigest,
                                                release.indexArtifact,
                                                release.dependencyArtifact,
                                                Math::WorldCoordinate64::FromMillimeters(0, 0, 0),
                                                {},
                                                CommittedWorldLiveSelection{limits, std::nullopt}};
        JobSystem jobs{JobSystemConfig{1, 4}};
        A::MemoryAssetProvider provider;
        A::AssetLoadService loads{jobs, provider};
        A::AssetRegistry registry;
        REQUIRE(registry.Publish({Physics::Record(material.id, material.expectedType)}).status == A::AssetRegistryBuildStatus::Complete);
        Physics::PhysicsSceneActivationAuthority authority;
        ScenePhysicsHost native;
        S::RuntimeSceneService scene{registry, loads, S::RuntimeSceneAssetLimits{}, S::SceneRuntimeId{101}};
        auto activation = native.MakeParticipant(*runtime.Value(), authority, Physics::Settings());
        REQUIRE(activation.HasValue());
        REQUIRE(scene.AddActivationParticipant(std::move(activation).Value()).HasValue());
        auto structural = native.MakeStructuralParticipant();
        REQUIRE(structural.HasValue());
        REQUIRE(scene.AddStructuralParticipant(std::move(structural).Value()).HasValue());
        S::SceneDefinitionBuilder startup{{99}, {1}};
        startup.Add({.object = {42}});
        auto definition = std::move(startup).Build();
        REQUIRE(definition.HasValue());
        const HeadlessSceneLoadBoundary boundary{{}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
        REQUIRE(PrepareHeadlessSceneAtLoadBoundary(scene, std::move(definition).Value(), boundary).HasValue());
        REQUIRE(scene.ActiveScene());
        const auto root = scene.ActiveScene()->RuntimeId();
        const S::SceneInstanceAddress address{root, request.occurrence, request.definition, S::SceneInstanceMode::Streamed};
        const S::SceneInstanceAddress siblingAddress{root, sibling.occurrence, sibling.definition, S::SceneInstanceMode::Streamed};
        const auto epoch = IdentityFrom<W::PartitionEpoch>(5);
        const auto attached = [&] {
            REQUIRE(scene.ActiveScene());
            REQUIRE(scene.ActiveScene()->Find({42}));
            return S::FindStreamedSceneInstance(*scene.ActiveScene(), address, identity, epoch);
        };
        const auto bodyCount = [&] {
            const auto scope = native.Borrow();
            REQUIRE(scope.HasValue());
            const auto observed = scope.Value().physics.ReadSceneActivation();
            REQUIRE(observed.HasValue());
            return observed.Value().staticBodies;
        };
        const auto siblingAttached = [&] {
            REQUIRE(scene.ActiveScene());
            return S::FindStreamedSceneInstance(*scene.ActiveScene(), siblingAddress, siblingIdentity, epoch);
        };
        auto prepared = MountedWorld::Prepare(NativeDurableFileSystem{}, selection, 5, scene);
        REQUIRE(prepared.HasValue());
        auto mount = std::move(prepared).Value();
        DeterministicClock clock;
        auto created = S::RuntimeHost::Create(clock);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        auto driver = std::make_unique<MountedCellDriver>(*mount, scene);
        const auto &progress = *driver;
        REQUIRE(host->AddParticipant(std::move(driver)).HasValue());
        REQUIRE(mount->BindLiveCell(host->DispatchSource(), jobs, limits, registry.Snapshot()).HasValue());
        REQUIRE(host->Startup().HasValue());
        REQUIRE(bodyCount() == 0);
        REQUIRE(mount->RequestLiveCell(request).HasValue());
        const auto frame = [&] {
            clock.Advance(Duration::FromMilliseconds(17));
            REQUIRE(host->RunFrame().HasValue());
        };
        const auto load = [&] {
            frame();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (progress.last.HasValue() && progress.last.Value() == Stage::Loading && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                frame();
            }
        };
        const auto chargedBytes = [&] {
            const auto charged = mount->InspectLiveAssetBytes();
            REQUIRE(charged.HasValue());
            return charged.Value();
        };
        load();
        if (expectCapacity) {
            W::TestSupport::RequireError(progress.last, S::SceneCellPayloadErrors::CapacityExceeded);
            CHECK_FALSE(attached());
            CHECK(bodyCount() == 0);
            CHECK(chargedBytes() == 0);
            return;
        }
        REQUIRE(progress.last.HasValue());
        REQUIRE(progress.last.Value() == Stage::Prepared);
        CHECK(bodyCount() == 0);
        CHECK(chargedBytes() == bytes);
        frame();
        const auto baseline = attached();
        REQUIRE(baseline);
        REQUIRE(baseline->resources.size() == 1);
        CHECK(baseline->resources.front().metadata == material);
        CHECK(baseline->resources.front().artifact.Digest() == T::DigestOf(fixture.baseArtifacts.front()));
        CHECK(bodyCount() == 1);
        frame();
        REQUIRE(progress.last.HasValue());
        REQUIRE(progress.last.Value() == Stage::Active);
        REQUIRE(mount->RequestLiveCell(sibling).HasValue());
        load();
        REQUIRE(progress.last.HasValue());
        REQUIRE(progress.last.Value() == Stage::Prepared);
        CHECK(chargedBytes() == bytes);
        frame();
        A::AssetPayloadLease reader;
        {
            const auto first = attached();
            const auto second = siblingAttached();
            REQUIRE(first);
            REQUIRE(second);
            REQUIRE(second->resources.size() == 1);
            CHECK(first->resources.front().artifact.SharesAllocationWith(second->resources.front().artifact));
            CHECK(bodyCount() == 2);
            if (retainReader)
                reader = second->resources.front().artifact;
        }
        frame();
        REQUIRE(mount->WithdrawLiveCell(request.occurrence).HasValue());
        frame();
        CHECK_FALSE(attached());
        REQUIRE(siblingAttached());
        CHECK(bodyCount() == 1);
        frame();
        REQUIRE(progress.last.HasValue());
        CHECK(progress.last.Value() == Stage::Active);
        CHECK(chargedBytes() == bytes);
        REQUIRE(mount->WithdrawLiveCell(sibling.occurrence).HasValue());
        frame();
        CHECK_FALSE(siblingAttached());
        CHECK(bodyCount() == 0);
        CHECK(chargedBytes() == bytes);
        frame();
        REQUIRE(progress.last.HasValue());
        CHECK(progress.last.Value() == Stage::Idle);
        if (retainReader) {
            CHECK(chargedBytes() == bytes);
            reader = {};
            CHECK(chargedBytes() == bytes);
            frame();
        }
        CHECK(chargedBytes() == 0);
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
