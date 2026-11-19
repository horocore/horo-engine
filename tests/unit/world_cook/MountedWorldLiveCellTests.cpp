#include "HeadlessSceneLoadBoundary.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "MountedWorldHost.h"
#include "WorldCookPublicationTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
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
}  // namespace Horo::WorldCook
