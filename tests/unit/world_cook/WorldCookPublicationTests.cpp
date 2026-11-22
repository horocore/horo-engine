#include "AllocationProbe.h"
#include "EditorProjectSceneHost.h"
#include "HeadlessSceneLoadBoundary.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/WorldCook/WorldCookPublication.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "MountedWorldHost.h"
#include "WorldCookPublicationTestSupport.h"
#if HORO_TEST_NAVIGATION_NATIVE
#include "navigation/ProjectSceneNavigationFixture.h"
#endif

#include <algorithm>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <fstream>
#include <functional>
#include <thread>

namespace Horo::WorldCook {
    namespace {
        namespace W = WorldStreaming;
        namespace S = Runtime;
        namespace A = Assets;
        namespace T = A::OutputTestSupport;
        using W::TestSupport::Asset;
        using W::TestSupport::IdentityFrom;

        /** @brief Forwards the actual runtime structural cutoff to the product Play owner. */
        class EditorWorldCutoff final : public S::RuntimeLifecycleParticipant {
        public:
            explicit EditorWorldCutoff(Editor::EditorPlaySceneHost &play) : play_(play) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                play_.Shutdown();
            }

            Result<void> OnPhase(S::RuntimePhase phase, const S::FrameContext &frame) override {
                return phase == S::RuntimePhase::CommitDeferredLifecycleChanges ? play_.OnRuntimeSceneCutoff(frame)
                                                                                : Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const S::FixedStepContext &) override {
                return Result<void>::Success();
            }

        private:
            Editor::EditorPlaySceneHost &play_;
        };

        /** @brief Observes the actual private candidate without publishing any test-owned geometry. */
        class StartupGeometryObserver final : public S::SceneActivationParticipant {
        public:
            explicit StartupGeometryObserver(const std::function<void(const S::RuntimeSceneView &)> &observe) : observe_(observe) {}

            Result<std::unique_ptr<S::SceneActivationCandidate>> Prepare(const S::RuntimeSceneDefinition &,
                                                                         const S::RuntimeSceneView &candidate) override {
                if (observe_)
                    observe_(candidate);
                return Result<std::unique_ptr<S::SceneActivationCandidate>>::Success(std::make_unique<Observed>());
            }

        private:
            class Observed final : public S::SceneActivationCandidate {
            public:
                Result<void> ValidatePublication() const override {
                    return Result<void>::Success();
                }

                void Shutdown() noexcept override {}
            };

            const std::function<void(const S::RuntimeSceneView &)> &observe_;
        };

        using namespace PublicationTestSupport;

        /** @brief Reads exact committed owner artifacts rather than reconstructing expected bytes in the assertion. */
        A::AssetCookGenerationContents Contents(const PublicationReport &report) {
            REQUIRE(report.publication);
            auto contents =
                A::ReadCookGenerationContents(Horo::NativeDurableFileSystem{}, *report.publication, Limits().maximumTotalArtifactBytes);
            REQUIRE(contents.HasValue());
            return std::move(contents).Value();
        }

        /** @brief Detaches actual published provider bytes for independent malformed/missing-artifact admission. */
        struct Detached final {
            std::vector<std::byte> index;
            std::vector<std::byte> dependencies;
            std::vector<std::pair<A::AssetId, std::vector<std::byte>>> cells;

            explicit Detached(const A::AssetCookGenerationContents &contents) {
                for (const auto &bytes : contents.artifacts) {
                    auto decoded = A::DecodeCookedArtifact(bytes);
                    REQUIRE(decoded.HasValue());
                    const auto raw = std::as_bytes(std::span{decoded.Value().payload});
                    std::vector<std::byte> copied(raw.begin(), raw.end());
                    if (decoded.Value().id == Asset(6))
                        index = std::move(copied);
                    else if (decoded.Value().id == Asset(7))
                        dependencies = std::move(copied);
                    else
                        cells.emplace_back(decoded.Value().id, std::move(copied));
                }
            }

            Result<ValidationReport> Validate(const Fixture &fixture) const {
                std::vector<CellArtifactView> views;
                for (const auto &[id, bytes] : cells)
                    views.push_back({id, bytes});
                return ValidateWorldCookArtifacts(fixture.Source(), fixture.Release(), {index, dependencies, views}, Limits());
            }
        };
    }  // namespace

    TEST_CASE("World cook binds cell NavigationMesh membership to the original release envelope", "[world_cook][native][navigation]") {
        Fixture fixture{true};
        const auto exact = CaptureNavigationCell(fixture);
        fixture.navigationCells.push_back({fixture.inputs.front().expected.cell, exact});
        auto limits = Limits();
        limits.cell.maximumPayloads = 2;

        auto published = fixture.Publish(limits);
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().publication);
        const auto contents = Contents(published.Value());
        const auto cellAsset = fixture.world.manifest.Descriptor().Cells().front().package.chunkAsset;
        const auto entry = std::ranges::find(contents.entries, cellAsset, &A::AssetCookManifestEntry::assetId);
        REQUIRE(entry != contents.entries.end());
        const auto envelope = A::DecodeCookedArtifact(contents.artifacts[static_cast<std::size_t>(entry - contents.entries.begin())]);
        REQUIRE(envelope.HasValue());
        const auto bytes = std::as_bytes(std::span{envelope.Value().payload});
        const Detached detached{contents};
        const auto index = W::DecodeWorldIndexArtifact(detached.index, limits.index);
        REQUIRE(index.HasValue());
        const auto expected =
            std::ranges::find(index.Value().cells, fixture.inputs.front().expected.cell, &W::CookedWorldCellManifestEntry::cell);
        REQUIRE(expected != index.Value().cells.end());
        const auto inspected = W::InspectStreamingCellArtifact(bytes, expected->artifactHash, {2, 65536, 65536});
        REQUIRE(inspected.HasValue());
        const auto header = inspected.Value().Header();
        REQUIRE(header.payloads.size() == 2);
        const auto &nav = header.payloads.back();
        CHECK(nav.provider == W::StreamingCellProvider::NavigationMesh);
        CHECK(nav.version == Navigation::NavigationCellTileSelection::WireVersion);
        CHECK(nav.requirement == W::StreamingCellPayloadRequirement::Required);
        const auto raw = bytes.subspan(static_cast<std::size_t>(nav.offset), static_cast<std::size_t>(nav.compressedSize));
        const auto admitted = Navigation::DecodeNavigationCellTileSelection(raw, raw.size());
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().definition == exact.definition);
        CHECK(admitted.Value().cookedEnvelope == exact.cookedEnvelope);
        CHECK(admitted.Value().tiles == exact.tiles);
        CHECK(Navigation::DecodeNavigationCellTileSelection(raw.first(raw.size() - 1), raw.size()).HasError());

        const auto revision = fixture.cache.Revision();
        fixture.navigationCells.front().selection.tiles.front().tile.z = 1;
        auto stale = fixture.Publish(limits);
        REQUIRE(stale.HasValue());
        REQUIRE(stale.Value().validation);
        CHECK_FALSE(stale.Value().validation->CanPublish());
        CHECK_FALSE(stale.Value().publication);
        CHECK(fixture.cache.Revision() == revision);
    }

    TEST_CASE("Headless World mount pins a committed release and retires its origin with the Scene owner",
              "[world_cook][native][mounted_world]") {
        using Application::Internal::CommittedWorldSelection;
        using Application::Internal::MountedWorld;
        Fixture fixture;
        auto published = fixture.Publish();
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().publication);
        const auto release = fixture.Release();
        const CommittedWorldSelection selection{release.targetRoot,
                                                release.target,
                                                published.Value().publication->manifestDigest,
                                                release.indexArtifact,
                                                release.dependencyArtifact,
                                                Math::WorldCoordinate64::FromMillimeters(20'000'000'001, -500, 180)};
        JobSystem jobs{JobSystemConfig{1, 4}};
        A::MemoryAssetProvider provider;
        A::AssetLoadService loads{jobs, provider};
        A::AssetRegistry registry;
        S::RuntimeSceneService scene{registry, loads, S::RuntimeSceneAssetLimits{}, S::SceneRuntimeId{101}};
        const NativeDurableFileSystem files;
        auto admitted = MountedWorld::Prepare(files, selection, 5, scene);
        REQUIRE(admitted.HasValue());
        auto mount = std::move(admitted).Value();
        const auto lease = mount->Origin().Lease().Value();
        CHECK(lease.Get().Value().Origin() == selection.origin);
        CHECK(lease.Get().Value().Binding().identity == IdentityFrom<W::OriginFrameId>(5));

        auto stale = selection;
        stale.manifest = {};
        W::TestSupport::RequireError(MountedWorld::Prepare(files, stale, 6, scene), W::WorldArtifactErrors::Stale);
        stale = selection;
        stale.dependencies = Asset(4);  // Committed cell ID cannot satisfy the exact sibling role.
        W::TestSupport::RequireError(MountedWorld::Prepare(files, stale, 6, scene), W::WorldArtifactErrors::Invalid);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        W::TestSupport::RequireError(MountedWorld::Prepare(files, selection, 6, scene, cancellation.Token()),
                                     W::WorldArtifactErrors::Cancelled);
        REQUIRE(lease.Get().HasValue());

        auto replacement = MountedWorld::Prepare(files, selection, 6, scene);
        REQUIRE(replacement.HasValue());
        auto replacementLease = replacement.Value()->Origin().Lease().Value();
        mount.reset();
        CHECK(lease.Get().HasError());
        CHECK(replacementLease.Get().Value().Binding().identity == IdentityFrom<W::OriginFrameId>(6));
    }

    TEST_CASE("Standalone startup attaches only the verified pinned Scene occurrence and retires it with the parent",
              "[world_cook][native][mounted_world][initial_cell]") {
        using namespace Application::Internal;
        const bool hasHardDependency = GENERATE(false, true);
        const bool rigidRoot = GENERATE(true, false);
        Fixture fixture{false, hasHardDependency};
        constexpr std::array<std::int64_t, 3> cameraPosition{9'007'199'254'740'993, -500, 180};
        constexpr std::array<std::int64_t, 3> cameraTarget{9'007'199'254'741'993, -500, 180};
        const std::array anchors{S::SceneCellWorldAnchor{{11}, cameraPosition, cameraTarget}};
        auto &cameraEntity = fixture.entities.front();
        cameraEntity.components.camera = S::CameraComponent{};
        cameraEntity.primitiveMesh = S::PrimitiveMeshDescriptor::Defaults(S::PrimitiveMeshType::Box);
        cameraEntity.localTransform = {{100.0F, 200.0F, 300.0F},
                                       Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, 0.5F),
                                       rigidRoot ? Math::Vec3{1.0F, 1.0F, 1.0F} : Math::Vec3{2.0F, 3.0F, 4.0F}};
        fixture.inputs.front().source.worldAnchors = anchors;
        const auto published = fixture.Publish();
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().publication);
        const auto release = fixture.Release();
        const auto selected = fixture.inputs.front().expected;
        CommittedWorldInitialCell request{selected.cell, {900}, selected.scene, selected.revision};
        const CommittedWorldSelection selection{release.targetRoot,
                                                release.target,
                                                published.Value().publication->manifestDigest,
                                                release.indexArtifact,
                                                release.dependencyArtifact,
                                                Math::WorldCoordinate64::FromMillimeters(cameraPosition[0] - 1250, cameraPosition[1],
                                                                                         cameraPosition[2]),
                                                request};

        JobSystem jobs{JobSystemConfig{1, 4}};
        A::MemoryAssetProvider provider;
        A::AssetLoadService loads{jobs, provider};
        A::AssetRegistry registry;
        std::function<void(const S::RuntimeSceneView &)> observeStartup;
        S::RuntimeSceneService parent{registry, loads, S::RuntimeSceneAssetLimits{}, S::SceneRuntimeId{101}};
        REQUIRE(parent.AddActivationParticipant(std::make_unique<StartupGeometryObserver>(observeStartup)).HasValue());
        auto definition = std::move(S::SceneDefinitionBuilder{{99}, {1}}).Build();
        REQUIRE(definition.HasValue());
        HeadlessSceneLoadBoundary boundary{{}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
        REQUIRE(PrepareHeadlessSceneAtLoadBoundary(parent, std::move(definition).Value(), boundary).HasValue());
        const NativeDurableFileSystem files;
        auto admitted = MountedWorld::Prepare(files, selection, 5, parent);
        REQUIRE(admitted.HasValue());
        auto mount = std::move(admitted).Value();
        const auto lease = mount->Origin().Lease().Value();
        const auto address = S::SceneInstanceAddress{{101}, request.occurrence, request.definition, S::SceneInstanceMode::Streamed};
        const auto epoch = IdentityFrom<W::PartitionEpoch>(5);
        REQUIRE_FALSE(S::FindStreamedSceneInstance(*parent.ActiveScene(), address, selected, epoch));
        CHECK(mount->Anchors(*parent.ActiveScene()).empty());

        if (hasHardDependency) {
            W::TestSupport::RequireError(mount->ActivateInitialCell(request, {boundary.cancellation, boundary.deadline}),
                                         W::WorldArtifactErrors::Unsupported);
            CHECK_FALSE(S::FindStreamedSceneInstance(*parent.ActiveScene(), address, selected, epoch));
            return;
        }
        SECTION("Startup cancellation leaves no structural publication") {
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            boundary.cancellation = cancellation.Token();
            W::TestSupport::RequireError(mount->ActivateInitialCell(request, {boundary.cancellation, boundary.deadline}),
                                         W::WorldArtifactErrors::Cancelled);
            CHECK_FALSE(S::FindStreamedSceneInstance(*parent.ActiveScene(), address, selected, epoch));
        }
        SECTION("Foreign revision cannot activate a committed baseline") {
            ++request.revision.value;
            CHECK(mount->ActivateInitialCell(request, {boundary.cancellation, boundary.deadline}).HasError());
            CHECK_FALSE(S::FindStreamedSceneInstance(*parent.ActiveScene(), address, selected, epoch));
        }
        SECTION("Private startup retains a real mount reservation until the exact Scene receipt publishes") {
            const auto old = *parent.ActiveScene();
            auto successor = address;
            successor.parent = parent.NextRuntimeId();
            REQUIRE(registry.Publish(std::vector<A::AssetRecord>{}).status == A::AssetRegistryBuildStatus::Complete);
            const auto revision = registry.Snapshot().Revision();
            REQUIRE(revision != A::AssetRegistryRevision{});
            const W::OriginRebaseParticipantRequirement participant{IdentityFrom<W::OriginRebaseParticipantId>(1),
                                                                    IdentityFrom<W::OriginRebaseParticipantRevision>(1)};
            auto renderer = Render::RenderOriginSceneAdapter::Create(mount->Origin(), {2}, participant, {2, 1, 1}).Value();
            S::PrimitiveMeshCache primitives;
            const std::array drawObjects{cameraEntity.object};
            std::optional<MountedWorldRenderPacket> geometry;
            CHECK_FALSE(parent.IsInitialStreamedCandidate(old));
            CHECK(mount->CaptureInitialGeometry(old, cameraEntity.object, drawObjects, primitives, *renderer).HasError());
            observeStartup = [&](const S::RuntimeSceneView &candidate) {
                REQUIRE(parent.IsInitialStreamedCandidate(candidate));
                CHECK(parent.ActiveScene()->SameIncarnation(old));
                auto packet = mount->CaptureInitialGeometry(candidate, cameraEntity.object, drawObjects, primitives, *renderer);
                REQUIRE(packet.HasValue());
                geometry.emplace(std::move(packet).Value());
                const auto captured = geometry->OriginScene();
                REQUIRE(captured.HasValue());
                REQUIRE(captured.Value().scene.IsValid());
                CHECK(captured.Value().origin.Get().Value() == lease.Get().Value());
                CHECK(captured.Value().scene.camera.position == Math::Vec3{1.25F, 0, 0});
                REQUIRE(captured.Value().scene.instances.size() == 1);
                CHECK(captured.Value().scene.instances.front().localToWorld.At(0, 3) == 1.25F);
                const auto root = mount->ResolveInitialTrackingRoot(candidate, cameraEntity.object);
                if (rigidRoot) {
                    REQUIRE(root.HasValue());
                    CHECK(root.Value().origin.Get().Value() == captured.Value().origin.Get().Value());
                    CHECK(root.Value().originFromLocal.translation == Math::Vec3{1.25F, 0, 0});
                    CHECK(root.Value().originFromLocal.rotation == cameraEntity.localTransform.rotation.TryNormalized().Value());
                } else {
                    W::TestSupport::RequireError(root, W::WorldArtifactErrors::Unsupported);
                }
                W::TestSupport::RequireError(mount->ResolveInitialTrackingRoot(candidate, {22}), W::WorldArtifactErrors::Unsupported);
            };
            const auto next = std::move(S::SceneDefinitionBuilder{{100}, {1}}).Build().Value();
            REQUIRE(mount->QueueInitialCell(next, request, {boundary.cancellation, boundary.deadline}, revision).HasValue());
            REQUIRE(geometry);
            const auto waiting = mount->ReconcileInitialCell();
            REQUIRE(waiting.HasValue());
            CHECK_FALSE(waiting.Value());
            CHECK(parent.ActiveScene()->SameIncarnation(old));
            CHECK(mount->Anchors(old).empty());
            REQUIRE(
                parent.OnPhase(S::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 2, .cancellation = boundary.cancellation})
                    .HasValue());
            const auto current = parent.ActiveScene();
            REQUIRE(current);
            REQUIRE(S::FindStreamedSceneInstance(*current, successor, selected, epoch));
            CHECK_FALSE(parent.IsInitialStreamedCandidate(*current));
            CHECK(mount->CaptureInitialGeometry(*current, cameraEntity.object, drawObjects, primitives, *renderer).HasError());
            CHECK(mount->Anchors(*current).empty());
            const auto active = mount->ReconcileInitialCell();
            REQUIRE(active.HasValue());
            REQUIRE(active.Value());
            REQUIRE(mount->Anchors(*current).size() == 1);
            CHECK(mount->Anchors(*current).front() == anchors.front());
            CHECK_FALSE(old.IsCurrent());
            CHECK(geometry->OriginScene().HasValue());
            renderer->Shutdown();
            mount.reset();
            CHECK_FALSE(parent.ActiveScene());
            CHECK(geometry->OriginScene().HasError());
        }
        SECTION("An initial cell conflict rejects the private root and retires the mount operation") {
            const auto old = *parent.ActiveScene();
            S::SceneDefinitionBuilder next{{100}, {1}};
            next.Add({.object = cameraEntity.object});
            const auto conflict = std::move(next).Build().Value();
            CHECK(mount->QueueInitialCell(conflict, request, {boundary.cancellation, boundary.deadline}, {}).HasError());
            CHECK(mount->ReconcileInitialCell().HasError());
            CHECK(parent.ActiveScene()->SameIncarnation(old));
            CHECK(mount->Anchors(old).empty());
            mount.reset();
            CHECK(parent.ActiveScene()->SameIncarnation(old));
        }
        SECTION("Parent cancellation retires the reservation without replacing the active Scene") {
            const auto old = *parent.ActiveScene();
            const auto next = std::move(S::SceneDefinitionBuilder{{100}, {1}}).Build().Value();
            CancellationSource cancellation;
            REQUIRE(mount->QueueInitialCell(next, request, {cancellation.Token(), boundary.deadline}, {}).HasValue());
            cancellation.RequestCancellation();
            REQUIRE(
                parent.OnPhase(S::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 2, .cancellation = boundary.cancellation})
                    .HasValue());
            W::TestSupport::RequireError(mount->ReconcileInitialCell(), S::SceneCellPayloadErrors::Cancelled);
            CHECK(mount->Anchors(old).empty());
            mount.reset();
            CHECK(parent.ActiveScene()->SameIncarnation(old));
        }
        SECTION("Closing the mount while a private root is pending revokes its publication lease") {
            const auto old = *parent.ActiveScene();
            const auto next = std::move(S::SceneDefinitionBuilder{{100}, {1}}).Build().Value();
            REQUIRE(mount->QueueInitialCell(next, request, {boundary.cancellation, boundary.deadline}, {}).HasValue());
            mount.reset();
            REQUIRE(
                parent.OnPhase(S::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 2, .cancellation = boundary.cancellation})
                    .HasValue());
            REQUIRE(parent.TakeOperationError());
            CHECK(lease.Get().HasError());
            CHECK(parent.ActiveScene()->SameIncarnation(old));
        }
        SECTION("Success exposes the exact authored occurrence until actual parent shutdown") {
            REQUIRE(mount->ActivateInitialCell(request, {boundary.cancellation, boundary.deadline}).HasValue());
            const auto live = parent.ActiveScene();
            REQUIRE(live);
            REQUIRE(S::FindStreamedSceneInstance(*live, address, selected, epoch));
            const auto mountedAnchors = mount->Anchors(*live);
            REQUIRE(mountedAnchors.size() == 1);
            CHECK(mountedAnchors.front() == anchors.front());
            CHECK(mount->Anchors({}).empty());
            CHECK_FALSE(S::FindStreamedSceneInstance(*live, address, selected, IdentityFrom<W::PartitionEpoch>(6)));
            CHECK(mount->ActivateInitialCell(request, {boundary.cancellation, boundary.deadline}).HasError());

            const W::OriginRebaseParticipantRequirement participant{IdentityFrom<W::OriginRebaseParticipantId>(1),
                                                                    IdentityFrom<W::OriginRebaseParticipantRevision>(1)};
            S::PrimitiveMeshCache primitives;
            auto foreign = W::OriginFrameOwner::Create(lease.Get().Value()).Value();
            auto wrongRenderer = Render::RenderOriginSceneAdapter::Create(*foreign, {1}, participant, {2, 1, 1}).Value();
            auto renderer = Render::RenderOriginSceneAdapter::Create(mount->Origin(), {2}, participant, {2, 1, 1}).Value();
            const std::array drawObjects{cameraEntity.object};
            W::TestSupport::RequireError(mount->CaptureGeometry(*live, cameraEntity.object, drawObjects, primitives, *wrongRenderer),
                                         W::WorldArtifactErrors::Stale);
            W::TestSupport::RequireError(mount->CaptureGeometry(*live, {88}, drawObjects, primitives, *renderer),
                                         W::WorldArtifactErrors::Unsupported);
            auto rendered = mount->CaptureGeometry(*live, cameraEntity.object, drawObjects, primitives, *renderer);
            REQUIRE(rendered.HasValue());
            auto gameFrame = std::move(rendered).Value();
            const auto renderScene = gameFrame.Scene();
            REQUIRE(renderScene.HasValue());
            REQUIRE(renderScene.Value().IsValid());
            CHECK(renderScene.Value().camera.position == Math::Vec3{});
            CHECK(renderScene.Value().camera.target.x == 1.0F);
            CHECK(renderScene.Value().camera.target.y == 0.0F);
            REQUIRE(renderScene.Value().meshResources.size() == 1);
            REQUIRE(renderScene.Value().instances.size() == 1);
            auto expectedTransform = cameraEntity.localTransform;
            expectedTransform.translation = {};
            CHECK(renderScene.Value().instances.front().localToWorld == expectedTransform.ToMatrix());
            CHECK_FALSE(renderScene.Value().meshResources.front().vertices.empty());

            auto capturedWorld = mount->CaptureGeometry(*live, cameraEntity.object, drawObjects, primitives, *renderer,
                                                        Render::RenderOriginCoordinateBasis::OriginRelative);
            REQUIRE(capturedWorld.HasValue());
            auto worldFrame = std::move(capturedWorld).Value();
            CHECK(worldFrame.Scene().HasError());
            CHECK(gameFrame.OriginScene().HasError());
            const auto worldScene = worldFrame.OriginScene();
            REQUIRE(worldScene.HasValue());
            REQUIRE(worldScene.Value().scene.IsValid());
            CHECK(worldScene.Value().view == Render::RenderViewId{2});
            CHECK(worldScene.Value().origin.Get().Value() == lease.Get().Value());
            CHECK(worldScene.Value().scene.camera.position == Math::Vec3{1.25F, 0, 0});
            CHECK(worldScene.Value().scene.camera.target == Math::Vec3{2.25F, 0, 0});
            expectedTransform.translation = {1.25F, 0, 0};
            CHECK(worldScene.Value().scene.instances.front().localToWorld == expectedTransform.ToMatrix());
            CHECK(worldScene.Value().scene.instances.front().mesh == renderScene.Value().instances.front().mesh);

            const auto liveEntity = live->Find(cameraEntity.object);
            REQUIRE(liveEntity);
            S::SceneCommandBuffer changed;
            auto moved = cameraEntity.localTransform;
            moved.translation.x += 1.0F;
            changed.SetLocalTransform(*liveEntity, moved);
            REQUIRE(parent.QueueStructuralCommands(std::move(changed)).HasValue());
            const CancellationToken cancellation;
            REQUIRE(parent.OnPhase(S::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 2, .cancellation = cancellation})
                        .HasValue());
            REQUIRE(parent.ActiveScene());
            W::TestSupport::RequireError(mount->CaptureGeometry(*parent.ActiveScene(), cameraEntity.object, drawObjects, primitives,
                                                                *renderer),
                                         W::WorldArtifactErrors::Stale);
            renderer->Shutdown();
            mount.reset();
            CHECK_FALSE(parent.ActiveScene());
            CHECK_FALSE(live->IsCurrent());
            CHECK(lease.Get().HasError());
            CHECK(gameFrame.Scene().HasError());
            CHECK(worldScene.Value().origin.Get().HasError());
            CHECK(worldFrame.OriginScene().HasError());
        }
    }

    /** @brief Runs the product Play factory through cold preparation and authentic live frames. */
    static void CheckEditorLiveCell(Fixture &world, const std::filesystem::path &project, const S::RuntimeSceneDefinition &definition,
                                    Application::Internal::EditorProjectScenePolicy policy,
                                    Application::Internal::MountedWorldCellNavigationReadiness navigation) {
        using namespace Application::Internal;
        auto limits = Limits();
        limits.cell.maximumPayloads = 2;  // CoreECS plus an optional NavigationMesh membership block.
        const auto published = world.Publish(limits);
        if (published.HasError())
            UNSCOPED_INFO(published.ErrorValue().message);
        REQUIRE(published.HasValue());
        for (const auto &finding : published.Value().validation->findings)
            UNSCOPED_INFO(finding.cause.message);
        REQUIRE(published.Value().publication);
        const auto release = world.Release();
        const auto selected = world.inputs.front().expected;
        const CommittedWorldInitialCell demand{selected.cell, {900}, selected.scene, selected.revision};
        DeterministicClock clock;
        JobSystem jobs{JobSystemConfig{1, 8}};
        std::unique_ptr<Editor::EditorPlaySceneHost> play;
        const auto runtime = S::RuntimeHost::Create(clock).Value();
        policy.world.emplace(
            EditorPlayWorldSelection{.projectRoot = project,
                                     .committed = {.targetRoot = release.targetRoot,
                                                   .target = release.target,
                                                   .manifest = published.Value().publication->manifestDigest,
                                                   .index = release.indexArtifact,
                                                   .dependencies = release.dependencyArtifact,
                                                   .liveCell = CommittedWorldLiveSelection{{20'000'000, 1'000'000, 16}, demand}},
                                     .dispatch = runtime->DispatchSource()});
        auto created = MakeEditorProjectSceneFactory(jobs, std::move(policy))(project, Editor::EditorFirstPlayScene, definition);
        if (created.HasError())
            UNSCOPED_INFO(created.ErrorValue().message);
        REQUIRE(created.HasValue());
        play = std::move(created).Value();
        const CancellationToken cancellation;
        REQUIRE(play->Scenes().Startup(cancellation).HasValue());
        REQUIRE(play->QueueInitialScene(definition, nullptr, cancellation).HasValue());
        const auto startupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (std::uint64_t frame = 1; !play->Scenes().ActiveScene() && std::chrono::steady_clock::now() < startupDeadline; ++frame) {
            REQUIRE(play->CommitSceneChanges({.frameNumber = frame, .cancellation = cancellation}).HasValue());
            if (const auto failure = play->Scenes().TakeOperationError()) {
                FAIL(failure->message);
            }
            if (!play->Scenes().ActiveScene())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(play->Scenes().ActiveScene());
        W::TestSupport::RequireError(play->OnRuntimeSceneCutoff({.frameNumber = 2, .cancellation = cancellation}),
                                     W::WorldArtifactErrors::Stale);
        W::TestSupport::RequireError(InspectEditorPlayMountedCell(*play, demand.occurrence), W::WorldArtifactErrors::Stale);
        REQUIRE(runtime->AddParticipant(std::make_unique<EditorWorldCutoff>(*play)).HasValue());
        REQUIRE(runtime->Startup().HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            clock.Advance(Duration::FromMilliseconds(17));
            const auto frame = runtime->RunFrame();
            if (frame.HasError())
                UNSCOPED_INFO(frame.ErrorValue().message);
            REQUIRE(frame.HasValue());
            const auto readiness = InspectEditorPlayMountedCell(*play, demand.occurrence);
            if (readiness.HasValue() && readiness.Value().geometryVisible)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        const auto readiness = InspectEditorPlayMountedCell(*play, demand.occurrence);
        REQUIRE(readiness.HasValue());
        CHECK(readiness.Value().geometryVisible);
        CHECK(readiness.Value().navigation == navigation);
        CHECK(play->Scenes().ActiveScene()->Find(world.entities.front().object));
        runtime->Shutdown();
        W::TestSupport::RequireError(InspectEditorPlayMountedCell(*play, demand.occurrence), W::WorldArtifactErrors::Stale);
    }

    TEST_CASE("Editor Play publishes live mounted cells only at the real runtime structural cutoff",
              "[world_cook][native][mounted_world][editor_play][live_cell]") {
        Fixture world{false, false};
        T::TempDir project;
        REQUIRE(std::filesystem::create_directory(project.path / "assets"));
        const auto definition = S::SceneDefinitionBuilder{{200}, {1}}.Build();
        REQUIRE(definition.HasValue());
        CheckEditorLiveCell(world, project.path, definition.Value(), {},
                            Application::Internal::MountedWorldCellNavigationReadiness::NotDeclared);
    }

#if HORO_TEST_NAVIGATION_NATIVE
    TEST_CASE("Editor Play admits verified mounted Detour coverage with its visible cell",
              "[world_cook][native][mounted_world][editor_play][live_cell][navigation]") {
        using namespace Application::Internal;
        using Application::CharacterSceneProductTest::NavigationProductFixture;
        NavigationProductFixture project;
        project.Publish(NavigationProductFixture::Input::Valid);
        const SceneCookSelection cook{project.project.cooked.path, project.project.request.assets.target};
        const auto assets = CommittedSceneAssets::Capture(project.project.files, cook, project.project.registry.Snapshot());
        REQUIRE(assets.HasValue());
        const auto definition = assets.Value()->DecodeScene(project.project.sceneId);
        REQUIRE(definition.HasValue());
        const auto *navigation = assets.Value()->Find(project.definitionId);
        REQUIRE(navigation);
        const auto tiles = Navigation::DecodeNavigationCookedTileSet(navigation->payload, 1024U * 1024U);
        REQUIRE(tiles.HasValue());
        REQUIRE(tiles.Value().tiles.size() == 1);

        const auto generation = A::ResolveCurrentCookGeneration(project.project.files, cook.targetRoot);
        REQUIRE(generation.HasValue());
        auto contents = A::ReadCookGenerationContents(project.project.files, generation.Value(), 1024U * 1024U);
        REQUIRE(contents.HasValue());
        const auto &entries = contents.Value().entries;
        const auto entry = std::ranges::find(entries, project.definitionId, &A::AssetCookManifestEntry::assetId);
        REQUIRE(entry != entries.end());

        Fixture world{true, false};
        world.packages = Fixture::Packages(true, project.definitionId);
        world.objects.back() = BaseObjectBinding{Endpoint(3), project.definitionId, entry->artifactHash};
        world.baseArtifacts.front() = std::move(contents.Value().artifacts[static_cast<std::size_t>(entry - entries.begin())]);
        world.navigationCells.push_back(
            {world.inputs.front().expected.cell, {project.definitionId, entry->artifactHash, {tiles.Value().tiles.front()->Key()}}});

        CheckEditorLiveCell(world, project.project.project.dir.path, definition.Value(),
                            {.cooked = cook, .playNavigationScene = project.project.sceneId}, MountedWorldCellNavigationReadiness::Ready);
    }
#endif

    TEST_CASE("Editor Play mounts an explicit committed World for one project and revokes its origin on Stop",
              "[world_cook][native][mounted_world][editor_play]") {
        using namespace Application::Internal;
        Fixture world{false, false};
        auto &object = world.entities.front();
        object.components.camera = S::CameraComponent{};
        object.primitiveMesh = S::PrimitiveMeshDescriptor::Defaults(S::PrimitiveMeshType::Box);
        const std::array anchors{S::SceneCellWorldAnchor{object.object,
                                                         {9'007'199'254'740'993, -400, 700},
                                                         std::array<std::int64_t, 3>{9'007'199'254'741'993, -400, 700}}};
        world.inputs.front().source.worldAnchors = anchors;
        const auto published = world.Publish();
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().publication);
        const auto release = world.Release();
        T::TempDir project;
        REQUIRE(std::filesystem::create_directory(project.path / "assets"));
        const auto origin = Math::WorldCoordinate64::FromMillimeters(9'007'199'254'740'993, -400, 700);
        EditorProjectScenePolicy policy;
        policy.world.emplace(EditorPlayWorldSelection{project.path,
                                                      {release.targetRoot, release.target, published.Value().publication->manifestDigest,
                                                       release.indexArtifact, release.dependencyArtifact, origin}});
        JobSystem jobs{JobSystemConfig{1, 8}};
        const auto definition = S::SceneDefinitionBuilder{{200}, {1}}.Build();
        REQUIRE(definition.HasValue());

        SECTION("Actual Scene factory publishes the mount only with ready Play") {
            auto factory = MakeEditorProjectSceneFactory(jobs, policy);
            auto created = factory(project.path, Editor::EditorFirstPlayScene, definition.Value());
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            const CancellationToken cancellation;
            REQUIRE(host->Scenes().Startup(cancellation).HasValue());
            REQUIRE(host->QueueInitialScene(definition.Value(), nullptr, cancellation).HasValue());
            CHECK_FALSE(ObserveEditorPlayMountedWorld(*host));
            REQUIRE(host->CommitSceneChanges({.frameNumber = 1, .cancellation = cancellation}).HasValue());
            REQUIRE(host->Scenes().ActiveScene());
            auto lease = ObserveEditorPlayMountedWorld(*host);
            REQUIRE(lease);
            CHECK(lease->Get().Value().Origin() == origin);
            host->Shutdown();
            CHECK_FALSE(ObserveEditorPlayMountedWorld(*host));
            CHECK(lease->Get().HasError());
        }
        SECTION("A selected World cell publishes the exact canonical graph input and retires it with Play") {
            const auto selected = world.inputs.front().expected;
            policy.world->committed.initialCell.emplace(CommittedWorldInitialCell{selected.cell, {900}, selected.scene, selected.revision});
            auto publication = std::make_shared<EditorMountedWorldViewport>();
            policy.world->viewport.emplace(EditorMountedWorldViewportSelection{object.object, {object.object}, publication, [] {
                return Result<Render::RenderViewId>::Success({43});
            }});
            S::PrimitiveMeshCache primitives;
            auto factory = MakeEditorProjectSceneFactory(jobs, policy);
            auto created = factory(project.path, Editor::EditorFirstPlayScene, definition.Value());
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            REQUIRE(host->UsesMountedViewport());
            host->BindViewportPrimitiveCache(&primitives);
            const CancellationToken cancellation;
            REQUIRE(host->Scenes().Startup(cancellation).HasValue());
            REQUIRE(host->QueueInitialScene(definition.Value(), nullptr, cancellation).HasValue());
            CHECK_FALSE(publication->Current().packet);
            const auto committed = host->CommitSceneChanges({.frameNumber = 1, .cancellation = cancellation});
            INFO((committed.HasError() ? committed.ErrorValue().code.Value() + ": " + committed.ErrorValue().message : "Committed"));
            REQUIRE(committed.HasValue());
            REQUIRE(host->Scenes().ActiveScene());
            CHECK(host->Scenes().ActiveScene()->AssetRegistryRevision().value != 0);
            auto packet = publication->Current().packet;
            REQUIRE(packet);
            CHECK(publication->Current().owner == Editor::EditorFirstPlayScene);
            CHECK(publication->Current().view.value == 43);
            const auto view = packet->Scene();
            REQUIRE(view.HasValue());
            CHECK(view.Value().camera.target.x == 1.0F);
            CHECK(view.Value().instances.size() == 1);
            CHECK(view.Value().meshResources.size() == 1);
            publication->Withdraw({7});
            CHECK(publication->Current().packet == packet);

            const auto updateRotation = [&](const float radians, const std::uint64_t frame) {
                const auto active = *host->Scenes().ActiveScene();
                const auto entity = active.Find(object.object);
                REQUIRE(entity);
                const auto entityState = active.Get(*entity);
                REQUIRE(entityState.HasValue());
                auto transform = *entityState.Value().localTransform;
                transform.rotation = Math::Quaternion::FromAxisAngle({1, 0, 0}, radians);
                S::SceneCommandBuffer commands;
                commands.SetLocalTransform(*entity, transform);
                REQUIRE(host->Scenes().QueueStructuralCommands(std::move(commands)).HasValue());
                REQUIRE(host->CommitSceneChanges({.frameNumber = frame, .cancellation = cancellation}).HasValue());
                return publication->Current().packet;
            };
            const auto second = updateRotation(Math::Pi / 2.0F, 2);
            REQUIRE(second);
            REQUIRE(second->IsSuccessorOf(*packet));
            REQUIRE(second->Scene().HasValue());
            CHECK(second->Scene().Value().camera.up.z > 0.99F);
            CHECK(packet->Scene().HasError());
            const auto third = updateRotation(Math::Pi / 4.0F, 3);
            REQUIRE(third);
            REQUIRE(third->IsSuccessorOf(*second));
            REQUIRE(third->Scene().HasValue());
            CHECK(third->Scene().Value().camera.up.y > 0.7F);
            CHECK(second->Scene().HasError());
            CHECK(publication->Current().owner == Editor::EditorFirstPlayScene);
            CHECK(publication->Current().view.value == 43);
            CHECK(publication->Replace(packet, third).HasError());
            CHECK(publication->Replace(third, second).HasError());
            REQUIRE(host->CommitSceneChanges({.frameNumber = 4, .cancellation = cancellation}).HasValue());
            CHECK(publication->Current().packet == third);

            const auto active = *host->Scenes().ActiveScene();
            const auto entity = active.Find(object.object);
            REQUIRE(entity);
            const auto entityState = active.Get(*entity);
            REQUIRE(entityState.HasValue());
            auto changed = *entityState.Value().localTransform;
            changed.translation.x += 1.0F;
            S::SceneCommandBuffer moveOutsideAuthority;
            moveOutsideAuthority.SetLocalTransform(*entity, changed);
            REQUIRE(host->Scenes().QueueStructuralCommands(std::move(moveOutsideAuthority)).HasValue());
            W::TestSupport::RequireError(host->CommitSceneChanges({.frameNumber = 5, .cancellation = cancellation}),
                                         W::WorldArtifactErrors::Stale);
            CHECK(publication->Current().packet == third);
            CHECK(third->Scene().HasError());
            host->Shutdown();
            CHECK_FALSE(publication->Current().packet);
            CHECK(packet->Scene().HasError());
        }
        SECTION("A different workspace cannot reuse the launch World") {
            T::TempDir foreign;
            auto factory = MakeEditorProjectSceneFactory(jobs, policy);
            W::TestSupport::RequireError(factory(foreign.path, Editor::EditorFirstPlayScene, definition.Value()),
                                         W::WorldArtifactErrors::Stale);
        }
        SECTION("A request for a cell cannot leak into the Editor float viewport") {
            const auto selected = world.inputs.front().expected;
            policy.world->committed.initialCell.emplace(CommittedWorldInitialCell{selected.cell, {900}, selected.scene, selected.revision});
            auto factory = MakeEditorProjectSceneFactory(jobs, policy);
            W::TestSupport::RequireError(factory(project.path, Editor::EditorFirstPlayScene, definition.Value()),
                                         W::WorldArtifactErrors::Unsupported);
        }
        SECTION("A superseded committed manifest rejects before any Scene is published") {
            policy.world->committed.manifest = {};
            auto factory = MakeEditorProjectSceneFactory(jobs, policy);
            auto created = factory(project.path, Editor::EditorFirstPlayScene, definition.Value());
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            const CancellationToken cancellation;
            REQUIRE(host->Scenes().Startup(cancellation).HasValue());
            W::TestSupport::RequireError(host->QueueInitialScene(definition.Value(), nullptr, cancellation), W::WorldArtifactErrors::Stale);
            CHECK_FALSE(host->Scenes().ActiveScene());
            CHECK_FALSE(ObserveEditorPlayMountedWorld(*host));
        }
    }

    TEST_CASE("Cook test directory collisions preserve existing roots and bound exclusive admission", "[world_cook][native]") {
        T::TempDir parent;
        const auto existing = parent.path / "existing";
        REQUIRE(std::filesystem::create_directory(existing));
        const auto sentinel = existing / "sentinel.txt";
        {
            std::ofstream stream(sentinel);
            stream << "existing owner";
            REQUIRE(stream.good());
        }
        std::size_t attempts{};
        SECTION("Collision retries into a distinct exclusively created child") {
            const auto fresh = parent.path / "fresh";
            const auto created = A::CookTestValues::CreateCookTestDirectoryFromCandidates([&](const std::size_t attempt) {
                ++attempts;
                return attempt == 0 ? existing : fresh;
            }, true);
            CHECK(attempts == 2);
            CHECK(created == std::filesystem::canonical(fresh));
            CHECK(T::ReadText(sentinel) == "existing owner");
            // This enclosing exclusively owned parent also owns the newly admitted child.
            CHECK(std::filesystem::remove(created));
            CHECK(std::filesystem::is_directory(existing));
            CHECK(T::ReadText(sentinel) == "existing owner");
        }
        SECTION("Repeated collisions exhaust the finite bound without adopting an existing root") {
            REQUIRE_THROWS_AS(A::CookTestValues::CreateCookTestDirectoryFromCandidates(
                                  [&](const std::size_t) {
                ++attempts;
                return existing;
            }, false),
                              std::filesystem::filesystem_error);
            CHECK(attempts == A::CookTestValues::MaximumCookTestDirectoryAttempts);
            CHECK(T::ReadText(sentinel) == "existing owner");
        }
        SECTION("Non-collision filesystem errors surface without retry") {
            REQUIRE_THROWS_AS(A::CookTestValues::CreateCookTestDirectoryFromCandidates(
                                  [&](const std::size_t) {
                ++attempts;
                return parent.path / "missing" / "child";
            }, false),
                              std::filesystem::filesystem_error);
            CHECK(attempts == 1);
            CHECK(T::ReadText(sentinel) == "existing owner");
        }
    }

    TEST_CASE("World committed Scene adoption and report lease transport allocate no new storage", "[world_cook][native]") {
        Fixture fixture;
        const auto source = fixture.Source();
        auto prepared = fixture.cache.Prepare(source.partition, source.cells, {}, source.settings, 0, Limits().scene.semantic);
        REQUIRE(prepared.HasValue());
        auto receipt = std::move(prepared).Value();
        const auto reportLease = receipt.ReportLease();
        const auto validation = std::make_shared<const ValidationReport>();
        const std::vector<std::vector<std::uint8_t>> payloads{T::MakePayload(Asset(8), 16)};
        const std::array entries{T::Entry(Asset(8), payloads.front())};
        auto writer = fixture.files.native.TryAcquireExclusive(fixture.directory.path / ".cook-writer.lock", "adoption fixture");
        REQUIRE(writer.HasValue());
        auto policy = T::Policy(fixture.files, "10000000-0000-0000-0000-000000000002");
        policy.writerLease = &writer.Value();
        policy.beforeCommit = [&] {
            return receipt.Reserve();
        };
        Tests::AllocationProbe::Measurement measured{};
        bool committed{};
        policy.afterCommit = [&](const A::AssetCookGeneration &) {
            Tests::AllocationProbe::ScopedMeasurement scope;
            receipt.Adopt();
            const auto copiedLease = receipt.ReportLease();
            auto result = Result<PublicationReport>::Success({validation, copiedLease, {}});
            auto copied = result;
            auto moved = std::move(copied);
            committed = moved.Value().scene == reportLease;
            measured = scope.Snapshot();
        };
        auto published =
            A::PublishCookGeneration(fixture.directory.path, T::Target("headless-null"), entries, payloads, Limits().assets, policy);
        REQUIRE(published.HasValue());
        CHECK(committed);
        CHECK(measured.requests == 0);
        CHECK(fixture.cache.Revision() == 1);
        CHECK(reportLease->cooked == 2);
        CHECK(receipt.Report().cells.size() == 2);
    }

    TEST_CASE("World cook publishes actual Scene cells hard dependencies and complete release bytes", "[world_cook][native]") {
        Fixture fixture;
        auto published = fixture.Publish();
        if (published.HasError())
            INFO(published.ErrorValue().message);
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().validation->CanPublish());
        REQUIRE(published.Value().publication);
        CHECK(published.Value().scene->cooked == 2);
        CHECK(fixture.cache.Revision() == 1);
        const auto contents = Contents(published.Value());
        REQUIRE(contents.entries.size() == 4);
        const auto indexEntry = std::ranges::find(contents.entries, Asset(6), &A::AssetCookManifestEntry::assetId);
        REQUIRE(indexEntry != contents.entries.end());
        const auto position = static_cast<std::size_t>(indexEntry - contents.entries.begin());
        auto envelope = A::DecodeCookedArtifact(contents.artifacts[position]);
        REQUIRE(envelope.HasValue());
        auto decoded = W::DecodeWorldIndexArtifact(std::as_bytes(std::span{envelope.Value().payload}), Limits().index);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().descriptor.Cells()[0].id == W::CandidateTestSupport::Cell(-1));
        CHECK(decoded.Value().cells.size() == 2);
    }

    TEST_CASE("World clean and incremental complete inventories are byte stable under input membership order", "[world_cook][native]") {
        Fixture fixture;
        auto first = fixture.Publish();
        REQUIRE(first.HasValue());
        const auto clean = Contents(first.Value());
        std::ranges::reverse(fixture.inputs);
        std::ranges::reverse(fixture.objects);
        auto second = fixture.Publish();
        REQUIRE(second.HasValue());
        CHECK(second.Value().scene->cooked == 0);
        CHECK(second.Value().scene->reused == 2);
        const auto incremental = Contents(second.Value());
        CHECK(clean.artifacts == incremental.artifacts);
        CHECK(first.Value().publication->manifestDigest == second.Value().publication->manifestDigest);
        S::IncrementalSceneCellCook fresh{{8, 8, 16, 8, 65536, 131072}};
        // A separate owner cooks the same reordered capture without any cache history.
        const auto source = fixture.Source();
        auto cold = fresh.Cook(source.partition, source.cells, {}, source.settings, 0, Limits().scene.semantic);
        REQUIRE(cold.HasValue());
        CHECK(cold.Value().cells[0].key == second.Value().scene->cells[0].key);
    }

    TEST_CASE("World precommit encoding freshness and filesystem failures preserve exact previous cache and pointer",
              "[world_cook][native]") {
        Fixture fixture;
        auto first = fixture.Publish();
        REQUIRE(first.HasValue());
        const auto pointer = T::ReadText(fixture.directory.path / "current.json");
        const auto oldLease = first.Value().scene->cells[0].payload;
        fixture.entities[0].localTransform.translation.x = 4;
        SECTION("Encoding bound") {
            auto limits = Limits();
            limits.scene.maximumEncodedBytes = 1;
            CHECK(fixture.Publish(limits).HasError());
        }
        SECTION("Source replacement at final freshness") {
            CHECK(fixture
                      .Publish(Limits(), [] {
                return Result<void>::Failure(MakeError(Errors::Stale));
            }).HasError());
        }
        SECTION("Cancellation after staging") {
            CancellationSource cancel;
            CHECK(fixture
                      .Publish(Limits(),
                               [&] {
                cancel.RequestCancellation();
                return Result<void>::Success();
            }, cancel.Token())
                      .HasError());
        }
        SECTION("Shutdown before cache reservation") {
            CHECK(fixture
                      .Publish(Limits(), [&] {
                fixture.cache.Shutdown();
                return Result<void>::Success();
            }).HasError());
        }
        SECTION("Durable generation write") {
            fixture.files.fault = T::PublicationFault::ArtifactWrite;
            CHECK(fixture.Publish().HasError());
        }
        CHECK(fixture.cache.Revision() == 1);
        CHECK(T::ReadText(fixture.directory.path / "current.json") == pointer);
        CHECK(oldLease->Definition().Entities()[0].localTransform.translation.x == 0);
    }

    TEST_CASE("World committed durability failure still adopts the exact prepared Scene state", "[world_cook][native]") {
        Fixture fixture;
        REQUIRE(fixture.Publish().HasValue());
        fixture.entities[0].localTransform.translation.x = 7;
        fixture.files.fault = T::PublicationFault::PointerSync;
        auto result = fixture.Publish();
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().publication);
        CHECK(result.Value().publication->durabilityError.has_value());
        CHECK(fixture.cache.Revision() == 2);
        CHECK(result.Value().scene->cells[0].payload->Definition().Entities()[0].localTransform.translation.x == 7);
        auto current = A::ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, fixture.directory.path);
        REQUIRE(current.HasValue());
        CHECK(current.Value().manifestDigest == result.Value().publication->manifestDigest);
    }

    TEST_CASE("World package omission and object content mismatch publish no partial generation", "[world_cook][native]") {
        Fixture fixture;
        SECTION("Missing actual authored binding") {
            fixture.objects[0] = SpatialObjectBinding{Endpoint(1), W::CandidateTestSupport::Cell(-1), {99}};
        }
        SECTION("Missing release inventory asset") {
            auto id = A::AssetChunkId::Parse("base");
            REQUIRE(id.HasValue());
            const std::array chunks{A::AssetChunkDefinition{std::move(id).Value(),
                                                            A::AssetChunkKind::Base,
                                                            {Asset(4), Asset(5), Asset(6), Asset(7), Asset(99)}}};
            auto plan = A::AssetChunkPlan::Create(chunks);
            REQUIRE(plan.HasValue());
            fixture.packages = std::move(plan).Value();
        }
        auto result = fixture.Publish();
        REQUIRE(result.HasValue());
        CHECK_FALSE(result.Value().validation->CanPublish());
        CHECK_FALSE(result.Value().publication);
        CHECK(fixture.cache.Revision() == 0);
        CHECK_FALSE(std::filesystem::exists(fixture.directory.path / "current.json"));
    }

    TEST_CASE("World artifact admission emits owned BuildOutput causes for actual missing corrupt and version bytes",
              "[world_cook][native]") {
        Fixture fixture;
        auto first = fixture.Publish();
        REQUIRE(first.HasValue());
        Detached outputs{Contents(first.Value())};
        SECTION("Missing cooked cell") {
            outputs.cells.pop_back();
        }
        SECTION("Corrupt actual cell bytes") {
            outputs.cells.front().second.back() ^= std::byte{1};
        }
        SECTION("Unsupported index wire version") {
            outputs.index[8] = std::byte{2};
        }
        SECTION("Corrupt dependency sibling") {
            outputs.dependencies.back() ^= std::byte{1};
        }
        SECTION("Captured authored source changed") {
            fixture.entities.front().localTransform.translation.y = 3;
        }
        auto validation = outputs.Validate(fixture);
        REQUIRE(validation.HasValue());
        REQUIRE_FALSE(validation.Value().CanPublish());
        REQUIRE_FALSE(validation.Value().findings.empty());
        CHECK_FALSE(validation.Value().findings.front().cause.code.Value().empty());
        CHECK(validation.Value().findings.front().diagnostic.result == BuildOutputResult::Failed);
        CHECK(fixture.cache.Revision() == 1);
        CHECK(A::ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, fixture.directory.path).Value().manifestDigest ==
              first.Value().publication->manifestDigest);
    }

    TEST_CASE("World source shutdown after final reservation cannot roll back committed publication", "[world_cook][native]") {
        Fixture fixture;
        auto lease = fixture.files.native.TryAcquireExclusive(fixture.directory.path / ".cook-writer.lock", "adoption fixture");
        REQUIRE(lease.HasValue());
        auto policy = T::Policy(fixture.files, "10000000-0000-0000-0000-000000000001");
        policy.writerLease = &lease.Value();
        policy.beforeCommit = [] {
            return Result<void>::Success();
        };
        bool adopted{};
        policy.afterCommit = [&](const A::AssetCookGeneration &) {
            adopted = true;
            fixture.cache.Shutdown();
        };
        auto result = PublishWorldCook(fixture.cache, fixture.Source(), fixture.Release(), Limits(), policy);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().publication);
        CHECK(adopted);
        CHECK(fixture.cache.Revision() == 1);
        CHECK(result.Value().scene->cells.size() == 2);
        const auto source = fixture.Source();
        CHECK(fixture.cache.Cook(source.partition, source.cells, {}, source.settings, 1, Limits().scene.semantic).HasError());
    }

    TEST_CASE("World nonspatial placement requires actual verified provider content in the complete Base inventory",
              "[world_cook][native]") {
        Fixture fixture{true};
        SECTION("Actual provider bytes retained") {
            auto result = fixture.Publish();
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().validation->CanPublish());
            const auto contents = Contents(result.Value());
            REQUIRE(contents.entries.size() == 5);
            const auto base = std::ranges::find(contents.entries, Asset(8), &A::AssetCookManifestEntry::assetId);
            REQUIRE(base != contents.entries.end());
            CHECK(contents.artifacts[static_cast<std::size_t>(base - contents.entries.begin())] == fixture.baseArtifacts.front());
        }
        SECTION("Missing base bytes cannot become metadata-only success") {
            fixture.baseArtifacts.clear();
            auto result = fixture.Publish();
            REQUIRE(result.HasValue());
            CHECK_FALSE(result.Value().validation->CanPublish());
            CHECK_FALSE(result.Value().publication);
            CHECK(fixture.cache.Revision() == 0);
        }
        SECTION("Replaced provider envelope fails independent captured hash") {
            fixture.baseArtifacts.front() = T::MakePayload(Asset(8), 16, 0x99);
            auto result = fixture.Publish();
            REQUIRE(result.HasValue());
            CHECK_FALSE(result.Value().validation->CanPublish());
            CHECK_FALSE(result.Value().publication);
            CHECK(fixture.cache.Revision() == 0);
        }
    }

}  // namespace Horo::WorldCook
