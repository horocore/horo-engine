#include "EditorProjectSceneHost.h"
#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "RuntimeUiPaintHostTestFixture.h"
#include "RuntimeUiPaintProjectSources.h"
#include "UiPaintPackageTestFixture.h"

using namespace Horo;
using namespace Horo::Tests::UiPaintHostTesting;

namespace {
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
        std::shared_ptr<RuntimeUiPaintProjectSources> sources;
        const Assets::AssetCookTargetId target{Take(Assets::AssetCookTargetId::Parse("headless-null"))};
        const Assets::AssetId asset{[] {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = 31;
            return Assets::AssetId::FromBytes(bytes);
        }()};
        UiCanvasAssetReference reference{asset, Source::Stable<UiDocumentId>(1), Source::Stable<UiCanvasId>(2),
                                         Source::Revision<UiDocumentRevision>(1)};

        explicit ProjectUi(const bool paint = true, const bool kernel = true, const std::filesystem::path &packagedRoot = {})
            : frontend(Frontend(paint)) {
            const auto type = Take(Assets::AssetTypeId::Parse("runtime.ui.document"));
            REQUIRE(registry
                        .Publish({{asset, type, Take(ProjectPath::Parse("assets/project.ui")),
                                   Take(ProjectPath::Parse("assets/project.ui.horo"))}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            UiDocumentBuilder builder{reference.document, reference.minimumRevision};
            REQUIRE(builder.AddCanvas({reference.canvas, Source::Stable<UiElementId>(3)}).HasValue());
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
            const auto cooked = Take(CookedUiDocument::Cook(Take(std::move(builder).Build())));
            Assets::AssetCookArtifact envelope;
            envelope.id = asset;
            envelope.type = type;
            envelope.target = target;
            envelope.payload.assign(cooked.Payload().begin(), cooked.Payload().end());
            envelope.payloadDigest = ComputeSha256(std::as_bytes(std::span{envelope.payload}));
            provider.Insert(asset, Take(Assets::EncodeCookedArtifact(envelope)));
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
            sources = Take(RuntimeUiPaintProjectSources::Create(port, std::move(allocator), std::move(policy)));
            REQUIRE(scenes.AddActivationParticipant(sources->MakeParticipant(scenes, registry, uiLoads, target)).HasValue());
            REQUIRE(scenes.Startup({}).HasValue());
            REQUIRE(host->AddParticipant(std::move(composition.participant)).HasValue());
            REQUIRE(host->AddParticipant(std::make_unique<Renderer>(*frontend, port, output)).HasValue());
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
            REQUIRE(
                scenes
                    .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = revision, .cancellation = cancellation})
                    .HasValue());
            auto failure = scenes.TakeOperationError();
            if (failure) {
                UNSCOPED_INFO("Scene publication failure: " << failure->domain.Value() << '/' << failure->code.Value() << ": "
                                                            << failure->message);
            }
            return failure;
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
    REQUIRE(play.CommitSceneChanges({.frameNumber = 1, .cancellation = fixture.cancellation}).HasValue());
    REQUIRE_FALSE(play.Scenes().TakeOperationError());
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
