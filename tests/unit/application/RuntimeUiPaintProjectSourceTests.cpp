#include "EditorProjectSceneHost.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "RuntimeUiPaintHostTestFixture.h"
#include "RuntimeUiPaintProjectSources.h"
#include "UiPaintPackageTestFixture.h"

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace Horo;
using namespace Horo::Tests::UiPaintHostTesting;

namespace {
    /** @brief Pumps the actual Scene owner to its exact requested publication or retained operation error. */
    std::optional<Error> FinishScenePublication(Runtime::RuntimeSceneService &scenes, const Runtime::SceneDefinitionRevision revision,
                                                const auto &advance) {
        using namespace std::chrono_literals;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            if (const auto advanced = advance(); advanced.HasError())
                return advanced.ErrorValue();
            if (auto failure = scenes.TakeOperationError())
                return failure;
            const auto scene = scenes.ActiveScene();
            if (scene && scene->DefinitionRevision() == revision)
                return std::nullopt;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL("Scene preparation did not publish the requested revision or reach a terminal error");
        return std::nullopt;
    }

    /** @brief Runs the real application project participant over canonical cooked artifact envelopes; tracking is CPU-only. */
    struct ProjectUi final {
        std::unique_ptr<Render::RenderFrontend> frontend{Frontend()};
        JobSystem jobs{JobSystemConfig{2, 32}};
        Assets::AssetRegistry registry;
        Assets::MemoryAssetProvider provider;
        Assets::AssetLoadService assetLoads{jobs, provider};
        UiRuntimeAssetLoadService uiLoads{registry, assetLoads};
        Runtime::RuntimeSceneService scenes{registry, assetLoads, {}, Runtime::SceneRuntimeId{40}};
        CancellationToken cancellation;
        DeterministicClock clock;
        Render::FramebufferExtent output{100, 100};
        std::unique_ptr<Runtime::RuntimeHost> host{Take(Runtime::RuntimeHost::Create(clock))};
        RuntimeUiPaintPort port;
        std::function<void(std::uint64_t)> observeViewport;
        std::shared_ptr<RuntimeUiPaintProjectSources> sources;
        const Assets::AssetCookTargetId target{Take(Assets::AssetCookTargetId::Parse("headless-null"))};
        const Assets::AssetId asset{[] {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = 31;
            return Assets::AssetId::FromBytes(bytes);
        }()};
        std::string sourceJson;
        std::vector<std::uint8_t> encodedArtifact;
        UiCanvasAssetReference reference{asset, Source::Stable<UiDocumentId>(1), Source::Stable<UiCanvasId>(2),
                                         Source::Revision<UiDocumentRevision>(1)};

        explicit ProjectUi(const bool paint = true, const bool kernel = true, const std::filesystem::path &packagedRoot = {},
                           const UiRenderMode renderMode = UiRenderMode::ScreenSpaceOverlay,
                           const UiCanvasReferenceResolution resolution = {}, const UiCanvasScaleFactor fontScale = {},
                           const std::filesystem::path &sceneCanvasRoot = {})
            : frontend(Frontend(paint)) {
            const auto type = Take(Assets::AssetTypeId::Parse("runtime.ui.document"));
            REQUIRE(registry
                        .Publish({{asset, type, Take(ProjectPath::Parse("assets/project.ui")),
                                   Take(ProjectPath::Parse("assets/project.ui.horo"))}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            UiDocumentBuilder builder{reference.document, reference.minimumRevision};
            REQUIRE(builder
                        .AddCanvas({.id = reference.canvas,
                                    .rootElement = Source::Stable<UiElementId>(3),
                                    .renderMode = renderMode,
                                    .referenceResolution = resolution,
                                    .presentation = {.fontScale = fontScale}})
                        .HasValue());
            auto root = Take(CreateUiElement(UiElementKind::Canvas, Source::Stable<UiElementId>(3), {}));
            root.presentation->layout.width = UiLength::Percent(64);
            root.presentation->layout.height = UiLength::Percent(64);
            REQUIRE(builder.AddElement(std::move(root)).HasValue());
            auto panel = Take(CreateUiElement(UiElementKind::Panel, Source::Stable<UiElementId>(4), Source::Stable<UiElementId>(3)));
            panel.presentation->layout.width = UiLength::Percent(32);
            panel.presentation->layout.height = UiLength::Percent(32);
            panel.presentation->inlineStyle.push_back(
                {CoreUiStylePropertyId(UiCoreStyleProperty::Fill), UiStyleValueSource::Literal(UiStyleColor{1, 0, 0, 0.5F}), false});
            REQUIRE(builder.AddElement(std::move(panel)).HasValue());
            const auto document = Take(std::move(builder).Build());
            sourceJson = Take(SerializeUiDocument(document));
            const auto cooked = Take(CookedUiDocument::Cook(document));
            Assets::AssetCookArtifact envelope;
            envelope.id = asset;
            envelope.type = type;
            envelope.target = target;
            envelope.payload.assign(cooked.Payload().begin(), cooked.Payload().end());
            envelope.payloadDigest = ComputeSha256(std::as_bytes(std::span{envelope.payload}));
            encodedArtifact = Take(Assets::EncodeCookedArtifact(envelope));
            provider.Insert(asset, encodedArtifact);
            auto composition =
                Take(RuntimeUiPaintHost::Compose(*frontend, host->DispatchSource(), UiSceneReconciliationLimits{Source::Owner()}));
            port = composition.port;
            REQUIRE(port.UpdateOutput({{100, 100}, {1, 1}, {}}).HasValue());
            auto allocator = std::make_shared<UiElementSlotAllocator>(Take(UiElementSlotAllocator::Create(Source::Owner())));
            RuntimeUiPaintProjectPolicy policy;
            if (kernel)
                policy.kernel = std::make_shared<Render::CookedUiPaintKernel>();  // Explicit CPU tracking seam, never native cook proof.
            if (!packagedRoot.empty())
                policy.source = RuntimeUiPaintPackagedAssets{packagedRoot, Render::ShaderTargetBackend::Metal};
            if (!sceneCanvasRoot.empty())
                policy.vulkanSceneCanvasAssets = sceneCanvasRoot;
            sources = Take(RuntimeUiPaintProjectSources::Create(port, std::move(allocator), std::move(policy)));
            REQUIRE(scenes.AddActivationParticipant(sources->MakeParticipant(scenes, registry, uiLoads, target)).HasValue());
            REQUIRE(scenes.Startup({}).HasValue());
            REQUIRE(host->AddParticipant(std::move(composition.participant)).HasValue());
            REQUIRE(host->AddParticipant(std::make_unique<Renderer>(*frontend, port, output, [this](const std::uint64_t frame) {
                if (observeViewport)
                    observeViewport(frame);
            })).HasValue());
            REQUIRE(host->Startup().HasValue());
        }

        ~ProjectUi() {
            auto &lease = Tests::RenderFrontendTesting::lifecycleState.graphLease;
            if (lease) {
                lease->Release();
                lease = nullptr;
            }
            scenes.Shutdown();
            host->Shutdown();
            static_cast<void>(port.CollectRetired());
        }

        Runtime::RuntimeSceneDefinition Definition(const std::uint64_t revision, const std::size_t canvases) {
            Runtime::SceneDefinitionBuilder builder{{7}, {revision}};
            for (std::size_t index = 0; index < std::max(std::size_t{1}, canvases); ++index) {
                Runtime::RuntimeEntityDefinition entity{.object = {index + 1}};
                if (index < canvases)
                    entity.components.uiCanvas = Runtime::UiCanvasComponent{reference};
                builder.Add(std::move(entity));
            }
            return Take(std::move(builder).Build());
        }

        std::optional<Error> Publish(const std::uint64_t revision, const std::size_t canvases) {
            const auto queued = scenes.QueuePreparation(Definition(revision, canvases));
            if (queued.HasError()) {
                const auto &error = queued.ErrorValue();
                UNSCOPED_INFO("Scene queue failure: " << error.domain.Value() << '/' << error.code.Value() << ": " << error.message);
                return error;
            }
            auto failure = FinishScenePublication(scenes, {revision}, [&] {
                return scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                      {.frameNumber = revision, .cancellation = cancellation});
            });
            if (failure) {
                UNSCOPED_INFO("Scene publication failure: " << failure->domain.Value() << '/' << failure->code.Value() << ": "
                                                            << failure->message);
            }
            return failure;
        }

        /** @brief Uses the actual selected Play owner on both sides of the native frame admission boundary. */
        std::optional<Error> CommitPlay(Editor::EditorPlaySceneHost &play, const std::uint64_t revision) const {
            return FinishScenePublication(play.Scenes(), {revision}, [&] {
                return play.CommitSceneChanges({.frameNumber = revision, .cancellation = cancellation});
            });
        }

        /** @brief Publishes this actual authored source through the production registry and immutable cook format for the real factory. */
        Application::Internal::SceneCookSelection PublishProject(const std::filesystem::path &root) const {
            std::filesystem::create_directories(root / "assets");
            REQUIRE(root == std::filesystem::canonical(root));
            const auto cookedRoot = root / "derived/cooked";
            std::filesystem::create_directories(cookedRoot);
            NativeDurableFileSystem files;
            REQUIRE(files.WriteDurable(root / "assets/project.horoasset", std::as_bytes(std::span{sourceJson})).HasValue());
            const auto sidecar =
                std::string{R"({"schemaVersion":1,"assetId":")"} + asset.ToString() + R"(","assetType":"runtime.ui.document"})";
            REQUIRE(files.WriteDurable(root / "assets/project.horoasset.horo", std::as_bytes(std::span{sidecar})).HasValue());
            auto lease = Take(files.TryAcquireExclusive(cookedRoot / ".cook-writer.lock", "project UI ordering test"));
            const std::array entries{Assets::AssetCookManifestEntry{asset, Take(Assets::AssetTypeId::Parse("runtime.ui.document")),
                                                                    asset.ToString() + ".cooked",
                                                                    ComputeSha256(std::as_bytes(std::span{encodedArtifact}))}};
            const std::array payloads{encodedArtifact};
            const Assets::AssetCookPublicationPolicy policy{.files = &files, .operationId = asset.ToString(), .writerLease = &lease};
            const auto published = Assets::PublishCookGeneration(cookedRoot, target, entries, payloads, {}, policy);
            if (published.HasError()) {
                const auto &error = published.ErrorValue();
                UNSCOPED_INFO("Project fixture cook publication: " << error.domain.Value() << '/' << error.code.Value() << ": "
                                                                   << error.message);
            }
            REQUIRE(published.HasValue());
            return {cookedRoot, target};
        }

        std::shared_ptr<const RuntimeUiClient> Client(const std::uint32_t slot) {
            return Take(port.Issue({"game.tests",
                                    scenes.ActiveScene()->RuntimeId().value,
                                    1,
                                    {Source::Owner(), slot, 1},
                                    reference.canvas,
                                    RuntimeUiPermission::Elements,
                                    {}}));
        }

        void Frame() {
            clock.Advance(Duration::FromMilliseconds(16));
            const auto frame = host->RunFrame();
            if (frame.HasError())
                UNSCOPED_INFO(frame.ErrorValue().domain.Value()
                              << '/' << frame.ErrorValue().code.Value() << ": " << frame.ErrorValue().message);
            REQUIRE(frame.HasValue());
        }
    };
}  // namespace

TEST_CASE("World canvas projection derives one centered plane from the exact current Scene entity",
          "[application][runtime_ui][project][world-canvas]") {
    ProjectUi fixture{true, true, {}, UiRenderMode::WorldSpace, {200, 80}, {3, 2}};
    // Observe the same registered Canvas on a Scene owner that deliberately has no renderer activation participant.
    Runtime::RuntimeSceneService placementScene{fixture.registry, fixture.assetLoads, {}, {100}};
    REQUIRE(placementScene.Startup({}).HasValue());
    const auto publish = [&](const std::uint64_t revision, const float x) {
        Runtime::SceneDefinitionBuilder scene{{9}, {revision}};
        scene.Add({.object = {1}, .localTransform = {.translation = {x, 20, -5}, .scale = {2, 3, 1}}});
        Runtime::RuntimeEntityDefinition child{.object = {2},
                                               .parent = Runtime::SceneObjectId{1},
                                               .localTransform = {.translation = {1, 2, 0},
                                                                  .rotation = Math::Quaternion::FromEulerRadians({0, 0, Math::Pi / 2}),
                                                                  .scale = {2, 1, 1}}};
        child.components.uiCanvas = Runtime::UiCanvasComponent{fixture.reference};
        scene.Add(std::move(child));
        REQUIRE(placementScene.QueuePreparation(Take(std::move(scene).Build())).HasValue());
        REQUIRE_FALSE(FinishScenePublication(placementScene, {revision}, [&] {
            return placementScene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                          {.frameNumber = revision, .cancellation = fixture.cancellation});
        }));
        return *placementScene.ActiveScene();
    };
    auto load = Take(fixture.uiLoads.LoadAsync(fixture.registry.Snapshot(), {fixture.reference, {}, fixture.target}));
    REQUIRE(load.Wait().HasValue());
    auto authored = Take(load.TakeResult());
    const auto original = publish(1, 10);
    REQUIRE(original.Find({2}));
    const auto child = *original.Find({2});
    const auto resolved = Take(ResolveRuntimeUiPaintWorldCanvas(original, child, authored, 512));
    CHECK(resolved.entity == child);
    CHECK(resolved.reference == fixture.reference);
    CHECK(resolved.documentRevision == fixture.reference.minimumRevision);
    CHECK(Math::NearlyEqual(resolved.plane.origin, {11, 23, -5}, 0.0001F));
    CHECK(Math::NearlyEqual(resolved.plane.horizontalExtent, {0, 6, 0}, 0.0001F));
    CHECK(Math::NearlyEqual(resolved.plane.verticalExtent, {2, 0, 0}, 0.0001F));
    const auto placement = Take(BuildRuntimeUiPaintWorldCanvasTransform(resolved.plane));
    CHECK(Math::NearlyEqual(Math::TransformPoint(placement, {-0.5F, 0.5F, 0}), resolved.plane.origin));
    CHECK(Math::NearlyEqual(Math::TransformPoint(placement, {0.5F, -0.5F, 0}),
                            resolved.plane.origin + resolved.plane.horizontalExtent + resolved.plane.verticalExtent));
    CHECK(BuildRuntimeUiPaintWorldCanvasTransform({resolved.plane.origin, resolved.plane.horizontalExtent, resolved.plane.horizontalExtent})
              .HasError());
    CHECK((resolved.logicalExtent == UiCanvasLogicalExtent{12'800, 5'120}));
    CHECK((resolved.rasterExtent == UiCanvasPixelExtent{200, 80}));
    CHECK((resolved.fontScale == UiCanvasScaleFactor{3, 2}));
    const auto capacity = ResolveRuntimeUiPaintWorldCanvas(original, child, authored, 199);
    REQUIRE(capacity.HasError());
    CHECK(capacity.ErrorValue().code.Value() == UiErrors::CapacityExceeded.code.Value());
    authored.rootAsset = {};
    const auto wrongAsset = ResolveRuntimeUiPaintWorldCanvas(original, child, authored, 512);
    REQUIRE(wrongAsset.HasError());
    CHECK(wrongAsset.ErrorValue().code.Value() == UiErrors::CanvasReferenceInvalid.code.Value());
    authored.rootAsset = fixture.asset;
    const auto registryRevision = authored.registryRevision;
    authored.registryRevision = {};
    const auto staleAsset = ResolveRuntimeUiPaintWorldCanvas(original, child, authored, 512);
    REQUIRE(staleAsset.HasError());
    CHECK(staleAsset.ErrorValue().code.Value() == UiErrors::RevisionStale.code.Value());
    authored.registryRevision = registryRevision;

    Runtime::SceneCommandBuffer edits;
    REQUIRE(original.Find({1}));
    edits.SetLocalTransform(*original.Find({1}), {.translation = {20, 20, -5}, .scale = {2, 3, 1}});
    REQUIRE(placementScene.QueueStructuralCommands(std::move(edits)).HasValue());
    REQUIRE(placementScene
                .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 2, .cancellation = fixture.cancellation})
                .HasValue());
    REQUIRE_FALSE(placementScene.TakeOperationError());
    const auto changed = *placementScene.ActiveScene();
    const auto updated = Take(CaptureRuntimeUiPaintWorldCanvasPlane(changed, resolved));
    CHECK(Math::NearlyEqual(updated.origin, {21, 23, -5}, 0.0001F));
    CHECK(Math::NearlyEqual(updated.horizontalExtent, resolved.plane.horizontalExtent));
    CHECK(
        Math::NearlyEqual(Math::TransformPoint(Take(BuildRuntimeUiPaintWorldCanvasTransform(updated)), {-0.5F, 0.5F, 0}), updated.origin));

    const auto replacement = publish(2, 30);
    REQUIRE(replacement.Find({2}));
    CHECK(ResolveRuntimeUiPaintWorldCanvas(original, child, authored, 512).HasError());
    CHECK(ResolveRuntimeUiPaintWorldCanvas(replacement, child, authored, 512).HasError());
    CHECK(CaptureRuntimeUiPaintWorldCanvasPlane(replacement, resolved).HasError());
    const auto moved = Take(ResolveRuntimeUiPaintWorldCanvas(replacement, *replacement.Find({2}), authored, 512));
    CHECK(Math::NearlyEqual(moved.plane.origin, {31, 23, -5}, 0.0001F));
    CHECK(moved.rasterExtent == resolved.rasterExtent);
    placementScene.Shutdown();
    CHECK(ResolveRuntimeUiPaintWorldCanvas(replacement, moved.entity, authored, 512).HasError());
}

TEST_CASE("Registered World canvas needs an actual selected Scene consumer before publication",
          "[application][runtime_ui][project][world-canvas]") {
    ProjectUi fixture{true, true, {}, UiRenderMode::WorldSpace};
    REQUIRE_FALSE(fixture.Publish(1, 0));
    const auto prior = *fixture.scenes.ActiveScene();
    const auto failure = fixture.Publish(2, 1);
    REQUIRE(failure);
    CHECK(failure->domain.Value() == UiErrors::CapabilityUnsupported.domain.Value());
    CHECK(failure->code.Value() == UiErrors::CapabilityUnsupported.code.Value());
    CHECK(prior.IsCurrent());
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() == prior.RuntimeId());
    CHECK_FALSE(fixture.port.InputEligible({Source::Owner(), 1, 1}));
    fixture.Frame();
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.graphCalls == 0);
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.kernels == 0);
}

TEST_CASE("Project World canvas enters the scene route without claiming screen presentation",
          "[application][runtime_ui][project][world-canvas]") {
    const auto *root = std::getenv("HORO_VULKAN_UI_PAINT_ASSET_ROOT");
    if (!root || !*root)
        SKIP("Supply the immutable offline Vulkan UI paint package.");
    ProjectUi fixture{true, false, {}, UiRenderMode::WorldSpace, {200, 80}, {3, 2}, root};
    REQUIRE_FALSE(fixture.Publish(1, 1));
    const auto client = fixture.Client(1);
    CHECK(client->Validate().HasValue());
    fixture.Frame();
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.graphCalls == 0);
    CHECK_FALSE(fixture.port.InputEligible({Source::Owner(), 1, 1}));

    const auto stable = fixture.scenes.ActiveScene()->RuntimeId();
    const auto failure = fixture.Publish(2, 2);
    REQUIRE(failure);
    CHECK(failure->code.Value() == UiErrors::CapacityExceeded.code.Value());
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() == stable);
    CHECK(client->Validate().HasValue());
}

TEST_CASE("Project Camera canvas uses game viewport dimensions instead of the main drawable",
          "[application][runtime_ui][project][camera-canvas]") {
    const auto *root = std::getenv("HORO_VULKAN_UI_PAINT_ASSET_ROOT");
    if (!root || !*root)
        SKIP("Supply the immutable offline Vulkan UI paint package.");
    ProjectUi fixture{true, false, {}, UiRenderMode::ScreenSpaceCamera, {200, 80}, {}, root};
    UiCanvasPixelExtent expected{72, 48};
    REQUIRE(fixture.port.UpdateCameraOutput(expected).HasValue());
    REQUIRE_FALSE(fixture.Publish(1, 1));
    const auto scene = fixture.scenes.ActiveScene()->RuntimeId();
    std::size_t frames{};
    fixture.observeViewport = [&](const std::uint64_t frame) {
        const auto source = Take(fixture.port.ReadSceneCanvasFrame(frame, scene));
        REQUIRE(source);
        CHECK_FALSE(source->worldCanvas);
        CHECK(source->program->projection.pixelExtent == expected);
        CHECK_FALSE(Take(fixture.port.ReadSceneCanvasFrame(frame, {scene.value + 1})));
        ++frames;
    };
    fixture.Frame();
    expected = {96, 64};
    REQUIRE(fixture.port.UpdateCameraOutput(expected).HasValue());
    fixture.Frame();
    CHECK(frames == 2);
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.graphCalls == 0);
    CHECK_FALSE(fixture.port.InputEligible({Source::Owner(), 1, 1}));
}

TEST_CASE("A rejected Camera canvas scale preserves the committed application visual policy",
          "[application][runtime_ui][project][camera-canvas][accessibility]") {
    const auto *root = std::getenv("HORO_VULKAN_UI_PAINT_ASSET_ROOT");
    if (!root || !*root)
        SKIP("Supply the immutable offline Vulkan UI paint package.");
    ProjectUi fixture{true, false, {}, UiRenderMode::ScreenSpaceCamera, {200, 80}, {}, root};
    constexpr UiCanvasPixelExtent regular{72, 48};
    REQUIRE(fixture.port.UpdateCameraOutput(regular).HasValue());
    REQUIRE_FALSE(fixture.Publish(1, 1));
    const auto scene = fixture.scenes.ActiveScene()->RuntimeId();
    auto expectedScale = UiCanvasScaleFactor{1, 1};
    fixture.observeViewport = [&](const std::uint64_t frame) {
        const auto source = Take(fixture.port.ReadSceneCanvasFrame(frame, scene));
        REQUIRE(source);
        CHECK(source->program->projection.pixelExtent == regular);
        CHECK(source->program->projection.userUiScale == expectedScale);
    };
    fixture.Frame();

    // This height fits in logical units at 100%, but would overflow when halving the scale.
    REQUIRE(fixture.port.UpdateCameraOutput({1, 100'000}).HasValue());
    const auto rejected = fixture.port.UpdateVisualAccessibility({2, false, false, {1, 2}});
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UiErrors::CanvasSpaceOverflow.code.Value());
    REQUIRE(fixture.port.UpdateCameraOutput(regular).HasValue());
    fixture.Frame();

    // A corrected retry can commit the same configuration revision.
    expectedScale = {3, 2};
    REQUIRE(fixture.port.UpdateVisualAccessibility({2, false, false, expectedScale}).HasValue());
    fixture.Frame();
}

TEST_CASE("Project Scene canvas sources use canonical admission and the same application paint phase",
          "[application][runtime_ui][project]") {
    ProjectUi fixture;
    const auto failure = fixture.Publish(1, 2);
    INFO((failure ? failure->message : ""));
    REQUIRE_FALSE(failure);
    const auto first = fixture.Client(1);
    const auto second = fixture.Client(2);
    CHECK(first->Find(Source::Stable<UiElementId>(4)).HasValue());
    CHECK(second->Find(Source::Stable<UiElementId>(4)).HasValue());
    CHECK_FALSE(fixture.port.InputEligible({Source::Owner(), 1, 1}));
    CHECK_FALSE(fixture.port.InputEligible({Source::Owner(), 2, 1}));
    CHECK_FALSE(fixture.port.InputEligible({Source::Owner(), 99, 1}));
    fixture.Frame();
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.graphCalls == 2);
    fixture.Frame();
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.graphCalls == 4);
    CHECK(fixture.port.InputEligible({Source::Owner(), 1, 1}));
    CHECK(fixture.port.InputEligible({Source::Owner(), 2, 1}));
    CHECK_FALSE(fixture.port.InputEligible({Source::Owner(), 99, 1}));
    REQUIRE(fixture.scenes.QueueUnload().HasValue());
    REQUIRE(fixture.scenes
                .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 3, .cancellation = fixture.cancellation})
                .HasValue());
    CHECK_FALSE(fixture.scenes.TakeOperationError());
    CHECK_FALSE(fixture.scenes.ActiveScene());
    CHECK(first->Validate().HasError());
    CHECK(second->Validate().HasError());
    REQUIRE(Take(fixture.port.CollectRetired()));
}

TEST_CASE("Project Scene replacement retires only the old runtime incarnation while queued paint drains",
          "[application][runtime_ui][project][lifetime]") {
    ProjectUi fixture;
    REQUIRE_FALSE(fixture.Publish(1, 1));
    const auto oldScene = *fixture.scenes.ActiveScene();
    const auto oldClient = fixture.Client(1);
    fixture.Frame();
    Tests::RenderFrontendTesting::lifecycleState.completeGraphOnPresent = false;
    fixture.Frame();
    REQUIRE(Tests::RenderFrontendTesting::lifecycleState.graphLease != nullptr);
    REQUIRE_FALSE(fixture.Publish(2, 1));
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() != oldScene.RuntimeId());
    CHECK_FALSE(oldScene.IsCurrent());
    CHECK(oldClient->Validate().HasError());
    const auto replacement = fixture.Client(2);
    CHECK(replacement->Find(Source::Stable<UiElementId>(4)).HasValue());
    CHECK_FALSE(Take(fixture.port.CollectRetired()));
    auto &lease = Tests::RenderFrontendTesting::lifecycleState.graphLease;
    lease->Release();
    lease = nullptr;
    REQUIRE(Take(fixture.port.CollectRetired()));
    CHECK(replacement->Validate().HasValue());
    Tests::RenderFrontendTesting::lifecycleState.completeGraphOnPresent = true;
    fixture.Frame();
    fixture.Frame();
    CHECK(fixture.port.InputEligible({Source::Owner(), 2, 1}));
}

TEST_CASE("Project Scene whole-canvas capacity rejects replacement before native preparation or publication",
          "[application][runtime_ui][project][capacity]") {
    ProjectUi fixture;
    REQUIRE_FALSE(fixture.Publish(1, 1));
    const auto scene = fixture.scenes.ActiveScene()->RuntimeId();
    const auto client = fixture.Client(1);
    const auto kernels = Tests::RenderFrontendTesting::lifecycleState.uiPaint.kernels;
    const auto failure = fixture.Publish(2, 9);
    REQUIRE(failure);
    CHECK(failure->code.Value() == UiErrors::CapacityExceeded.code.Value());
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() == scene);
    CHECK(client->Validate().HasValue());
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.kernels == kernels);
}

TEST_CASE("Empty project Scene stays lawful when optional native paint or its admitted compiler is unavailable",
          "[application][runtime_ui][project][unsupported]") {
    ProjectUi fixture{false, false};
    REQUIRE_FALSE(fixture.Publish(1, 0));
    const auto scene = fixture.scenes.ActiveScene()->RuntimeId();
    const auto failure = fixture.Publish(2, 1);
    REQUIRE(failure);
    CHECK(failure->code.Value() == UiErrors::CapabilityUnsupported.code.Value());
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() == scene);
    fixture.Frame();
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.kernels == 0);
}

TEST_CASE("Failed native project UI preparation preserves the old Scene and canonical client",
          "[application][runtime_ui][project][rollback]") {
    ProjectUi fixture;
    REQUIRE_FALSE(fixture.Publish(1, 1));
    const auto scene = fixture.scenes.ActiveScene()->RuntimeId();
    const auto client = fixture.Client(1);
    Tests::RenderFrontendTesting::lifecycleState.failResourceCreation = true;
    const auto failure = fixture.Publish(2, 1);
    REQUIRE(failure);
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() == scene);
    CHECK(client->Validate().HasValue());
    Tests::RenderFrontendTesting::lifecycleState.failResourceCreation = false;
    REQUIRE(Take(fixture.port.CollectRetired()));
}

TEST_CASE("Actual project Play factory installs the same optional UI participant before Scene activation",
          "[application][runtime_ui][project][play]") {
    ProjectUi fixture{false, false};
    auto create = Application::Internal::MakeEditorProjectSceneFactory(fixture.jobs, {.uiSources = fixture.sources});
    auto prepared = create({}, Editor::EditorFirstPlayScene, fixture.Definition(1, 0));
    REQUIRE(prepared.HasValue());
    auto &play = *prepared.Value();
    REQUIRE(play.Scenes().Startup({}).HasValue());
    REQUIRE(play.Scenes().QueuePreparation(fixture.Definition(1, 0)).HasValue());
    REQUIRE_FALSE(fixture.CommitPlay(play, 1));
    REQUIRE(play.Scenes().ActiveScene());
    CHECK(fixture.port.Phase() != RuntimeUiPaintHostPhase::Failed);
    play.Shutdown();
    CHECK_FALSE(play.Scenes().ActiveScene());
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.kernels == 0);
}

TEST_CASE("Project Scene loads the actual packaged Metal source policy before canonical publication",
          "[application][runtime_ui][project][package]") {
    Tests::UiPaintPackageTesting::PackageDirectory package;
    package.PublishMetadataFixture();  // CPU metadata fixture; native pipeline qualification is separate.
    ProjectUi fixture{true, false, package.root};
    REQUIRE_FALSE(fixture.Publish(1, 2));
    const auto oldClient = fixture.Client(1);
    fixture.Frame();
    fixture.Frame();
    REQUIRE(fixture.port.InputEligible({Source::Owner(), 1, 1}));
    REQUIRE(Tests::RenderFrontendTesting::lifecycleState.uiPaint.graphCalls == 4);
    REQUIRE_FALSE(fixture.Publish(2, 2));
    CHECK(oldClient->Validate().HasError());
    fixture.Frame();
    REQUIRE(fixture.scenes.QueueUnload().HasValue());
    REQUIRE(fixture.scenes
                .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 3, .cancellation = fixture.cancellation})
                .HasValue());
    REQUIRE_FALSE(fixture.scenes.TakeOperationError());
    REQUIRE(Take(fixture.port.CollectRetired()));
}

TEST_CASE("Project missing packaged kernel cannot partially publish authored UI or Scene", "[application][runtime_ui][project][package]") {
    Tests::UiPaintPackageTesting::PackageDirectory package;
    ProjectUi fixture{true, false, package.root};
    REQUIRE(fixture.Publish(1, 1).has_value());
    CHECK_FALSE(fixture.scenes.ActiveScene());
    package.PublishMetadataFixture();
    REQUIRE_FALSE(fixture.Publish(2, 1));
    fixture.Frame();
    fixture.Frame();
    CHECK(fixture.port.InputEligible({Source::Owner(), 1, 1}));
}

TEST_CASE("Invalid packaged canvas reference leaves the current project Scene and input active",
          "[application][runtime_ui][project][package][rollback]") {
    Tests::UiPaintPackageTesting::PackageDirectory package;
    package.PublishMetadataFixture();
    ProjectUi fixture{true, false, package.root};
    REQUIRE_FALSE(fixture.Publish(1, 1));
    const auto scene = *fixture.scenes.ActiveScene();
    const auto client = fixture.Client(1);
    fixture.Frame();
    fixture.Frame();
    REQUIRE(fixture.port.InputEligible({Source::Owner(), 1, 1}));

    const auto validReference = fixture.reference;
    fixture.reference.minimumRevision = Source::Revision<UiDocumentRevision>(2);
    const auto failure = fixture.Publish(2, 1);
    REQUIRE(failure);
    CHECK(failure->domain.Value() == UiErrors::CanvasReferenceInvalid.domain.Value());
    CHECK(failure->code.Value() == UiErrors::CanvasReferenceInvalid.code.Value());
    CHECK(scene.IsCurrent());
    CHECK(client->Validate().HasValue());
    fixture.Frame();
    CHECK(fixture.port.InputEligible({Source::Owner(), 1, 1}));

    fixture.reference = validReference;
    REQUIRE_FALSE(fixture.Publish(3, 1));
    CHECK_FALSE(scene.IsCurrent());
    CHECK(client->Validate().HasError());
    fixture.Frame();
    fixture.Frame();
    CHECK(fixture.port.InputEligible({Source::Owner(), 2, 1}));
}

TEST_CASE("Actual project Play UI preparation stays outside native admission through queued publication",
          "[application][runtime_ui][project][play][safe_point]") {
    ProjectUi fixture;
    Tests::UiPaintPackageTesting::PackageDirectory directory;
    const auto projectRoot = std::filesystem::canonical(directory.root);
    const auto cooked = fixture.PublishProject(projectRoot);
    auto create = Application::Internal::MakeEditorProjectSceneFactory(fixture.jobs, {.cooked = cooked, .uiSources = fixture.sources});
    auto result = create(projectRoot, Editor::EditorFirstPlayScene, fixture.Definition(1, 1));
    REQUIRE(result.HasValue());
    auto play = std::move(result).Value();
    REQUIRE(play->Scenes().Startup({}).HasValue());
    REQUIRE(play->Scenes().QueuePreparation(fixture.Definition(1, 1)).HasValue());
    CHECK_FALSE(Tests::RenderFrontendTesting::lifecycleState.frameActive);
    const auto error = fixture.CommitPlay(*play, 1);
    INFO((error ? error->message : "Project preparation succeeded"));
    REQUIRE_FALSE(error);
    REQUIRE(play->Scenes().ActiveScene());
    CHECK_FALSE(Tests::RenderFrontendTesting::lifecycleState.frameActive);
    // One source prepares both the primary output and isolated RGBA16F effect pipeline.
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.kernels == 2);
    CHECK_FALSE(fixture.port.InputEligible({Source::Owner(), 1, 1}));
    fixture.Frame();
    fixture.Frame();
    CHECK(fixture.port.InputEligible({Source::Owner(), 1, 1}));
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.graphCalls == 2);
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.kernels == 2);
    play->Shutdown();
    REQUIRE(Take(fixture.port.CollectRetired()));
}

TEST_CASE("Early native admission cannot publish actual project Play UI resources",
          "[application][runtime_ui][project][play][safe_point]") {
    ProjectUi fixture;
    Tests::UiPaintPackageTesting::PackageDirectory directory;
    const auto projectRoot = std::filesystem::canonical(directory.root);
    const auto cooked = fixture.PublishProject(projectRoot);
    auto create = Application::Internal::MakeEditorProjectSceneFactory(fixture.jobs, {.cooked = cooked, .uiSources = fixture.sources});
    auto result = create(projectRoot, Editor::EditorFirstPlayScene, fixture.Definition(1, 1));
    REQUIRE(result.HasValue());
    auto play = std::move(result).Value();
    REQUIRE(play->Scenes().Startup({}).HasValue());
    {
        auto frame = Take(fixture.frontend->BeginFrame({.frameNumber = 1, .outputExtent = fixture.output}));
        REQUIRE(Tests::RenderFrontendTesting::lifecycleState.frameActive);
        REQUIRE(play->Scenes().QueuePreparation(fixture.Definition(1, 1)).HasValue());
        CHECK_FALSE(play->Scenes().ActiveScene());
        // The registered canvas loads asynchronously; keep the actual native frame open through the publication attempt.
        const auto error = fixture.CommitPlay(*play, 1);
        REQUIRE(error);
        UNSCOPED_INFO(error->domain.Value() << '/' << error->code.Value() << ": " << error->message);
        CHECK(error->domain.Value() == "render.frontend");
        CHECK(error->code.Value() == "render.frontend.resource.change_during_frame");
        CHECK_FALSE(play->Scenes().ActiveScene());
        CHECK_FALSE(fixture.port.InputEligible({Source::Owner(), 1, 1}));
    }
    CHECK_FALSE(Tests::RenderFrontendTesting::lifecycleState.frameActive);
    play->Shutdown();
    REQUIRE(Take(fixture.port.CollectRetired()));
}
