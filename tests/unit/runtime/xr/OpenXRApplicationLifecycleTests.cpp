#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "OpenXRApplicationSession.h"
#include "OpenXRApplicationVulkanTestSupport.h"
#include "OpenXRPlayHost.h"
#include "editor/renderer/vulkan/EditorViewportRendererVulkan.h"
#include "support/OpenXRApplicationNativeTestSupport.h"
#include "support/OpenXRInstalledProductTestFixture.h"
#include "unit/editor/SceneDocumentHierarchyFixture.h"

extern "C" [[gnu::visibility("default")]] XRAPI_ATTR XrResult XRAPI_CALL HoroTestOpenXRResolve(XrInstance instance, const char *name,
                                                                                               PFN_xrVoidFunction *function) {
    return Horo::Tests::OpenXRApplication::ApplicationNativeScript::Resolve(instance, name, function);
}

namespace Horo::Tests::OpenXRApplication {
    namespace {
        namespace Host = Horo::Application::Internal;
        using OpenXRProduct::RequireValue;

        ConfigurationSchema XrSchema() {
            ConfigurationSchema schema;
            for (const auto &descriptor : XR::XRProjectSettingDescriptors())
                REQUIRE(schema.Register(descriptor).HasValue());
            REQUIRE(schema.Seal().HasValue());
            return schema;
        }

        /** @brief Owns real installation, native preparation and canonical scene/input parents through explicit cleanup. */
        struct ApplicationFixture final {
            ApplicationNativeScript script;
            OpenXRProduct::InstalledProductFixture product{HORO_TEST_OPENXR_LOADER};
            ConfigurationService configuration{XrSchema()};
            PreparedGraphicsPort graphics;
            std::unique_ptr<ApplicationVulkanFixture> vulkan;
            PreparedGraphicsPort *activeGraphics{&graphics};
            RenderNative::VulkanEditorGraphicsBridge bridge;
            DeterministicClock schedulerClock;
            std::unique_ptr<Runtime::RuntimeHost> scheduler{
                RequireValue(Runtime::RuntimeHost::Create(schedulerClock, {.fixedStep = Duration::FromMilliseconds(10)}))};
            Editor::SceneDocument document;
            const Editor::SceneDocumentTestSupport::HierarchySceneObjects objects{
                Editor::SceneDocumentTestSupport::MakeHierarchyScene(document)};
            Runtime::PrimitiveMeshCache primitives;
            Editor::EditorAssetMeshCache assets;
            Editor::EditorXrSceneSourceOwner source{document, primitives, assets};
            const Runtime::RuntimeSceneDefinition definition{
                RequireValue(Editor::ConvertSceneDocumentToRuntime(document.Snapshot(), Runtime::SceneDefinitionId{1}))};
            std::unique_ptr<Runtime::RuntimeScene> scene{RequireValue(Runtime::RuntimeScene::Create(definition, {1}))};
            Input::InputRouter router;
            Input::InputContextToken inputContext; /**< Outlives all session/play borrowers and is destroyed before its router. */
            std::shared_ptr<const Host::OpenXRApplicationBindings> bindings{product.ReadBindings()};
            std::unique_ptr<Host::OpenXRApplicationSession> session;
            Render::RenderBackendRegistry renderers;
            std::unique_ptr<Render::RenderFrontend> frontend;
            std::unique_ptr<Editor::EditorViewportRendererVulkan> viewport;
            std::unique_ptr<Host::OpenXRPlayHost> play;
            bool admission{true};

            explicit ApplicationFixture(const bool render = false) {
                ConfigurationResolutionRequest request;
                request.invocation.emplace(SettingKey{"xr.enabled"}, ConfigurationInputValue{true, std::nullopt});
                REQUIRE(configuration.ResolveAndCommit(request).HasValue());
                const auto settings = RequireValue(XR::ResolveXRProjectSettings(configuration.Snapshot()));
                session = std::make_unique<Host::OpenXRApplicationSession>(Host::OpenXRApplicationAuthorities{configuration, *product.host,
                                                                                                              product.Root(),
                                                                                                              product.host->Generation(),
                                                                                                              [this] {
                    return admission ? Result<void>::Success() : Result<void>::Failure(MakeError(XR::XRErrors::RuntimeRejected));
                }},
                                                                           settings, product.Candidate(), *product.policy);
                if (render) {
                    vulkan = std::make_unique<ApplicationVulkanFixture>();
                    activeGraphics = &vulkan->port;
                    REQUIRE(renderers
                                .Register(
                                    {Render::RenderBackendId{"vulkan"}, "Scripted native Vulkan",
                                     std::make_unique<Render::VulkanExternalTest::ExternalGraphProvider>(vulkan->native,
                                                                                                         Render::RenderBackendId{"vulkan"},
                                                                                                         [this] {
                        vulkan->port.ReleaseDrainedChild();
                    })}).HasValue());
                } else {
                    REQUIRE(Render::RegisterNullRenderBackend(renderers).HasValue());
                }
                REQUIRE(bridge.Attach(*activeGraphics));
                REQUIRE(renderers.Seal().HasValue());
                frontend = RequireValue(Render::RenderFrontend::Create(renderers, Render::RenderBackendId{render ? "vulkan" : "null"},
                                                                       Render::RenderBackendConfig{}));
                Editor::EditorVulkanViewportCook viewportCook;
                if (render) {
                    const auto cooked = ApplicationVulkanFixture::Cook(false);
                    viewportCook = RequireValue(
                        Editor::LoadEditorVulkanViewportCook(cooked.compiled.payload,
                                                             RequireValue(Render::EncodeReflectedShaderArtifact(cooked.reflected))));
                }
                viewport = std::make_unique<Editor::EditorViewportRendererVulkan>(*frontend, bridge, std::move(viewportCook));
                if (render) {
                    REQUIRE(viewport->Initialize().HasValue());
                    const auto cooked = ApplicationVulkanFixture::Cook(true);
                    RequireValue(viewport->PrepareXrProgram(RequireValue(
                        Editor::LoadEditorVulkanMultiviewCook(cooked.compiled.payload,
                                                              RequireValue(Render::EncodeReflectedShaderArtifact(cooked.reflected))))));
                }
            }

            ApplicationFixture(const ApplicationFixture &) = delete;
            ApplicationFixture &operator=(const ApplicationFixture &) = delete;

            ~ApplicationFixture() {
                // Clear only test-injected failure controls. The real owners still perform every native release.
                script.native.fail = {};
                script.native.failRetirement = false;
                script.native.failInstanceRetirement = false;
                script.census.fail = {};
                script.graphics.failure = XR_SUCCESS;
                if (vulkan)
                    vulkan->native.commandState.state.Complete();
                if (play) {
                    static_cast<void>(play->ProgressRenderTail());
                    auto closed = play->Close();
                    if (closed.HasError() || !closed.Value())
                        std::terminate();
                    play.reset();
                }
                if (session && session->Close().HasError())
                    std::terminate();
                session.reset();
                if (vulkan) {
                    static_cast<void>(frontend->BeginShutdown());
                    if (frontend->PollDrain().HasError())
                        std::terminate();
                    vulkan->port.ReleaseDrainedChild();
                }
                viewport.reset();
                frontend.reset();
                bridge.Detach(*activeGraphics);
            }

            void Prepare() {
                RequireValue(session->PrepareDiscovery());
                RequireValue(session->AttachGraphics(*activeGraphics, bridge));
                RequireValue(session->PrepareSession());
            }

            void CreatePlay() {
                Prepare();
                play = RequireValue(Host::OpenXRPlayHost::Create({*session,
                                                                  router,
                                                                  *viewport,
                                                                  *frontend,
                                                                  bindings,
                                                                  scheduler->DispatchSource(),
                                                                  Duration::FromMilliseconds(10),
                                                                  {objects.parent.value}}));
            }

            Input::InputActionCollection InputPort() {
                const std::vector<Input::ActionDescriptor> registered{
                    {Input::ActionId{"select"},
                     Input::ActionValueType::Digital,
                     Input::InputContextId{"gameplay"},
                     true,
                     {{.kind = Input::BindingControlKind::Key, .key = Input::Key::Space}}}};
                REQUIRE(router.SetActionMap(registered).HasValue());
                inputContext = router.PushContext(Input::InputContextId{"gameplay"}, Input::InputContextKind::Gameplay);
                return RequireValue(router.ActionCollection(inputContext));
            }
        };
    }  // namespace

    TEST_CASE("Signed production application discovery retains actual preparation without issuing Ready", "[xr][application][lifecycle]") {
        ApplicationFixture fixture;
        CHECK(fixture.session->BorrowPreparation().HasError());
        fixture.Prepare();
        const auto prepared = fixture.session->BorrowPreparation();
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().system.primaryViewCount == 2);
        CHECK(prepared.Value().session.spaceCount == 2);
        CHECK_FALSE(prepared.Value().native.Session().IsValid());
        CHECK_FALSE(fixture.session->RetainedSession().has_value());
        const auto calls = fixture.script.census.calls.size();
        REQUIRE(fixture.session->PrepareSession().HasValue());
        CHECK(fixture.script.census.calls.size() == calls);
        CHECK(fixture.session->PrepareDiscovery().HasError());
        REQUIRE(fixture.session->RetirePreparation().HasValue());
        CHECK(fixture.script.native.liveSessions == 0);
        CHECK(fixture.script.native.liveInstances == 1);
        REQUIRE(fixture.session->Close().HasValue());
        CHECK(fixture.script.native.liveInstances == 0);
    }

    TEST_CASE("Application native discovery failures preserve verified loader and partial owners for explicit cleanup",
              "[xr][application][lifecycle]") {
        ApplicationFixture fixture;
        SECTION("native instance creation") {
            fixture.script.native.fail = "instance";
        }
        SECTION("native system query") {
            fixture.script.native.fail = "system";
        }
        SECTION("census failure") {
            fixture.script.census.fail = "system-properties";
        }
        SECTION("graphics requirement failure") {
            fixture.script.graphics.failure = XR_ERROR_RUNTIME_FAILURE;
        }
        CHECK(fixture.session->PrepareDiscovery().HasError());
        CHECK(fixture.session->BorrowPreparation().HasError());
        CHECK_FALSE(fixture.session->RetainedSession().has_value());
        fixture.script.native.fail = {};
        fixture.script.census.fail = {};
        fixture.script.graphics.failure = XR_SUCCESS;
        REQUIRE(fixture.session->Close().HasValue());
        CHECK(fixture.script.native.liveInstances == 0);
        CHECK(fixture.script.native.liveSessions == 0);
    }

    TEST_CASE("Application native cleanup ignores revoked admission and retains failed destruction for retry",
              "[xr][application][lifecycle]") {
        ApplicationFixture fixture;
        fixture.Prepare();
        fixture.admission = false;
        CHECK(fixture.session->BorrowPreparation().HasError());
        fixture.script.native.failRetirement = true;
        CHECK(fixture.session->RetirePreparation().HasError());
        CHECK(fixture.script.native.liveSessions == 1);
        fixture.script.native.failRetirement = false;
        REQUIRE(fixture.session->RetirePreparation().HasValue());
        CHECK(fixture.script.native.liveSessions == 0);
        fixture.script.native.failInstanceRetirement = true;
        CHECK(fixture.session->Close().HasError());
        CHECK(fixture.script.native.liveInstances == 1);
        fixture.script.native.failInstanceRetirement = false;
        REQUIRE(fixture.session->Close().HasValue());
        CHECK(fixture.script.native.liveInstances == 0);
    }

    TEST_CASE("Actual application Play remains inactive until canonical scene and renderer preparation succeed",
              "[xr][application][play]") {
        ApplicationFixture fixture;
        fixture.CreatePlay();
        CHECK(fixture.play->PresentationState() == Host::OpenXRPlayPresentation::Inactive);
        CHECK_FALSE(fixture.play->UsesXrScene());
        CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
        CHECK(fixture.play->PrepareRenderFrame(1).HasError());
        CHECK(fixture.play->FinishGui(true).HasError());
        CHECK(fixture.play->ProgressRenderTail().HasValue());
        REQUIRE(fixture.play->SetPlayState(Editor::EditorPlayState::Playing).HasValue());
        auto participant = fixture.play->MakeSceneParticipant();
        CHECK(participant->Prepare(fixture.definition, fixture.scene->View()).HasError());
        const auto input = fixture.InputPort();
        REQUIRE(fixture.play->AttachInput(input, fixture.source.Port()).HasValue());
        CHECK(fixture.play->AttachInput(input, fixture.source.Port()).HasError());
        // This viewport was deliberately never initialized. Real native preparation must roll back;
        // no alternate renderer, camera, made-up image or Ready candidate can cover the failed stage.
        CHECK(participant->Prepare(fixture.definition, fixture.scene->View()).HasError());
        CHECK(fixture.play->PresentationState() == Host::OpenXRPlayPresentation::Pending);
        CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
        fixture.script.native.failRetirement = true;
        CHECK(fixture.play->Close().HasError());
        CHECK(fixture.script.native.liveSessions == 1);
        fixture.script.native.failRetirement = false;
        const auto closed = fixture.play->Close();
        REQUIRE(closed.HasValue());
        CHECK(closed.Value());
        CHECK(fixture.script.native.liveSessions == 0);
        fixture.play.reset();
        CHECK(fixture.script.actions.liveSets == 0);
        CHECK(fixture.script.actions.liveActions == 0);
        CHECK(fixture.script.actions.tracking.spaces == 0);
    }

    namespace {
        /** @brief Declares the actual first-eye generation to the canonical frontend GUI completion lease. */
        class MirrorGui final : public Render::IGuiPassExecutor, public RenderNative::IVulkanGuiEncoder {
        public:
            MirrorGui(GraphGraphicsPort &port, const Render::RenderTextureViewHandle view, const Render::RenderSamplerHandle sampler)
                : port_(port), samples_{{view, sampler, {}}} {}

            Result<Render::GuiCompositionPreparation> PrepareComposition(const Render::FrameToken frame) override {
                return Result<Render::GuiCompositionPreparation>::Success(
                    {frame, Render::GuiCompositionMode::ExplicitGraphicsSubmission, samples_, {}});
            }

            Result<void> RenderDrawData(const Render::GuiCompositionRequest &request) override {
                return port_.ExecuteGui(request, *this);
            }

            Result<void> Record(const VkCommandBuffer command, const RenderNative::VulkanFrameRetentionSlot slot,
                                const std::span<const RenderNative::VulkanGuiSampleBinding> samples,
                                const std::span<const RenderNative::VulkanGuiBufferBinding> buffers) override {
                CHECK(command != VK_NULL_HANDLE);
                CHECK(slot.index < slot.count);
                REQUIRE(samples.size() == 1);
                CHECK(samples[0].view != VK_NULL_HANDLE);
                CHECK(samples[0].sampler != VK_NULL_HANDLE);
                CHECK(buffers.empty());
                ++records;
                return Result<void>::Success();
            }

            unsigned records{};

        private:
            GraphGraphicsPort &port_;
            std::array<Render::GuiSampledResource, 1> samples_;
        };

        std::unique_ptr<Runtime::SceneActivationCandidate> PublishPlay(ApplicationFixture &fixture) {
            fixture.CreatePlay();
            REQUIRE(fixture.play->SetPlayState(Editor::EditorPlayState::Playing).HasValue());
            const auto input = fixture.InputPort();
            REQUIRE(fixture.play->AttachInput(input, fixture.source.Port()).HasValue());
            auto participant = fixture.play->MakeSceneParticipant();
            auto candidate = RequireValue(participant->Prepare(fixture.definition, fixture.scene->View()));
            REQUIRE(candidate->ValidatePublication().HasValue());
            candidate->Publish();
            REQUIRE(fixture.play->UsesXrScene());
            fixture.script.events.session = fixture.script.native.lastSession;
            fixture.script.events.Push(XR_SESSION_STATE_READY);
            fixture.script.events.Push(XR_SESSION_STATE_SYNCHRONIZED);
            fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
            fixture.script.events.Push(XR_SESSION_STATE_FOCUSED);
            REQUIRE(fixture.play->PollEvents().HasValue());
            return candidate;
        }
    }  // namespace

    TEST_CASE("Real installed Play closes nonrendering and failed predictions through the native zero-layer protocol",
              "[xr][application][play][frame]") {
        ApplicationFixture fixture{true};
        auto scene = PublishPlay(fixture);
        fixture.script.clock.sample = 1;
        SECTION("native runtime declines rendering") {
            fixture.script.frames.render = false;
        }
        SECTION("native wait failure retains no borrowed eye") {
            fixture.script.frames.waitResult = XR_ERROR_RUNTIME_FAILURE;
        }
        SECTION("native frame begin failure") {
            fixture.script.frames.beginResult = XR_ERROR_RUNTIME_FAILURE;
        }
        SECTION("prediction misses actual converted deadline") {
            fixture.script.clock.sample = 100'000'000;
        }
        SECTION("successful acquisition is explicitly aborted before submission") {
            REQUIRE(fixture.play->PrepareRenderFrame(1).Value());
            CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
            REQUIRE(fixture.play->FinishGui(false).HasValue());
            REQUIRE(fixture.play->ProgressRenderTail().HasValue());
            CHECK(fixture.script.frames.layers == 0);
            scene->Shutdown();
            return;
        }
        const auto prepared = fixture.play->PrepareRenderFrame(1);
        CHECK((prepared.HasError() || !prepared.Value()));
        CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
        static_cast<void>(fixture.play->ProgressRenderTail());
        scene->Shutdown();
    }

    TEST_CASE("Real installed Play retains its acquired first eye through actual scene and GUI queue completion",
              "[xr][application][play][frame][gui]") {
        ApplicationFixture fixture{true};
        auto scene = PublishPlay(fixture);
        fixture.script.clock.sample = 1;
        const auto sampler = RequireValue(fixture.frontend->CreateSampler({}));
        const auto cooked = ApplicationVulkanFixture::Cook(true);
        const auto layout = RequireValue(Render::PrepareMaterialBindingLayout(cooked.reflected.reflection));
        REQUIRE(RequireValue(Render::ComputeMaterialParameterBufferByteSize(layout, {1})) == 2612);
        REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
        REQUIRE(fixture.frontend->ResourceOperationResult(sampler.operation).HasValue());

        REQUIRE(fixture.play->PrepareRenderFrame(1).Value());
        auto frame = RequireValue(fixture.frontend->BeginFrame({1, {8, 8}}));
        REQUIRE(fixture.play->ExecuteRenderFrame(frame, {}).HasValue());
        CHECK(fixture.vulkan->port.multiviewUniformWrites > 0);
        const auto mirror = fixture.play->MirrorView(1);
        REQUIRE(mirror.IsValid());
        CHECK_FALSE(fixture.play->MirrorView(2).IsValid());
        MirrorGui gui{fixture.vulkan->port, mirror.sampledView, sampler.handle};
        REQUIRE(frame.ComposeGui(gui).HasValue());
        CHECK(gui.records == 1);
        REQUIRE(frame.Present().HasValue());
        REQUIRE(fixture.play->FinishGui(true).HasValue());
        CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
        REQUIRE(fixture.play->ProgressRenderTail().HasValue());
        CHECK(fixture.script.swapchain.releases == 0);
        CHECK(fixture.script.frames.ends == 0);
        fixture.vulkan->native.commandState.state.Complete();
        REQUIRE(fixture.play->ProgressRenderTail().HasValue());
        CHECK(fixture.script.swapchain.releases == 1);
        CHECK(fixture.script.frames.ends == 1);
        CHECK(fixture.script.frames.layers == 1);
        CHECK(fixture.script.frames.views == 2);
        REQUIRE(fixture.frontend->ReleaseSampler(sampler.handle).HasValue());
        auto participant = fixture.play->MakeSceneParticipant();
        // Dispatch through the actual canonical const-reference interface, rather than its default no-op retirement.
        auto retirement = RequireValue(participant->PrepareRetirement(fixture.scene->View()));
        REQUIRE(retirement);
        REQUIRE(retirement->ValidatePublication().HasValue());
        retirement->Publish();
        scene->Shutdown();
        REQUIRE(fixture.play->ProgressRenderTail().HasValue());
    }
}  // namespace Horo::Tests::OpenXRApplication
