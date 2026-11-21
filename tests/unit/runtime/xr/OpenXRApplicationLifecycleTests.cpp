#include "EditorVulkanXrViewportAccess.h"
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderOriginScene.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/XR/XRSessionErrors.h"
#include "OpenXRApplicationSession.h"
#include "OpenXRApplicationVulkanTestSupport.h"
#include "OpenXRPlayHost.h"
#include "RenderExternalImages.h"
#include "VulkanExternalImages.h"
#include "editor/renderer/vulkan/EditorViewportRendererVulkan.h"
#include "support/OpenXRApplicationNativeTestSupport.h"
#include "support/OpenXRInstalledProductTestFixture.h"
#include "support/OpenXRLightTestSupport.h"
#include "support/OpenXRPassthroughTestSupport.h"
#include "support/OpenXRPlaneCaptureTestSupport.h"
#include "unit/editor/SceneDocumentHierarchyFixture.h"

#include <algorithm>
#include <bit>
#include <catch2/generators/catch_generators.hpp>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
    thread_local PFN_xrGetInstanceProcAddr resolveNativePlayFixture{};
}

extern "C" [[gnu::visibility("default")]] XRAPI_ATTR XrResult XRAPI_CALL HoroTestOpenXRResolve(XrInstance instance, const char *name,
                                                                                               PFN_xrVoidFunction *function) {
    if (resolveNativePlayFixture) {
        const auto result = resolveNativePlayFixture(instance, name, function);
        if (result != XR_ERROR_FUNCTION_UNSUPPORTED)
            return result;
    }
    return Horo::Tests::OpenXRApplication::ApplicationNativeScript::Resolve(instance, name, function);
}

static_assert(!std::is_default_constructible_v<Horo::Application::Internal::OpenXRPlayHost::ConstructionKey>);
static_assert(!std::is_aggregate_v<Horo::Application::Internal::OpenXRPlayHost::ConstructionKey>);

namespace Horo::Tests::OpenXRApplication {
    namespace {
        namespace Host = Horo::Application::Internal;
        using OpenXRProduct::RequireValue;

        /** @brief Limits initialization diagnostics to eight typed causes and bounded message/identity text per cause. */
        std::string ViewportInitializationError(const Error &error) {
            std::string text;
            const auto *current = &error;
            for (std::size_t depth = 0; current && depth < 8; ++depth) {
                if (!text.empty())
                    text += " <- ";
                text += current->domain.Value().substr(0, 96);
                text += "/";
                text += current->code.Value().substr(0, 96);
                text += ": ";
                text += current->message.substr(0, 256);
                current = current->cause.Get();
            }
            return text;
        }

        ConfigurationSchema XrSchema() {
            ConfigurationSchema schema;
            for (const auto &descriptor : XR::XRProjectSettingDescriptors())
                REQUIRE(schema.Register(descriptor).HasValue());
            REQUIRE(schema.Seal().HasValue());
            return schema;
        }

        /** @brief Revokes through the real operator route while native plane scan dispatch is still active. */
        struct PlaneScanRevocation final {
            static inline PlaneScanRevocation *armed{};
            Editor::IEditorXrPlaneCapturePreview &preview;
            bool observed{};

            explicit PlaneScanRevocation(Editor::IEditorXrPlaneCapturePreview &target) : preview(target) {
                armed = this;
            }

            ~PlaneScanRevocation() {
                if (armed == this)
                    armed = nullptr;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Start(XrSpatialContextEXT context, const XrSpatialDiscoverySnapshotCreateInfoEXT *info,
                                                        XrFutureEXT *future) {
                const auto result = OpenXR::SpatialCaptureScript::StartScan(context, info, future);
                if (auto *revocation = std::exchange(armed, nullptr)) {
                    revocation->observed = true;
                    revocation->preview.RevokePlaneCapture();
                }
                return result;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
                if (std::string_view{name} == "xrCreateSpatialDiscoverySnapshotAsyncEXT") {
                    *function = std::bit_cast<PFN_xrVoidFunction>(&Start);
                    return XR_SUCCESS;
                }
                return OpenXR::SpatialCaptureScript::GetProc(instance, name, function);
            }
        };

        /** @brief Owns real installation, native preparation and canonical scene/input parents through explicit cleanup. */
        struct ApplicationFixture final {
            ApplicationNativeScript script;
            OpenXRProduct::InstalledProductFixture product;
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
            std::function<void()> onCheckAdmission;

            explicit ApplicationFixture(const bool render = false, const std::int64_t maximumSpaces = 5,
                                        const XR::XRProjectFeaturePolicy depth = XR::XRProjectFeaturePolicy::Unrequested,
                                        const XR::XRProjectFeaturePolicy hands = XR::XRProjectFeaturePolicy::Unrequested,
                                        const XR::XRProjectFeaturePolicy stage = XR::XRProjectFeaturePolicy::Unrequested,
                                        const bool synthesis = false, std::optional<std::string> controllerBindings = std::nullopt,
                                        const XR::XRProjectFeaturePolicy gaze = XR::XRProjectFeaturePolicy::Unrequested,
                                        const bool planePreview = false,
                                        std::shared_ptr<const Host::IOpenXRPassthroughPermission> passthrough = {},
                                        const XR::IXRHandGesturePolicy *handGestures = nullptr)
                : product{HORO_TEST_OPENXR_LOADER, std::move(controllerBindings)} {
                ConfigurationResolutionRequest request;
                request.invocation.emplace(SettingKey{"xr.enabled"}, ConfigurationInputValue{true, std::nullopt});
                if (planePreview)
                    request.invocation.emplace(SettingKey{"xr.preview.spatial_planes"}, ConfigurationInputValue{true, std::nullopt});
                if (gaze != XR::XRProjectFeaturePolicy::Unrequested) {
                    request.invocation.emplace(SettingKey{"xr.features.eye_gaze"},
                                               ConfigurationInputValue{static_cast<std::int64_t>(gaze), std::nullopt});
                    script.native.gazeExtension = true;
                    ++script.native.extensionCount;
                    script.census.gazeSupport = XR_TRUE;
                }
                if (depth != XR::XRProjectFeaturePolicy::Unrequested) {
                    request.invocation.emplace(SettingKey{"xr.features.depth_composition"},
                                               ConfigurationInputValue{static_cast<std::int64_t>(depth), std::nullopt});
                    script.native.depthExtension = true;
                    ++script.native.extensionCount;
                }
                if (hands != XR::XRProjectFeaturePolicy::Unrequested) {
                    request.invocation.emplace(SettingKey{"xr.features.hand_tracking"},
                                               ConfigurationInputValue{static_cast<std::int64_t>(hands), std::nullopt});
                    request.invocation.emplace(SettingKey{"xr.limits.devices"}, ConfigurationInputValue{std::int64_t{5}, std::nullopt});
                    script.native.handExtension = true;
                    ++script.native.extensionCount;
                    script.census.handsSupported = true;
                }
                if (stage != XR::XRProjectFeaturePolicy::Unrequested)
                    request.invocation.emplace(SettingKey{"xr.features.stage_space"},
                                               ConfigurationInputValue{static_cast<std::int64_t>(stage), std::nullopt});
                if (synthesis) {
                    request.invocation.emplace(SettingKey{"xr.features.space_warp"},
                                               ConfigurationInputValue{static_cast<std::int64_t>(
                                                                           XR::XRProjectFeaturePolicy::OptionalProjection),
                                                                       std::nullopt});
                    script.native.synthesisExtension = true;
                    ++script.native.extensionCount;
                }
                // Play reserves five canonical spaces besides any authored controller pose spaces.
                request.invocation.emplace(SettingKey{"xr.limits.spaces"}, ConfigurationInputValue{maximumSpaces, std::nullopt});
                REQUIRE(configuration.ResolveAndCommit(request).HasValue());
                const auto settings = RequireValue(XR::ResolveXRProjectSettings(configuration.Snapshot()));
                product.policy->passthroughPermission = std::move(passthrough);
                session = std::make_unique<Host::OpenXRApplicationSession>(Host::OpenXRApplicationAuthorities{configuration, *product.host,
                                                                                                              product.Root(),
                                                                                                              product.host->Generation(),
                                                                                                              [this] {
                    if (onCheckAdmission)
                        onCheckAdmission();
                    return admission ? Result<void>::Success() : Result<void>::Failure(MakeError(XR::XRErrors::RuntimeRejected));
                }, handGestures},
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
                frontend =
                    RequireValue(Render::RenderFrontend::Create(renderers, Render::RenderBackendId{render ? "vulkan" : "null"},
                                                                Render::RenderBackendConfig{}, {}, {.budget = {.maximumBlocks = 4096}}));
                Editor::EditorVulkanViewportCook viewportCook;
                if (render) {
                    const auto cooked = ApplicationVulkanFixture::Cook(false);
                    viewportCook = RequireValue(
                        Editor::LoadEditorVulkanViewportCook(cooked.compiled.payload,
                                                             RequireValue(Render::EncodeReflectedShaderArtifact(cooked.reflected))));
                    const auto shadow = ApplicationVulkanFixture::CookShadow();
                    viewportCook.shadow = RequireValue(
                        Editor::LoadEditorVulkanShadowCook(shadow.compiled.payload,
                                                           RequireValue(Render::EncodeReflectedShaderArtifact(shadow.reflected))));
                }
                viewport = std::make_unique<Editor::EditorViewportRendererVulkan>(*frontend, bridge, std::move(viewportCook));
                if (render) {
                    const auto initialized = viewport->Initialize();
                    INFO((initialized.HasError() ? ViewportInitializationError(initialized.ErrorValue()) : "Viewport initialized."));
                    REQUIRE(initialized.HasValue());
                    const auto cooked = ApplicationVulkanFixture::Cook(true);
                    RequireValue(viewport->PrepareXrProgram(RequireValue(
                        Editor::LoadEditorVulkanMultiviewCook(cooked.compiled.payload,
                                                              RequireValue(Render::EncodeReflectedShaderArtifact(cooked.reflected))))));
                }
            }

            ApplicationFixture(const ApplicationFixture &) = delete;
            ApplicationFixture &operator=(const ApplicationFixture &) = delete;

            /** @brief Delivers real scripted stop events only after the production owner actually requested native exit. */
            Result<bool> ClosePlay() {
                if (!play)
                    return Result<bool>::Success(true);
                const auto closed = play->Close();
                if (closed.HasError() || closed.Value() || !script.events.running || script.events.exits == 0)
                    return closed;
                script.events.Push(XR_SESSION_STATE_STOPPING);
                script.events.Push(XR_SESSION_STATE_IDLE);
                return play->Close();
            }

            ~ApplicationFixture() {
                // Clear only test-injected failure controls. The real owners still perform every native release.
                script.native.fail = {};
                script.native.failRetirement = false;
                script.native.failInstanceRetirement = false;
                script.census.fail = {};
                script.graphics.failure = XR_SUCCESS;
                script.frames.waitResult = XR_SUCCESS;
                script.frames.beginResult = XR_SUCCESS;
                script.frames.endResult = XR_SUCCESS;
                if (vulkan)
                    vulkan->native.commandState.state.Complete();
                if (play) {
                    const auto tail = play->ProgressRenderTail();
                    INFO((tail.HasError() ? OpenXRProduct::ErrorText(tail.ErrorValue()) : "frame tail retired"));
                    auto closed = ClosePlay();
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

            Input::InputActionCollection InputPort(const std::optional<Input::PlayerId> player = std::nullopt) {
                const std::vector<Input::ActionDescriptor> registered{
                    {Input::ActionId{"select"},
                     bindings->schema.actions.front().valueType,
                     Input::InputContextId{"gameplay"},
                     true,
                     {{.kind = Input::BindingControlKind::Key, .key = Input::Key::Space}}}};
                REQUIRE(router.SetActionMap(registered).HasValue());
                inputContext = router.PushContext(Input::InputContextId{"gameplay"}, Input::InputContextKind::Gameplay);
                return RequireValue(player ? router.ActionCollectionForPlayer(inputContext, *player)
                                           : router.ActionCollection(inputContext));
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

    TEST_CASE("Actual application Play rejects an immutable space budget below its canonical reservation", "[xr][application][play]") {
        ApplicationFixture fixture{true, 2};
        fixture.CreatePlay();
        REQUIRE(fixture.play->SetPlayState(Editor::EditorPlayState::Playing).HasValue());
        const auto input = fixture.InputPort();
        REQUIRE(fixture.play->AttachInput(input, fixture.source.Port()).HasValue());
        auto participant = fixture.play->MakeSceneParticipant();
        const auto prepared = participant->Prepare(fixture.definition, fixture.scene->View());
        REQUIRE(prepared.HasError());
        CHECK(prepared.ErrorValue().code.Value() == XR::XRErrors::CapacityExceeded.code.Value());
        CHECK_FALSE(fixture.play->UsesXrScene());
        CHECK(fixture.play->PresentationState() == Host::OpenXRPlayPresentation::Pending);
    }

    TEST_CASE("Selected OpenXR Play calibrates mounted origin before structural publication", "[xr][application][play][mounted_origin]") {
        namespace WS = WorldStreaming;
        ApplicationFixture fixture{true};
        fixture.CreatePlay();
        REQUIRE(fixture.play->SetPlayState(Editor::EditorPlayState::Playing).HasValue());
        REQUIRE(fixture.play->AttachInput(fixture.InputPort(), fixture.source.Port()).HasValue());
        constexpr std::int64_t globalX = 9'007'199'254'740'993;
        const auto originPosition = Math::WorldCoordinate64::FromMillimeters(globalX, 0, 0);
        const auto anchor = Math::WorldCoordinate64::FromMillimeters(globalX + 1000, 2000, 3000);
        const auto origin = RequireValue(
            WS::OriginFrame::Create({RequireValue(WS::OriginFrameId::Create(7)), RequireValue(WS::OriginFrameRevision::Create(3)),
                                     RequireValue(WS::OriginGeneration::Create(1))},
                                    originPosition));
        auto owner = RequireValue(WS::OriginFrameOwner::Create(origin));
        const auto view = RequireValue(fixture.frontend->CreateRenderViewIdentity());
        const WS::OriginRebaseParticipantRequirement requirement{RequireValue(WS::OriginRebaseParticipantId::Create(7)),
                                                                 RequireValue(WS::OriginRebaseParticipantRevision::Create(1))};
        auto adapter = RequireValue(Render::RenderOriginSceneAdapter::Create(*owner, view, requirement, {2, 0, 0}));
        const auto runtime = fixture.scene->View().RuntimeId();
        const Render::RenderOriginSceneSource source{{originPosition,
                                                      Math::WorldCoordinate64::FromMillimeters(9'007'199'254'740'993, 0, -1000)},
                                                     {},
                                                     {},
                                                     runtime.value};
        const auto frame = RequireValue(adapter->Capture(source, Render::RenderOriginCoordinateBasis::OriginRelative));
        std::shared_ptr<const Render::RenderOriginLocalScene> packet =
            std::make_shared<Render::RenderOriginLocalScene>(RequireValue(frame.OriginScene({})));
        bool read{};
        Math::Transform placement{.translation = RequireValue(origin.ToLocal(anchor)).Value()};
        auto participant = fixture.play->MakeSceneParticipant([&](const Runtime::RuntimeSceneView &candidate, Runtime::SceneObjectId root) {
            CHECK(candidate.SameIncarnation(fixture.scene->View()));
            CHECK(root.IsValid());
            read = true;
            return Result<Editor::EditorMountedXrScene>::Success({packet, root, placement});
        });
        SECTION("exact relation is observed at the same native clock before publication") {
            auto prepared = participant->Prepare(fixture.definition, fixture.scene->View());
            REQUIRE(prepared.HasValue());
            CHECK(read);
            CHECK_FALSE(fixture.play->UsesXrScene());
            CHECK(fixture.script.actions.tracking.locatedTime == fixture.script.clock.sample);
            REQUIRE(prepared.Value()->ValidatePublication().HasValue());
            prepared.Value()->Publish();
            CHECK(fixture.play->UsesXrScene());
            const auto retained = fixture.session->RetainedSession();
            REQUIRE(retained.has_value());
            const auto session = fixture.play->ReadReferences(*retained);
            REQUIRE(session.HasValue());
            CHECK(session.Value().local.worldOriginRevision.Value() == 3);
            owner->Shutdown();
            CHECK(fixture.play->UpdateScene(fixture.scene->View()).HasError());
            CHECK_FALSE(fixture.play->UsesXrScene());
        }
        SECTION("mismatched authored placement cannot publish the candidate") {
            placement.translation.x = 1.25F;
            const auto rejected = participant->Prepare(fixture.definition, fixture.scene->View());
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == XR::XRErrors::PoseInvalid.code.Value());
            CHECK(fixture.script.actions.tracking.locatedTime == fixture.script.clock.sample);
            CHECK_FALSE(fixture.play->UsesXrScene());
        }
        SECTION("inferred location cannot publish the candidate") {
            fixture.script.actions.tracking.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
            const auto rejected = participant->Prepare(fixture.definition, fixture.scene->View());
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == XR::XRErrors::OperationUnavailable.code.Value());
            CHECK_FALSE(fixture.play->UsesXrScene());
        }
        SECTION("the retained origin must remain current through the publication gate") {
            auto prepared = participant->Prepare(fixture.definition, fixture.scene->View());
            REQUIRE(prepared.HasValue());
            owner->Shutdown();
            const auto stale = prepared.Value()->ValidatePublication();
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == WS::WorldStreamingErrors::OriginFrameStale.code.Value());
            CHECK_FALSE(fixture.play->UsesXrScene());
            prepared.Value()->Shutdown();
        }
        SECTION("a published rebase retires the old XR session and calibrates the unchanged global anchor again") {
            auto first = RequireValue(participant->Prepare(fixture.definition, fixture.scene->View()));
            REQUIRE(first->ValidatePublication().HasValue());
            first->Publish();
            const auto initialSession = fixture.session->RetainedSession();
            REQUIRE(initialSession.has_value());
            const auto firstSession = *initialSession;
            const auto firstReferences = RequireValue(fixture.play->ReadReferences(firstSession));
            const auto firstWorld = RequireValue(fixture.session->WorldSpace(firstSession));
            CHECK(firstReferences.local.worldOriginRevision.Value() == 3);
            REQUIRE(fixture.session->HasInputProjection());
            fixture.script.events.session = fixture.script.native.lastSession;
            fixture.script.events.Push(XR_SESSION_STATE_READY);
            fixture.script.events.Push(XR_SESSION_STATE_SYNCHRONIZED);
            fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
            fixture.script.events.Push(XR_SESSION_STATE_FOCUSED);
            REQUIRE(fixture.play->PollEvents().HasValue());
            REQUIRE(fixture.script.events.begins == 1);
            REQUIRE(fixture.play->PresentationState() == Host::OpenXRPlayPresentation::FrameAdmitted);

            const auto policy =
                RequireValue(WS::OriginShiftPolicy::Create({.id = RequireValue(WS::OriginShiftPolicyId::Create(1)),
                                                            .revision = RequireValue(WS::OriginShiftPolicyRevision::Create(1)),
                                                            .frame = origin.Binding().identity,
                                                            .mode = WS::OriginShiftRuntimeMode::EditorPreview}));
            const auto focus = Math::WorldCoordinate64::FromMillimeters(globalX + 2'000'000, 0, 0);
            const auto shift = RequireValue(
                WS::EvaluateOriginShift(policy, {policy.Id(), policy.Revision(), origin, WS::OriginShiftPolicyState::Active},
                                        {RequireValue(WS::OriginShiftRequestId::Create(1)), WS::OriginShiftRequester::Editor, focus}));
            REQUIRE(shift.kind == WS::OriginShiftDecisionKind::RequestShift);
            const std::array required{requirement};
            const std::array participants{static_cast<WS::IOriginRebaseParticipant *>(adapter.get())};
            auto rebase = RequireValue(WS::OriginRebaseTransaction::Prepare({RequireValue(WS::OriginRebaseTransactionId::Create(1)), shift,
                                                                             origin, 1, WS::OriginRebaseLifecycle::Active},
                                                                            required, participants));
            REQUIRE(rebase.Commit(*owner, WS::OriginRebaseCommitPoint::PostSimulation, WS::OriginRebaseLifecycle::Active).HasValue());
            CHECK(packet->origin.Get().HasError());
            CHECK(first->ValidatePublication().HasError());
            const auto previousWaits = fixture.script.frames.waits;
            const auto staleFrame = fixture.play->PrepareRenderFrame(1);
            REQUIRE(staleFrame.HasError());
            CHECK(staleFrame.ErrorValue().code.Value() == WS::WorldStreamingErrors::OriginFrameStale.code.Value());
            CHECK(fixture.script.frames.waits == previousWaits);
            const CancellationSource cancellation;
            const auto staleInput = fixture.play->BuildInputSnapshot({.frameNumber = 1, .cancellation = cancellation.Token()}, {});
            REQUIRE(staleInput.HasError());
            CHECK(staleInput.ErrorValue().code.Value() == WS::WorldStreamingErrors::OriginFrameStale.code.Value());
            CHECK_FALSE(fixture.play->UsesXrScene());
            CHECK_FALSE(fixture.session->HasInputProjection());
            REQUIRE(fixture.play->PollEvents().HasValue());
            REQUIRE(fixture.script.events.exits == 1);
            fixture.script.events.Push(XR_SESSION_STATE_STOPPING);
            fixture.script.events.Push(XR_SESSION_STATE_IDLE);
            REQUIRE(fixture.play->PollEvents().HasValue());
            CHECK_FALSE(fixture.session->RetainedSession());
            REQUIRE(fixture.play->BuildInputSnapshot({.frameNumber = 2, .cancellation = cancellation.Token()}, {}).HasValue());

            fixture.scene = RequireValue(Runtime::RuntimeScene::Create(fixture.definition, {2}));
            auto rebasedSource = source;
            rebasedSource.sceneDomain = fixture.scene->View().RuntimeId().value;
            const auto rebasedFrame = RequireValue(adapter->Capture(rebasedSource, Render::RenderOriginCoordinateBasis::OriginRelative));
            packet = std::make_shared<Render::RenderOriginLocalScene>(RequireValue(rebasedFrame.OriginScene({})));
            const auto rebased = RequireValue(packet->origin.Get());
            REQUIRE(rebased.Origin() == focus);
            const auto projectedAnchor = RequireValue(rebased.ToLocal(anchor));
            CHECK(projectedAnchor.Generation().Value() == 2);
            CHECK(RequireValue(rebased.ToGlobal(projectedAnchor)) == anchor);
            placement.translation = projectedAnchor.Value();
            REQUIRE(placement.translation == Math::Vec3{-1999.0F, 2.0F, 3.0F});
            auto &tracking = fixture.script.actions.tracking;
            tracking.locatedPosition = {placement.translation.x, placement.translation.y, placement.translation.z};
            ++fixture.script.clock.sample;
            auto second = RequireValue(participant->Prepare(fixture.definition, fixture.scene->View()));
            CHECK(tracking.locatedTime == fixture.script.clock.sample);
            REQUIRE_FALSE(tracking.references.empty());
            CHECK(tracking.references.back().poseInReferenceSpace.position.x == -placement.translation.x);
            REQUIRE(second->ValidatePublication().HasValue());
            second->Publish();
            first->Shutdown();
            CHECK(fixture.play->UsesXrScene());
            REQUIRE(fixture.session->HasInputProjection());
            const auto replacementSession = fixture.session->RetainedSession();
            REQUIRE(replacementSession.has_value());
            const auto secondSession = *replacementSession;
            CHECK(secondSession != firstSession);
            const auto secondReferences = RequireValue(fixture.play->ReadReferences(secondSession));
            CHECK(secondReferences.local.worldOriginRevision.Value() == 4);
            CHECK(secondReferences.local.id != firstReferences.local.id);
            CHECK(RequireValue(fixture.session->WorldSpace(secondSession)) != firstWorld);
            CHECK(fixture.play->ReadReferences(firstSession).HasError());
            CHECK(fixture.session->BorrowWorldSpace(firstWorld).HasError());
            fixture.script.events.session = fixture.script.native.lastSession;
            fixture.script.events.Push(XR_SESSION_STATE_READY);
            fixture.script.events.Push(XR_SESSION_STATE_SYNCHRONIZED);
            fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
            fixture.script.events.Push(XR_SESSION_STATE_FOCUSED);
            REQUIRE(fixture.play->PollEvents().HasValue());
            CHECK(fixture.script.events.begins == 2);
            CHECK(fixture.play->PresentationState() == Host::OpenXRPlayPresentation::FrameAdmitted);
        }
        SECTION("expired origin never prepares native tracking") {
            const auto preparedCalls = fixture.script.native.calls.size();
            owner->Shutdown();
            const auto stale = participant->Prepare(fixture.definition, fixture.scene->View());
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == WS::WorldStreamingErrors::OriginFrameStale.code.Value());
            CHECK(fixture.script.native.calls.size() == preparedCalls);
        }
    }

    TEST_CASE("Input revocation cancels the exact unpublished application Play attempt", "[xr][application][play]") {
        bool revoked{};
        ApplicationFixture fixture;
        fixture.CreatePlay();
        REQUIRE(fixture.script.native.liveSessions == 1);
        const auto preparedSessions = std::ranges::count(fixture.script.native.calls, "session");
        const auto input = fixture.InputPort();
        REQUIRE(fixture.play->AttachInput(input, fixture.source.Port()).HasValue());
        auto participant = fixture.play->MakeSceneParticipant();
        fixture.onCheckAdmission = [&] {
            if (!std::exchange(revoked, true))
                static_cast<void>(fixture.play->DetachInput());
        };

        const auto prepared = participant->Prepare(fixture.definition, fixture.scene->View());
        REQUIRE(prepared.HasError());
        CHECK(revoked);
        CHECK(prepared.ErrorValue().code.Value() == XR::XRSessionErrors::ActivationCancelled.code.Value());
        CHECK_FALSE(fixture.play->UsesXrScene());
        CHECK(std::ranges::count(fixture.script.native.calls, "session") == preparedSessions);
        CHECK(fixture.script.native.liveSessions == 1);  // The App owner retains its existing standby until physical retirement.
        fixture.onCheckAdmission = {};
        const auto closed = fixture.play->Close();
        REQUIRE(closed.HasValue());
        CHECK(closed.Value());
        CHECK(fixture.script.native.liveSessions == 0);
        CHECK(fixture.script.actions.liveSets == 0);
    }

    TEST_CASE("XR setup checks a private session and leaves the selected owner available for Play", "[xr][application][setup]") {
        using enum Editor::EditorXrSetupTestState;
        ApplicationFixture fixture;
        fixture.CreatePlay();
        const auto initial = fixture.play->ReadSetup();
        REQUIRE(initial.loaderMajor > 0);
        REQUIRE(initial.rendererReady);
        REQUIRE(initial.mayRequestTest);
        REQUIRE(fixture.script.native.liveSessions == 1);
        const auto created = std::ranges::count(fixture.script.native.calls, "session");

        const auto cancelled = RequireValue(fixture.play->RequestSetupTest());
        REQUIRE(fixture.play->RequestSetupTest().HasError());
        fixture.play->CancelSetupTest(cancelled);
        fixture.play->PumpSetupTest();
        CHECK(fixture.play->ReadSetup().test == Cancelled);
        CHECK(fixture.script.native.liveSessions == 1);
        CHECK(std::ranges::count(fixture.script.native.calls, "session") == created);

        const auto next = RequireValue(fixture.play->RequestSetupTest());
        REQUIRE(next != cancelled);
        fixture.play->CancelSetupTest(cancelled);
        CHECK(fixture.play->ReadSetup().test == Queued);
        fixture.play->PumpSetupTest();
        CHECK(fixture.play->ReadSetup().test == Passed);
        CHECK(std::ranges::count(fixture.script.native.calls, "session") == created + 1);
        CHECK(fixture.script.native.liveSessions == 0);
        CHECK_FALSE(fixture.play->UsesXrScene());
        CHECK_FALSE(fixture.session->RetainedSession());
        REQUIRE(fixture.session->PrepareSession().HasValue());
        CHECK(std::ranges::count(fixture.script.native.calls, "session") == created + 2);
        CHECK(fixture.script.native.liveSessions == 1);
        REQUIRE(fixture.play->Close().HasValue());
        CHECK(fixture.script.native.liveSessions == 0);
        CHECK_FALSE(fixture.play->ReadSetup().mayRequestTest);
        CHECK(fixture.play->RequestSetupTest().HasError());
    }

    TEST_CASE("XR setup hands sealed packaged compatibility to the current device qualification", "[xr][application][setup]") {
        using enum Editor::EditorXrSetupTestState;
        ApplicationFixture fixture;
        fixture.script.census.implementationVersion = XR_MAKE_VERSION(9, 8, 70'000);
        fixture.CreatePlay();
        REQUIRE(fixture.play->ReadDeviceQualificationPreflight().HasError());
        RequireValue(fixture.play->RequestSetupTest());
        fixture.play->PumpSetupTest();
        REQUIRE(fixture.play->ReadSetup().test == Passed);
        const auto admitted = RequireValue(fixture.play->ReadDeviceQualificationPreflight());
        const auto &row = admitted.Compatibility();
        CHECK(row.loader.LoaderSource() == XR::XRLoaderSourcePolicy::BundledVerified);
        CHECK(row.loader.RuntimeSelection() == XR::XRRuntimeSelectionPolicy::SystemDefault);
        const Host::OpenXRRuntimeVersion implementation{9, 8, 70'000};
        CHECK(row.runtimeVersion == implementation);
        CHECK(row.loader.LoaderApiVersion() >= row.admittedLoaderApis.minimum);
        CHECK(row.loader.LoaderApiVersion() <= row.admittedLoaderApis.maximum);
        CHECK(row.system.IsValid());
        CHECK(row.system.runtime == row.loader.RuntimeGeneration());
        CHECK(row.platform == fixture.product.platform);
        CHECK(row.component.rendererBridge == fixture.product.policy->component.rendererBridge);
        CHECK(row.profile == XR::XRFeatureProfile::Projection1_0);
        CHECK(row.loaderDigest == fixture.product.loaderSignature.artifactDigest);
        CHECK(row.signedDeploymentDigest == fixture.product.policy->deploymentSignature.artifactDigest);
        const Render::VulkanApiVersion renderer{1, 3, 0};
        CHECK(row.graphicsApi == renderer);
        CHECK(row.externallySuppliedOsAbiCount > 0);
        CHECK(row.referenceSpaceCount == fixture.script.census.spaces);
        CHECK(row.runtimeSwapchainFormatCount == fixture.script.census.formats);
        const auto graphics = static_cast<std::size_t>(Host::OpenXRPackagedRoute::Graphics);
        const auto time = static_cast<std::size_t>(Host::OpenXRPackagedRoute::PlatformTime);
        const auto hands = static_cast<std::size_t>(Host::OpenXRPackagedRoute::HandTracking);
        CHECK(row.extensionRoutes[graphics].state == Native::NativeExtensionState::Enabled);
        CHECK(row.extensionRoutes[time].state == Native::NativeExtensionState::Enabled);
        CHECK(row.extensionRoutes[hands].state == Native::NativeExtensionState::Absent);
        CHECK(row.extensionRoutes[static_cast<std::size_t>(Host::OpenXRPackagedRoute::EyeGazeObservation)].state ==
              Native::NativeExtensionState::Absent);
        CHECK(row.eyeGaze == XR::XRCapabilityState::Unsupported);
        CHECK(row.extensionRoutes[time].discoveredVersion > 0);
        CHECK(fixture.script.native.liveSessions == 0);
        REQUIRE(fixture.session->ValidateDeviceQualification(admitted).HasValue());

        SECTION("a changed native device invalidates the retained report") {
            fixture.graphics.deviceIdentity = 9;
            const auto stale = fixture.play->ReadDeviceQualificationPreflight();
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == XR::XRErrors::CapabilityStale.code.Value());
        }
        SECTION("a failed replacement probe revokes the old result and retires the test session") {
            fixture.script.native.instanceDispatchAbsent = "xrWaitFrame";
            RequireValue(fixture.play->RequestSetupTest());
            fixture.play->PumpSetupTest();
            CHECK(fixture.play->ReadSetup().test == Failed);
            CHECK(fixture.play->ReadDeviceQualificationPreflight().HasError());
            CHECK(fixture.session->ValidateDeviceQualification(admitted).HasError());
            CHECK(fixture.script.native.liveSessions == 0);
            CHECK(fixture.script.native.liveInstances == 1);
            fixture.script.native.instanceDispatchAbsent = {};
            RequireValue(fixture.play->RequestSetupTest());
            fixture.play->PumpSetupTest();
            const auto replacement = RequireValue(fixture.play->ReadDeviceQualificationPreflight());
            CHECK(fixture.session->ValidateDeviceQualification(replacement).HasValue());
            CHECK(fixture.session->ValidateDeviceQualification(admitted).HasError());
        }
        SECTION("changing installed-host admission invalidates the prerequisite") {
            fixture.admission = false;
            CHECK(fixture.play->ReadDeviceQualificationPreflight().HasError());
        }

        REQUIRE(RequireValue(fixture.ClosePlay()));
        fixture.play.reset();
        RequireValue(fixture.session->Close());
        CHECK(fixture.session->ValidateDeviceQualification(admitted).HasError());
    }

    TEST_CASE("XR setup rejects unsupported or invalid swapchain formats and withdraws prior qualification", "[xr][application][setup]") {
        using enum Editor::EditorXrSetupTestState;
        using enum Editor::EditorXrSetupIssue;
        ApplicationFixture fixture;
        fixture.CreatePlay();
        const auto run = [&] {
            RequireValue(fixture.play->RequestSetupTest());
            fixture.play->PumpSetupTest();
        };
        run();
        REQUIRE(fixture.play->ReadSetup().test == Passed);
        const auto prior = RequireValue(fixture.play->ReadDeviceQualificationPreflight());
        constexpr std::array incompatibleFormats{std::int64_t{VK_FORMAT_D32_SFLOAT}, std::int64_t{VK_FORMAT_R8G8B8A8_SRGB}};
        constexpr std::array malformedFormats{std::int64_t{VK_FORMAT_R8G8B8A8_UNORM}, std::int64_t{VK_FORMAT_R8G8B8A8_UNORM}};
        auto issue = Unsupported;
        const ErrorCodeDescriptor *reason = &XR::XRErrors::OperationUnsupported;
        SECTION("only nonmatching formats, including a real depth format") {
            fixture.script.census.formatPublication = incompatibleFormats;
        }
        SECTION("duplicate native format") {
            fixture.script.census.formatPublication = malformedFormats;
            issue = Invalid;
            reason = &XR::XRErrors::OperationInvalid;
        }
        SECTION("over-capacity native format inventory") {
            fixture.script.census.formats = Native::NativeSessionCensus{}.formats.size() + 1;
            issue = Capacity;
            reason = &XR::XRErrors::CapacityExceeded;
        }

        run();
        CHECK(fixture.play->ReadSetup().test == Failed);
        CHECK(fixture.play->ReadSetup().issue == issue);
        CHECK(fixture.play->ReadDeviceQualificationPreflight().HasError());
        CHECK(fixture.session->ValidateDeviceQualification(prior).HasError());
        CHECK(fixture.script.native.liveSessions == 0);
        CHECK(fixture.script.native.liveInstances == 1);

        const auto &row = prior.Compatibility();
        const Host::OpenXRDesktopProbeTarget target{row.platform.operatingSystem, "Actual scripted runtime", "Observed native system",
                                                    row.installDigest};
        const auto rejected = fixture.session->ProbeDesktopTarget(*fixture.bindings, target);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == reason->code.Value());
        CHECK(fixture.script.native.liveSessions == 0);

        constexpr std::array supportedFormats{std::int64_t{VK_FORMAT_D32_SFLOAT}, std::int64_t{VK_FORMAT_R8G8B8A8_UNORM}};
        fixture.script.census.formats = supportedFormats.size();
        fixture.script.census.formatPublication = supportedFormats;
        run();
        REQUIRE(fixture.play->ReadSetup().test == Passed);
        const auto recovered = RequireValue(fixture.play->ReadDeviceQualificationPreflight());
        CHECK(recovered.Compatibility().runtimeSwapchainFormatCount == supportedFormats.size());
        CHECK(fixture.session->ValidateDeviceQualification(recovered).HasValue());
        CHECK(fixture.session->ValidateDeviceQualification(prior).HasError());
        CHECK(fixture.script.native.liveSessions == 0);
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("XR setup probes the explicit installed desktop runtime and HMD in the actual package owner",
              "[xr][application][setup][deployment]") {
        ApplicationFixture fixture;
        fixture.CreatePlay();
        RequireValue(fixture.play->RequestSetupTest());
        fixture.play->PumpSetupTest();
        auto prior = RequireValue(fixture.play->ReadDeviceQualificationPreflight());
        const auto &row = prior.Compatibility();
        Host::OpenXRDesktopProbeTarget target{row.platform.operatingSystem, "Actual scripted runtime", "Observed native system",
                                              row.installDigest};
        const ErrorCodeDescriptor *error = nullptr;
        SECTION("matches the selected package/runtime/system and retires the new probe") {}
        SECTION("runtime was replaced") {
            target.runtimeName = "A different runtime";
            error = &XR::XRErrors::RuntimeRejected;
        }
        SECTION("explicit target is disconnected or mismatched") {
            target.systemName = "A different headset";
            error = &XR::XRErrors::RuntimeRejected;
        }
        SECTION("host platform is unsupported") {
            target.operatingSystem = "android";
            error = &XR::XRErrors::OperationUnsupported;
        }
        SECTION("package digest was replaced") {
            target.installDigest.bytes[0] ^= 1U;
            error = &XR::XRErrors::LoaderPreflightStale;
        }
        const auto created = std::ranges::count(fixture.script.native.calls, "session");
        const auto probed = fixture.session->ProbeDesktopTarget(*fixture.bindings, target);
        if (error) {
            REQUIRE(probed.HasError());
            CHECK(probed.ErrorValue().code.Value() == error->code.Value());
            CHECK(std::ranges::count(fixture.script.native.calls, "session") == created);
        } else {
            REQUIRE(probed.HasValue());
            REQUIRE(fixture.session->ValidateDeviceQualification(probed.Value()).HasValue());
            CHECK(fixture.session->ValidateDeviceQualification(prior).HasError());
            CHECK(std::ranges::count(fixture.script.native.calls, "session") == created + 1);
        }
        CHECK(fixture.script.native.liveSessions == 0);
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("XR setup rejects an ambiguous signed external dependency inventory before opening the native loader",
              "[xr][application][setup]") {
        OpenXRProduct::Json dependencies = {"libc.so.6", "libc.so.6"};
        SECTION("over capacity") {
            dependencies = OpenXRProduct::Json::array();
            for (std::size_t index = 0; index < 33; ++index)
                dependencies.push_back("lib" + std::to_string(index) + ".so");
        }
        SECTION("path rather than ABI name") {
            dependencies = {"vendor/libopenxr_runtime.so"};
        }
        SECTION("duplicate ABI names") {}
        OpenXRProduct::InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER, std::nullopt, std::move(dependencies)};
        const auto rejected =
            Host::SelectInstalledOpenXRProduct(*fixture.host, fixture.Root(), fixture.host->Generation(), &*fixture.policy, {});
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == XR::XRErrors::OperationUnavailable.code.Value());
    }

    TEST_CASE("XR setup distinguishes busy Play, permission denial, and retained cleanup", "[xr][application][setup]") {
        ApplicationFixture fixture;
        fixture.CreatePlay();
        const auto queued = RequireValue(fixture.play->RequestSetupTest());
        SECTION("the owner can enter Play between enqueue and pump") {
            REQUIRE(fixture.play->SetPlayState(Editor::EditorPlayState::Playing).HasValue());
            fixture.play->PumpSetupTest();
            CHECK(fixture.play->ReadSetup().issue == Editor::EditorXrSetupIssue::Busy);
            CHECK(fixture.script.native.liveSessions == 1);
        }
        SECTION("native permission cannot become successful preflight") {
            fixture.script.native.fail = "session";
            fixture.script.native.failureResult = XR_ERROR_PERMISSION_INSUFFICIENT;
            fixture.play->PumpSetupTest();
            CHECK(fixture.play->ReadSetup().issue == Editor::EditorXrSetupIssue::Permission);
            CHECK(fixture.script.native.liveSessions == 0);
            CHECK(fixture.script.native.liveInstances == 1);
            fixture.script.native.fail = {};
            REQUIRE(fixture.play->RequestSetupTest().HasValue());
            fixture.play->PumpSetupTest();
            CHECK(fixture.play->ReadSetup().test == Editor::EditorXrSetupTestState::Passed);
            CHECK(fixture.script.native.liveSessions == 0);
        }
        SECTION("a retained exact session must retire on retry") {
            fixture.script.native.failRetirement = true;
            fixture.play->PumpSetupTest();
            CHECK(fixture.play->ReadSetup().issue == Editor::EditorXrSetupIssue::Cleanup);
            CHECK(fixture.script.native.liveSessions == 1);
            fixture.script.native.failRetirement = false;
            REQUIRE(RequireValue(fixture.play->RequestSetupTest()) != queued);
            fixture.play->PumpSetupTest();
            CHECK(fixture.play->ReadSetup().test == Editor::EditorXrSetupTestState::Passed);
            CHECK(fixture.script.native.liveSessions == 0);
        }
        REQUIRE(fixture.play->Close().HasValue());
    }

    TEST_CASE("XR setup rejects an under-budget profile before entering a native session test", "[xr][application][setup]") {
        ApplicationFixture fixture{false, 4};
        fixture.CreatePlay();
        REQUIRE(fixture.play->RequestSetupTest().HasValue());
        fixture.play->PumpSetupTest();
        CHECK(fixture.play->ReadSetup().issue == Editor::EditorXrSetupIssue::Capacity);
        CHECK(fixture.script.native.liveSessions == 1);
        REQUIRE(fixture.play->Close().HasValue());
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

        class AppInputSample final : public Runtime::RuntimeLifecycleParticipant {
        public:
            explicit AppInputSample(Host::OpenXRPlayHost &play) : play_(play) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {}

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
                if (phase == Runtime::RuntimePhase::BuildInputSnapshot)
                    result.emplace(play_.BuildInputSnapshot(context, raw_));
                return Result<void>::Success();
            }

            std::optional<Result<void>> result;

        private:
            Host::OpenXRPlayHost &play_;
            Input::RawInputSnapshot raw_;
        };

        std::unique_ptr<Runtime::SceneActivationCandidate> ActivatePlay(auto &fixture, const std::optional<Input::PlayerId> player) {
            REQUIRE(fixture.play);
            REQUIRE(fixture.play->SetPlayState(Editor::EditorPlayState::Playing).HasValue());
            const auto input = fixture.InputPort(player);
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

        std::unique_ptr<Runtime::SceneActivationCandidate> PublishPlay(ApplicationFixture &fixture,
                                                                       const std::optional<Input::PlayerId> player = std::nullopt) {
            fixture.CreatePlay();
            return ActivatePlay(fixture, player);
        }
    }  // namespace

    TEST_CASE("Signed XR Setup can observe gaze hardware while Play creates only ordinary tracking actions",
              "[xr][application][setup][gaze]") {
        using enum XR::XRCapabilityState;
        ApplicationFixture fixture{true};
        fixture.script.native.gazeExtension = true;
        ++fixture.script.native.extensionCount;
        fixture.script.census.gazeSupport = XR_TRUE;
        SECTION("reversible packaged Setup reports only policy-gated capability") {
            fixture.CreatePlay();
            RequireValue(fixture.play->RequestSetupTest());
            fixture.play->PumpSetupTest();
            REQUIRE(fixture.play->ReadSetup().test == Editor::EditorXrSetupTestState::Passed);
            const auto receipt = RequireValue(fixture.play->ReadDeviceQualificationPreflight());
            const auto &compatibility = receipt.Compatibility();
            CHECK(compatibility.eyeGaze == DisabledByPolicy);
            constexpr auto route = static_cast<std::size_t>(Host::OpenXRPackagedRoute::EyeGazeObservation);
            CHECK(compatibility.extensionRoutes[route].state == Native::NativeExtensionState::Enabled);
            CHECK(fixture.script.actions.actionNames.empty());
            CHECK(fixture.script.native.liveSessions == 0);
            REQUIRE(RequireValue(fixture.ClosePlay()));
            fixture.play.reset();
            REQUIRE(fixture.session->Close().HasValue());
            CHECK(fixture.session->ValidateDeviceQualification(receipt).HasError());
        }
        SECTION("normal Play never prepares a gaze action") {
            auto scene = PublishPlay(fixture);
            const auto inspector = fixture.play->ReadInspection();
            const auto gaze =
                std::ranges::find(inspector.capabilities, XR::XRCapability::EyeGaze, &Editor::EditorXrInspectionCapability::kind);
            REQUIRE(gaze != inspector.capabilities.end());
            CHECK(gaze->observed == DisabledByPolicy);
            CHECK(gaze->decision == XR::XRFeatureDecisionState::NotRequested);
            CHECK(std::ranges::find(fixture.script.native.enabledExtensions, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) !=
                  fixture.script.native.enabledExtensions.end());
            CHECK_FALSE(fixture.script.actions.actionNames.empty());
            CHECK(std::ranges::none_of(fixture.script.actions.actionNames, [](const auto &name) {
                return std::string_view{name}.find("gaze") != std::string_view::npos;
            }));
            REQUIRE(RequireValue(fixture.ClosePlay()));
            CHECK(fixture.script.actions.liveActions == 0);
            CHECK(fixture.script.native.liveSessions == 0);
        }
        CHECK(std::ranges::count(fixture.script.census.calls, "eye-gaze-properties") == 1);
    }

    TEST_CASE("Signed XR Play submits reconstruction only with platform and operator grants and the exact alpha frame",
              "[xr][application][play][passthrough]") {
        using enum XR::XRProjectFeaturePolicy;

        struct PlatformPermission final : Host::IOpenXRPassthroughPermission {
            bool permitted{true};
            mutable std::optional<XR::XRPassthroughScope> observed;
            mutable Host::OpenXRPlayHost *revokeOnRead{};

            bool SupportsReconstruction() const noexcept override {
                return true;
            }

            bool Allows(const XR::XRPassthroughScope &scope) const noexcept override {
                if (!observed)
                    observed = scope;
                if (auto *play = std::exchange(revokeOnRead, nullptr))
                    play->RevokePassthrough();
                return permitted && observed == scope && scope.session == scope.world.id.session &&
                       scope.world.kind == XR::XRSpaceKind::World;
            }
        };

        bool installAuthority = true;
        bool arm = true;
        bool revokeArmed = false;
        bool revokeOperatorInFlight = false;
        bool revokeInFlight = false;
        bool revokeDuringBind = false;
        bool revokeBeforeCreate = false;
        bool denyNative = false;
        std::uint32_t layerLimit = 2;
        const auto permission = std::make_shared<PlatformPermission>();
        SECTION("authority and next-session consent") {}
        SECTION("advertisement cannot create platform permission") {
            installAuthority = false;
            arm = false;
        }
        SECTION("an installed permission port does not provide operator consent") {
            arm = false;
        }
        SECTION("Setup revokes its queued one-use consent before entering Play") {
            revokeArmed = true;
        }
        SECTION("the exact scoped platform permission was denied") {
            permission->permitted = false;
        }
        SECTION("the runtime denies the compositor feature even with platform and operator consent") {
            denyNative = true;
        }
        SECTION("the exact native system supports only its ordinary projection layer") {
            layerLimit = 1;
        }
        SECTION("operator revocation during platform admission prevents native activation") {
            revokeBeforeCreate = true;
        }
        SECTION("operator revocation inside frame permission keeps the projection opaque") {
            revokeDuringBind = true;
        }
        SECTION("platform revocation prevents an already rendered alpha frame from submitting underlay") {
            revokeInFlight = true;
        }
        SECTION("Setup revokes its live operator consent before compositor submission") {
            revokeOperatorInFlight = true;
        }

        OpenXR::PassthroughScript native;
        if (denyNative)
            native.createResult = XR_ERROR_NOT_PERMITTED_PASSTHROUGH_FB;
        ApplicationFixture fixture{true,         5,
                                   Unrequested,  Unrequested,
                                   Unrequested,  false,
                                   std::nullopt, Unrequested,
                                   false,        installAuthority ? permission : nullptr};
        constexpr std::array<std::string_view, 1> extensions{XR_FB_PASSTHROUGH_EXTENSION_NAME};
        fixture.script.native.extraExtensions = extensions;
        const auto provider = XR::OpenXRInternal::NativePassthroughExtensionRequest();
        fixture.script.native.extraExtensionVersion = provider ? provider->minimumVersion : 3;
        ++fixture.script.native.extensionCount;
        fixture.script.native.resolveExtra = &OpenXR::PassthroughScript::Resolve;
        fixture.script.census.passthroughCapabilities = XR_PASSTHROUGH_CAPABILITY_BIT_FB;
        fixture.script.census.maximumLayers = layerLimit;
        fixture.CreatePlay();
        Editor::IEditorXrSetup &setup = *fixture.play;
        auto *preview = setup.PassthroughPreview();
        REQUIRE(preview);
        using PreviewState = Editor::EditorXrPassthroughState;
        if (!provider) {
            CHECK(std::ranges::find(fixture.script.native.enabledExtensions, XR_FB_PASSTHROUGH_EXTENSION_NAME) ==
                  fixture.script.native.enabledExtensions.end());
            CHECK(preview->ReadPassthroughPreviewState() == PreviewState::Unavailable);
            CHECK(preview->ArmPassthrough().HasError());
            CHECK(native.featuresCreated == 0);
            return;
        }
        CHECK((std::ranges::find(fixture.script.native.enabledExtensions, XR_FB_PASSTHROUGH_EXTENSION_NAME) !=
               fixture.script.native.enabledExtensions.end()) == installAuthority);
        CHECK(preview->ReadPassthroughPreviewState() == (installAuthority ? PreviewState::Ready : PreviewState::Unavailable));
        if (arm) {
            RequireValue(preview->ArmPassthrough());
            CHECK(preview->ReadPassthroughPreviewState() == PreviewState::Armed);
        }
        if (revokeArmed) {
            preview->RevokePassthrough();
            CHECK(preview->ReadPassthroughPreviewState() == PreviewState::Ready);
        }
        if (!installAuthority)
            CHECK(preview->ArmPassthrough().HasError());
        CHECK(native.featuresCreated == 0);
        CHECK_FALSE(permission->observed);
        if (revokeBeforeCreate)
            permission->revokeOnRead = fixture.play.get();

        auto scene = ActivatePlay(fixture, std::nullopt);
        const bool active = arm && !revokeArmed && permission->permitted && !revokeBeforeCreate && !denyNative && layerLimit >= 2;
        CHECK((fixture.play->ReadPassthroughState() == XR::XRCapabilityState::Available) == active);
        CHECK((preview->ReadPassthroughPreviewState() == PreviewState::Active) == active);
        CHECK(native.featuresCreated == (active ? 1U : 0U));
        CHECK(native.running == active);
        if (denyNative || !permission->permitted) {
            CHECK(fixture.play->ReadPassthroughState() == XR::XRCapabilityState::Denied);
            CHECK(preview->ReadPassthroughPreviewState() == PreviewState::Denied);
        }
        if (arm && !revokeArmed)
            REQUIRE(permission->observed);
        if (active) {
            CHECK(permission->observed->session == *fixture.session->RetainedSession());
            CHECK(permission->observed->world == RequireValue(fixture.session->WorldSpace(permission->observed->session)));
        }
        fixture.script.clock.sample = 1;
        if (revokeDuringBind)
            permission->revokeOnRead = fixture.play.get();
        REQUIRE(RequireValue(fixture.play->PrepareRenderFrame(1)));
        if (revokeDuringBind) {
            CHECK(permission->revokeOnRead == nullptr);
            CHECK_FALSE(native.running);
            CHECK(fixture.play->ReadPassthroughState() == XR::XRCapabilityState::Unavailable);
        }
        auto frame = RequireValue(fixture.frontend->BeginFrame({1, {8, 8}}));
        RequireValue(fixture.play->ExecuteRenderFrame(frame, {}));
        RequireValue(frame.Present());
        RequireValue(fixture.play->FinishGui(true));
        RequireValue(fixture.play->ProgressRenderTail());
        CHECK(fixture.script.frames.ends == 0);
        if (revokeInFlight) {
            permission->permitted = false;
            RequireValue(fixture.play->PollEvents());
            CHECK_FALSE(native.running);
            CHECK(fixture.play->ReadPassthroughState() == XR::XRCapabilityState::Denied);
        }
        if (revokeOperatorInFlight) {
            preview->RevokePassthrough();
            CHECK(preview->ReadPassthroughPreviewState() == PreviewState::Unavailable);
        }
        fixture.vulkan->native.commandState.state.Complete();
        RequireValue(fixture.play->ProgressRenderTail());
        CHECK(fixture.script.frames.ends == 1);
        const bool composed = active && !revokeInFlight && !revokeDuringBind && !revokeOperatorInFlight;
        CHECK(fixture.script.frames.layers == (composed ? 2U : 1U));
        CHECK(fixture.script.frames.projectionFlags ==
              (composed ? std::uint64_t{XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT} : 0U));
        CHECK((fixture.script.frames.passthroughLayer != XR_NULL_HANDLE) == composed);
        RequireValue(fixture.play->SetPlayState(Editor::EditorPlayState::Paused));
        RequireValue(fixture.play->PollEvents());
        CHECK_FALSE(native.running);
        CHECK_FALSE(native.liveFeature);
        CHECK(native.layersDestroyed == (active ? 1U : 0U));
        CHECK(fixture.play->ReadPassthroughState() == XR::XRCapabilityState::Unavailable);
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Signed XR Setup spatial plane policy and operator consent are independent", "[xr][application][play][planes]") {
        using enum XR::XRProjectFeaturePolicy;
        using enum Editor::EditorXrPlaneCaptureState;
        const bool policy = GENERATE(false, true);
        OpenXR::SpatialCaptureScript native;
        ApplicationFixture fixture{true, 6, Unrequested, Unrequested, Unrequested, false, std::nullopt, Unrequested, policy};
        constexpr std::array<std::string_view, 3> extensions{XR_EXT_FUTURE_EXTENSION_NAME, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME,
                                                             XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME};
        fixture.script.native.extraExtensions = extensions;
        fixture.script.native.extensionCount += static_cast<std::uint32_t>(extensions.size());
        fixture.script.census.extraProc = &OpenXR::SpatialCaptureScript::GetProc;
        fixture.CreatePlay();
        auto &preview = *fixture.play->PlaneCapturePreview();
        CHECK(preview.ReadPlaneCaptureState() == (policy ? Ready : Unavailable));
        CHECK(fixture.play->ReadPlaneObservations().HasError());
        CHECK((std::ranges::find(fixture.script.native.enabledExtensions, XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME) !=
               fixture.script.native.enabledExtensions.end()) == policy);
        CHECK(native.contextStarts == 0);
        if (policy) {
            RequireValue(preview.ArmPlaneCapture());
            CHECK(preview.ReadPlaneCaptureState() == Armed);
            CHECK(fixture.play->ReadPlaneObservations().HasError());
            preview.RevokePlaneCapture();
            CHECK(preview.ReadPlaneCaptureState() == Ready);
        } else {
            CHECK(preview.ArmPlaneCapture().HasError());
        }
        CHECK(native.contextStarts == 0);
        CHECK_FALSE(native.hasContext);
        REQUIRE(RequireValue(fixture.ClosePlay()));
        CHECK(fixture.script.native.liveSessions == 0);
    }

    TEST_CASE("Signed XR Play captures bounded room planes only under its one-session grant", "[xr][application][play][planes]") {
        using enum XR::XRProjectFeaturePolicy;
        using enum Editor::EditorXrPlaneCaptureState;
        OpenXR::SpatialCaptureScript native;
        OpenXR::PlaneSnapshotFixture geometry;
        ApplicationFixture fixture{true, 6, Unrequested, Unrequested, Unrequested, false, std::nullopt, Unrequested, true};
        constexpr std::array<std::string_view, 3> extensions{XR_EXT_FUTURE_EXTENSION_NAME, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME,
                                                             XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME};
        fixture.script.native.extraExtensions = extensions;
        fixture.script.native.extensionCount += static_cast<std::uint32_t>(extensions.size());
        fixture.script.census.extraProc = &PlaneScanRevocation::Resolve;
        fixture.CreatePlay();
        auto &preview = *fixture.play->PlaneCapturePreview();
        REQUIRE(preview.ReadPlaneCaptureState() == Ready);
        const bool grant = GENERATE(false, true);
        if (grant)
            RequireValue(preview.ArmPlaneCapture());
        auto scene = ActivatePlay(fixture, std::nullopt);
        auto cutoff = std::make_unique<AppInputSample>(*fixture.play);
        auto *observed = cutoff.get();
        RequireValue(fixture.scheduler->AddParticipant(std::move(cutoff)));
        RequireValue(fixture.scheduler->Startup());
        const auto sample = [&](const std::int64_t now) {
            fixture.script.clock.sample = now;
            RequireValue(fixture.scheduler->RunFrame());
            REQUIRE(observed->result.has_value());
            RequireValue(*observed->result);
            return fixture.play->ReadPlaneObservations();
        };
        CHECK(sample(100'000'000).HasError());
        native.ready = true;
        CHECK(sample(110'000'000).HasError());
        CHECK(native.contextStarts == (grant ? 1U : 0U));
        CHECK(native.hasContext == grant);
        CHECK(sample(360'000'000).HasError());
        const auto batch = sample(370'000'000);
        REQUIRE(batch.HasValue() == grant);
        if (grant) {
            REQUIRE(batch.Value().Planes().size() == 2);
            CHECK(batch.Value().Scope().space.kind == XR::XRSpaceKind::World);
            CHECK(batch.Value().Planes()[0].classification == XR::XRPlaneClassification::Platform);
            const auto &plane = batch.Value().Planes().front();
            const auto normal = plane.orientation.Rotate({0.0F, 1.0F, 0.0F});
            const XR::XRSpatialHitRequest ray{batch.Value().Scope(),
                                              {plane.centerMeters + normal, -normal, 0.0F, 2.0F},
                                              batch.Value().SampledAt(),
                                              Duration::FromMilliseconds(500),
                                              2};
            const auto hits = RequireValue(fixture.play->QueryPlaneHits(ray));
            REQUIRE(hits.Hits().size() == 1);
            CHECK(hits.Source() == batch.Value().Scope());
            CHECK(hits.SampledAt() == batch.Value().SampledAt());
            CHECK(hits.Hits().front().plane == plane.id);
            CHECK(hits.Hits().front().intersection.distance > 0.99F);
            CHECK(hits.Hits().front().intersection.distance < 1.01F);
            CHECK(hits.Hits().front().confidence == plane.confidence);
            CHECK(preview.ReadPlaneCaptureState() == Tracking);
            CHECK(native.scans == 1);
            SECTION("operator revokes after the next scan is queued") {
                CHECK(sample(620'000'000).HasValue());
                preview.RevokePlaneCapture();
            }
            SECTION("operator revokes during the native scan call") {
                PlaneScanRevocation revocation{preview};
                CHECK(sample(620'000'000).HasError());
                CHECK(revocation.observed);
                CHECK(preview.ReadPlaneCaptureState() == Unavailable);
            }
            CHECK(native.scans == 2);
            CHECK(fixture.play->QueryPlaneHits(ray).HasError());
            CHECK(fixture.play->ReadPlaneObservations().HasError());
            RequireValue(fixture.play->PollEvents());
            CHECK(native.cancels == 1);
            CHECK_FALSE(native.hasContext);
            CHECK(preview.ArmPlaneCapture().HasError());
        } else {
            CHECK(native.scans == 0);
            CHECK(geometry.calls == 0);
            CHECK(fixture.play->QueryPlaneHits({}).HasError());
        }
        fixture.scheduler->Shutdown();
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
        CHECK(fixture.play->ReadPlaneObservations().HasError());
    }

    TEST_CASE("Signed XR Play admits only current consenting eye pointing", "[xr][application][play][gaze]") {
        using enum XR::XRProjectFeaturePolicy;
        using enum Editor::EditorXrEyeGazeState;
        ApplicationFixture fixture{true, 6, Unrequested, Unrequested, Unrequested, false, std::nullopt, OptionalDisable};
        fixture.CreatePlay();
        auto &preview = *fixture.play->EyeGazePreview();
        REQUIRE(preview.ReadEyeGazeState() == Ready);
        CHECK(fixture.script.actions.gaze.action == XR_NULL_HANDLE);

        const bool grant = GENERATE(false, true);
        if (grant) {
            RequireValue(preview.ArmEyeGaze());
            CHECK(preview.ReadEyeGazeState() == Armed);
            preview.RevokeEyeGaze();
            CHECK(preview.ReadEyeGazeState() == Ready);
            RequireValue(preview.ArmEyeGaze());
        }
        auto scene = ActivatePlay(fixture, std::nullopt);
        REQUIRE((fixture.script.actions.gaze.action != XR_NULL_HANDLE) == grant);
        auto cutoff = std::make_unique<AppInputSample>(*fixture.play);
        auto *observed = cutoff.get();
        RequireValue(fixture.scheduler->AddParticipant(std::move(cutoff)));
        RequireValue(fixture.scheduler->Startup());
        const auto sample = [&] {
            RequireValue(fixture.scheduler->RunFrame());
            REQUIRE(observed->result.has_value());
            RequireValue(*observed->result);
            return fixture.play->ReadEyeGaze();
        };
        fixture.script.clock.sample = 100'000'000;
        auto &source = fixture.script.actions.gaze;
        source.sampleTime = fixture.script.clock.sample - 1;
        const auto current = sample();
        CHECK(current.has_value() == grant);
        if (grant) {
            REQUIRE(current);
            CHECK(current->Time().runtimeSample.Nanoseconds() == source.sampleTime);
            CHECK(preview.ReadEyeGazeState() == Tracking);
            source.sampleTime = 1;
            CHECK_FALSE(sample().has_value());
            CHECK(preview.ReadEyeGazeState() == WaitingForSample);
            source.sampleTime = fixture.script.clock.sample;
            REQUIRE(sample());
            if (source.locationReads == 0)
                FAIL("Consented gaze did not reach the native owner.");
            const auto reads = source.locationReads;
            SECTION("focus interruption revokes even when the same event batch restores focus") {
                fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
                fixture.script.events.Push(XR_SESSION_STATE_FOCUSED);
                RequireValue(fixture.play->PollEvents());
                CHECK_FALSE(fixture.play->ReadEyeGaze());
                CHECK_FALSE(sample());
                CHECK_FALSE(sample());
                CHECK(source.locationReads == reads);
            }
            SECTION("platform denial withdraws the grant for the remainder of Play") {
                source.poseResult = XR_ERROR_PERMISSION_INSUFFICIENT;
                CHECK_FALSE(sample());
                CHECK(preview.ReadEyeGazeState() == Unavailable);
                source.poseResult = XR_SUCCESS;
                CHECK_FALSE(sample());
                CHECK(source.locationReads == reads);
            }
            SECTION("consent withdrawn during the native activity query prevents the location read") {
                source.onActivity = [&preview] {
                    preview.RevokeEyeGaze();
                };
                CHECK_FALSE(sample());
                CHECK(preview.ReadEyeGazeState() == Unavailable);
                CHECK(source.locationReads == reads);
                source.onActivity = {};
                CHECK_FALSE(sample());
                CHECK(source.locationReads == reads);
            }
            SECTION("consent withdrawn during location discards the returned pose") {
                source.onLocation = [&preview] {
                    preview.RevokeEyeGaze();
                };
                CHECK_FALSE(sample());
                CHECK(preview.ReadEyeGazeState() == Unavailable);
                CHECK(source.locationReads == reads + 1);
                source.onLocation = {};
                CHECK_FALSE(sample());
                CHECK(source.locationReads == reads + 1);
            }
        } else {
            CHECK(source.locationReads == 0);
        }
        fixture.scheduler->Shutdown();
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
        CHECK_FALSE(fixture.play->ReadEyeGaze());
    }

    TEST_CASE("XR inspection bounds action metadata and ignores action payload values", "[xr][application][inspection]") {
        const XR::XRSessionId session{{RequireValue(XR::XRRuntimeGeneration::Create(1)), {1, 1}}, {1, 1}};
        XR::XRTrackedDeviceCapabilities features;
        features.values[static_cast<std::size_t>(XR::XRTrackedDeviceCapability::GripPose)] = true;
        const std::array devices{XR::XRTrackedDeviceRecord{{session, {1, 3}},
                                                           XR::XRTrackedDeviceRole::LeftController,
                                                           XR::XRTrackedDeviceState::TrackingLost,
                                                           features}};
        std::array<XR::XRTrackedActionRecord, Editor::EditorXrInspectionSnapshot::MaximumDisplayedActions + 1> actions{};
        for (std::size_t index = 0; index < actions.size(); ++index)
            actions[index] = {devices.front().id, static_cast<std::uint32_t>(index), XR::XRTrackedActionKind::Scalar, true, 1.0F, 0};
        const auto revision = RequireValue(XR::XRTrackingSnapshotRevision::Create(1));
        const auto origin = RequireValue(XR::XRWorldOriginRevision::Create(1));
        const XR::XRTrackingSnapshotDescriptor descriptor{.session = session,
                                                          .revision = revision,
                                                          .sequence = 1,
                                                          .sampledAt = RequireValue(XR::XRRuntimeTime::Create(100)),
                                                          .worldOriginRevision = origin,
                                                          .focus = XR::XRTrackingFocusState::Focused,
                                                          .visibility = XR::XRTrackingVisibilityState::Visible,
                                                          .state = XR::XRTrackingState::TrackingLost,
                                                          .limits = {1, 1, static_cast<std::uint32_t>(actions.size())},
                                                          .devices = devices,
                                                          .bindingRevision = 8,
                                                          .actions = actions};
        for (const auto scalar : {0.5F, -0.5F}) {
            actions.front().x = scalar;
            const auto snapshot = RequireValue(XR::XRTrackingSnapshot::Create(descriptor, session, origin));
            Editor::EditorXrInspectionSnapshot report;
            Host::CopyOpenXRInspectionTracking(report, snapshot);
            CHECK(report.privacy == Editor::EditorXrInspectionPrivacy::Redacted);
            CHECK(report.actionCount == actions.size());
            CHECK(report.activeActionCount == actions.size());
            CHECK(report.omittedActionCount == 1);
            CHECK(report.actions.back().bindingIndex == report.actions.size() - 1);
            CHECK(report.actions.front().deviceGeneration == 3);
            CHECK(report.actions.front().active);
        }
    }

    TEST_CASE("Installed XR Play preserves controller input through startup profile confirmation", "[xr][application][controller]") {
        using enum XR::XRProjectFeaturePolicy;
        using enum Input::ActionValueType;
        using Editor::EditorXrInspectionStatus;
        const auto type = GENERATE(Digital, Axis1D, Axis2D);
        const bool aim = type == Axis2D;
        const std::string profilePath = "/interaction_profiles/oculus/touch_controller";
        auto document = OpenXRProduct::Json::parse(OpenXRProduct::InstalledProductFixture::BindingDocument());
        document["profiles"][0]["path"] = profilePath;
        document["actions"][0]["type"] = static_cast<unsigned>(type);
        document["controls"][0]["type"] = static_cast<unsigned>(type);
        document["controls"][0]["path"] = type == Digital  ? "/user/hand/left/input/thumbstick/touch"
                                          : type == Axis1D ? "/user/hand/left/input/trigger/value"
                                                           : "/user/hand/left/input/thumbstick";
        document["poses"].push_back({{"profile", 1},
                                     {"set", 1},
                                     {"role", static_cast<unsigned>(XR::XRTrackedDeviceRole::LeftController)},
                                     {"kind", static_cast<unsigned>(aim ? XR::XRTrackedPoseKind::Aim : XR::XRTrackedPoseKind::Grip)},
                                     {"path", aim ? "/user/hand/left/input/aim/pose" : "/user/hand/left/input/grip/pose"},
                                     {"label", aim ? "Aim" : "Grip"}});
        ApplicationFixture fixture{true, 6, Unrequested, Unrequested, Unrequested, false, document.dump()};
        fixture.script.actions.tracking.enabled = true;
        fixture.script.actions.sample.expectedType = type == Digital  ? XR_ACTION_TYPE_BOOLEAN_INPUT
                                                     : type == Axis1D ? XR_ACTION_TYPE_FLOAT_INPUT
                                                                      : XR_ACTION_TYPE_VECTOR2F_INPUT;
        constexpr Input::PlayerId player = 7;
        constexpr Input::PlayerId other = 2;
        fixture.router.AssignKeyboardMouse(other);
        auto scene = PublishPlay(fixture, player);
        const auto generation = fixture.play->ReadInspection().sessionGeneration;
        REQUIRE(generation != 0);
        auto cutoff = std::make_unique<AppInputSample>(*fixture.play);
        auto *observed = cutoff.get();
        REQUIRE(fixture.scheduler->AddParticipant(std::move(cutoff)).HasValue());
        RequireValue(fixture.scheduler->Startup());

        const auto runCutoff = [&](const char *phase = "active controller cutoff") {
            INFO(phase);
            RequireValue(fixture.scheduler->RunFrame());
            REQUIRE(observed->result.has_value());
            RequireValue(*observed->result);
            CHECK_FALSE(fixture.router.ReadAction(fixture.inputContext, Input::ActionId{"select"}, other).down);
            return fixture.router.ReadAction(fixture.inputContext, Input::ActionId{"select"}, player);
        };
        const auto before = runCutoff();
        CHECK(before.down);
        CHECK(before.x == (type == Digital ? 1.0F : type == Axis1D ? 0.5F : 0.25F));
        CHECK(before.y == (type == Axis2D ? -0.5F : 0.0F));
        const auto initial = fixture.play->ReadInspection();
        REQUIRE(initial.status == EditorXrInspectionStatus::Current);
        REQUIRE(initial.trackingRevision != 0);

        const auto currentActions = fixture.router.Actions();
        const std::vector<Input::ActionDescriptor> original(currentActions.begin(), currentActions.end());
        auto mismatched = original;
        mismatched.front().required = !mismatched.front().required;
        REQUIRE(fixture.router.SetActionMap(mismatched).HasValue());
        CHECK_FALSE(runCutoff().down);
        CHECK_FALSE(runCutoff().down);
        REQUIRE(fixture.router.SetActionMap(original).HasValue());
        CHECK_FALSE(runCutoff().down);
        const auto rebound = runCutoff();
        CHECK(rebound.down);
        CHECK(rebound.x == before.x);
        CHECK(rebound.y == before.y);
        CHECK(fixture.play->ReadInspection().sessionGeneration == generation);
        CHECK(fixture.play->ReadInspection().bindingRevision == initial.bindingRevision);

        fixture.script.actions.currentProfiles[0] = fixture.script.actions.Find(profilePath);
        REQUIRE(fixture.script.actions.currentProfiles[0] != XR_NULL_PATH);
        fixture.script.events.interactionProfileChanged = true;
        RequireValue(fixture.play->PollEvents());
        CHECK(fixture.play->ReadInspection().status == EditorXrInspectionStatus::WaitingForSample);
        const auto confirmed = runCutoff();
        CHECK(confirmed.down);
        CHECK(confirmed.x == before.x);
        CHECK(confirmed.y == before.y);
        const auto refreshed = fixture.play->ReadInspection();
        CHECK(refreshed.sessionGeneration == generation);
        CHECK(refreshed.bindingRevision == initial.bindingRevision);
        CHECK(refreshed.trackingRevision > initial.trackingRevision);
        REQUIRE(refreshed.actionCount == 1);
        const auto controllerGeneration = refreshed.actions.front().deviceGeneration;

        fixture.script.actions.tracking.poseActive = XR_FALSE;
        const auto untracked = runCutoff("pose loss with native scalar still active");
        CHECK_FALSE(untracked.down);
        CHECK(untracked.released);
        const auto trackingLost = fixture.play->ReadInspection();
        CHECK(trackingLost.sessionGeneration == generation);
        CHECK(trackingLost.deviceCount == refreshed.deviceCount);
        CHECK(trackingLost.actionCount == 0);
        CHECK_FALSE(runCutoff().released);
        fixture.script.actions.tracking.poseActive = XR_TRUE;
        const auto trackedAgain = runCutoff();
        CHECK(trackedAgain.pressed);
        CHECK(trackedAgain.x == before.x);
        CHECK(trackedAgain.y == before.y);
        const auto resumed = fixture.play->ReadInspection();
        REQUIRE(resumed.actionCount == 1);
        CHECK(resumed.actions.front().deviceGeneration == controllerGeneration);

        fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
        fixture.script.events.Push(XR_SESSION_STATE_FOCUSED);
        RequireValue(fixture.play->PollEvents());
        CHECK(fixture.play->ReadInspection().trackingRevision == 0);
        const auto neutral = runCutoff("neutral cutoff after focus returns in one event batch");
        CHECK_FALSE(neutral.down);
        CHECK(neutral.released);
        CHECK(runCutoff().pressed);
        CHECK(fixture.play->ReadInspection().sessionGeneration == generation);

        // Both polls can run before Input sees the suspension.
        for (std::uint32_t index = 0; index < Native::OpenXRSessionEvents::MaximumEventsPerPoll; ++index)
            fixture.script.events.Push(XR_SESSION_STATE_FOCUSED);
        RequireValue(fixture.play->PollEvents());
        CHECK(fixture.play->ReadInspection().status == EditorXrInspectionStatus::EventsPending);
        RequireValue(fixture.play->PollEvents());
        CHECK(fixture.play->ReadInspection().status == EditorXrInspectionStatus::WaitingForSample);
        const auto afterBacklog = runCutoff("neutral cutoff after the native backlog drains between frames");
        CHECK_FALSE(afterBacklog.down);
        CHECK(afterBacklog.released);
        CHECK(runCutoff().pressed);

        RequireValue(fixture.play->SetPlayState(Editor::EditorPlayState::Paused));
        RequireValue(fixture.play->SetPlayState(Editor::EditorPlayState::Playing));
        const auto afterPause = runCutoff("neutral cutoff after a Play pause and resume between frames");
        CHECK_FALSE(afterPause.down);
        CHECK(afterPause.released);
        CHECK(runCutoff().pressed);

        fixture.script.actions.currentProfiles[0] = XR_NULL_PATH;
        if (type != Digital) {
            fixture.script.events.interactionProfileChanged = true;
            RequireValue(fixture.play->PollEvents());
        }
        // Digital tests the same confirmed profile change detected by native sampling before notification.
        const auto unplugged = runCutoff("confirmed profile loss");
        const auto retired = fixture.play->ReadInspection();
        CHECK(retired.status == EditorXrInspectionStatus::Stale);
        CHECK(retired.trackingRevision == 0);
        CHECK_FALSE(fixture.play->UsesXrScene());
        CHECK_FALSE(unplugged.down);
        CHECK(unplugged.released);

        RequireValue(fixture.play->PollEvents());
        CHECK(fixture.script.events.exits == 1);
        CHECK(fixture.script.native.liveSessions == 1);
        fixture.script.events.Push(XR_SESSION_STATE_STOPPING);
        fixture.script.events.Push(XR_SESSION_STATE_IDLE);
        RequireValue(fixture.play->PollEvents());
        CHECK(fixture.script.events.ends == 1);
        REQUIRE(fixture.script.native.liveSessions == 0);
        RequireValue(fixture.play->SetPlayState(Editor::EditorPlayState::Idle));
        const auto stopped = runCutoff("neutral cutoff after native stop");
        CHECK_FALSE(stopped.down);
        CHECK_FALSE(stopped.released);
        fixture.script.actions.currentProfiles[0] = fixture.script.actions.Find(profilePath);
        auto replacement = RequireValue(Runtime::RuntimeScene::Create(fixture.definition, {2}));
        auto participant = fixture.play->MakeSceneParticipant();
        auto replacementScene = RequireValue(participant->Prepare(fixture.definition, replacement->View()));
        RequireValue(replacementScene->ValidatePublication());
        replacementScene->Publish();
        RequireValue(fixture.play->SetPlayState(Editor::EditorPlayState::Playing));
        fixture.script.events.session = fixture.script.native.lastSession;
        fixture.script.events.Push(XR_SESSION_STATE_READY);
        fixture.script.events.Push(XR_SESSION_STATE_SYNCHRONIZED);
        fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
        fixture.script.events.Push(XR_SESSION_STATE_FOCUSED);
        RequireValue(fixture.play->PollEvents());
        scene->Shutdown();
        CHECK(fixture.play->UsesXrScene());
        CHECK(fixture.play->ReadInspection().sessionGeneration != generation);
        CHECK(runCutoff("replacement session cutoff").pressed);
        replacementScene->Shutdown();
    }

    TEST_CASE("Installed Play previews vibration through the assigned controller and exact app receipt", "[xr][application][haptics]") {
        using enum XR::XRProjectFeaturePolicy;
        using enum Editor::EditorXrHapticHand;
        using enum Editor::EditorXrHapticPreviewState;
        const std::string profilePath = "/interaction_profiles/oculus/touch_controller";
        auto document = OpenXRProduct::Json::parse(OpenXRProduct::InstalledProductFixture::BindingDocument());
        document["profiles"][0]["path"] = profilePath;
        document["controls"][0]["path"] = "/user/hand/left/input/thumbstick/touch";
        document["poses"].push_back({{"profile", 1},
                                     {"set", 1},
                                     {"role", static_cast<unsigned>(XR::XRTrackedDeviceRole::LeftController)},
                                     {"kind", static_cast<unsigned>(XR::XRTrackedPoseKind::Grip)},
                                     {"path", "/user/hand/left/input/grip/pose"},
                                     {"label", "Grip"}});
        document["haptics"].push_back({{"profile", 1},
                                       {"set", 1},
                                       {"role", static_cast<unsigned>(XR::XRTrackedDeviceRole::LeftController)},
                                       {"path", "/user/hand/left/output/haptic"},
                                       {"label", "Vibration"}});
        ApplicationFixture fixture{true, 6, Unrequested, Unrequested, Unrequested, false, document.dump()};
        fixture.script.actions.tracking.enabled = true;
        constexpr Input::PlayerId player = 7;
        auto scene = PublishPlay(fixture, player);
        fixture.script.actions.currentProfiles[0] = fixture.script.actions.Find(profilePath);
        REQUIRE(fixture.script.actions.currentProfiles[0] != XR_NULL_PATH);
        fixture.script.events.interactionProfileChanged = true;
        RequireValue(fixture.play->PollEvents());
        auto cutoff = std::make_unique<AppInputSample>(*fixture.play);
        auto *observed = cutoff.get();
        RequireValue(fixture.scheduler->AddParticipant(std::move(cutoff)));
        RequireValue(fixture.scheduler->Startup());
        RequireValue(fixture.scheduler->RunFrame());
        REQUIRE(observed->result.has_value());
        RequireValue(*observed->result);

        auto &preview = *fixture.play->HapticPreview();
        auto &native = fixture.script.actions.haptic;
        native.boundCount = 1;
        REQUIRE(preview.CanPreviewHaptic(Left));
        constexpr Editor::EditorXrHapticIntent valid{Left, 0.45F, Duration::FromMilliseconds(125)};
        const auto requireDenial = [&](const Editor::EditorXrHapticIntent intent, const ErrorCodeDescriptor &expected) {
            const auto denied = preview.QueueHapticPreview(intent);
            REQUIRE(denied.HasError());
            CHECK(denied.ErrorValue().domain.Value() == expected.domain.Value());
            CHECK(denied.ErrorValue().code.Value() == expected.code.Value());
        };
        CHECK_FALSE(preview.CanPreviewHaptic(Right));
        requireDenial({Right, valid.amplitude, valid.duration}, XR::XRErrors::OperationUnsupported);
        requireDenial({static_cast<Editor::EditorXrHapticHand>(255), valid.amplitude, valid.duration}, XR::XRErrors::OperationInvalid);
        requireDenial({Left, -0.1F, valid.duration}, XR::XRErrors::OperationInvalid);
        requireDenial({Left, valid.amplitude, Duration::FromMilliseconds(501)}, XR::XRErrors::OperationInvalid);
        const auto request = RequireValue(preview.QueueHapticPreview(valid));
        CHECK(preview.ReadHapticPreview(request) == Queued);
        requireDenial(valid, XR::XRErrors::CapacityExceeded);
        CHECK(native.applies == 0);

        SECTION("submission and cancellation retain the exact receipt") {
            fixture.play->PumpHapticPreview();
            REQUIRE(preview.ReadHapticPreview(request) == Submitted);
            CHECK(native.applies == 1);
            CHECK(native.vibration.duration == valid.duration.ToNanoseconds());
            CHECK(native.vibration.amplitude == valid.amplitude);
            CHECK(native.vibration.frequency == XR_FREQUENCY_UNSPECIFIED);
            preview.CancelHapticPreview({request.value + 1});
            fixture.play->PumpHapticPreview();
            CHECK(preview.ReadHapticPreview(request) == Submitted);
            preview.CancelHapticPreview(request);
            fixture.play->PumpHapticPreview();
            RequireValue(fixture.play->PollEvents());
            CHECK(preview.ReadHapticPreview(request) == Cancelled);
            CHECK(native.stops == 1);
        }
        SECTION("cancelled queue never reaches native output") {
            preview.CancelHapticPreview(request);
            fixture.play->PumpHapticPreview();
            CHECK(preview.ReadHapticPreview(request) == Cancelled);
            CHECK(native.applies == 0);
        }
        SECTION("replaced canonical Input map invalidates a queued output") {
            const auto registered = fixture.router.Actions();
            std::vector<Input::ActionDescriptor> replacement(registered.begin(), registered.end());
            replacement.front().required = !replacement.front().required;
            RequireValue(fixture.router.SetActionMap(replacement));
            requireDenial(valid, XR::XRErrors::IdentityStale);
            fixture.play->PumpHapticPreview();
            CHECK(preview.ReadHapticPreview(request) == Cancelled);
            CHECK(native.applies == 0);
        }
        SECTION("a modal revokes the queued owner and blocks new previews") {
            const auto modal = fixture.router.PushContext(Input::InputContextId{"haptic-modal"}, Input::InputContextKind::ModalRoot);
            CHECK_FALSE(preview.CanPreviewHaptic(Left));
            requireDenial(valid, XR::XRErrors::OperationUnavailable);
            fixture.play->PumpHapticPreview();
            CHECK(preview.ReadHapticPreview(request) == Cancelled);
            CHECK(native.applies == 0);
        }
        SECTION("a modal opened by the native callback retires the submitted vibration") {
            std::optional<Input::InputContextToken> modal;
            native.onApply = [&] {
                modal.emplace(
                    fixture.router.PushContext(Input::InputContextId{"haptic-callback-modal"}, Input::InputContextKind::ModalRoot));
            };
            fixture.play->PumpHapticPreview();
            native.onApply = {};
            CHECK(preview.ReadHapticPreview(request) == Cancelled);
            CHECK(native.applies == 1);
            RequireValue(fixture.play->PollEvents());
            CHECK(native.stops == 1);
        }
        SECTION("runtime refusal reports unsupported without emulation") {
            native.boundCount = 0;
            fixture.play->PumpHapticPreview();
            CHECK(preview.ReadHapticPreview(request) == Unsupported);
            CHECK(native.applies == 0);
        }
        SECTION("focus loss before owner pump cancels the old target") {
            fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
            RequireValue(fixture.play->PollEvents());
            fixture.play->PumpHapticPreview();
            CHECK(preview.ReadHapticPreview(request) == Cancelled);
            CHECK(native.applies == 0);
            requireDenial(valid, XR::XRErrors::OperationUnavailable);
        }
        SECTION("an unread native event backlog stops output before the next input cutoff") {
            fixture.play->PumpHapticPreview();
            REQUIRE(preview.ReadHapticPreview(request) == Submitted);
            for (std::uint32_t index = 0; index < Native::OpenXRSessionEvents::MaximumEventsPerPoll; ++index)
                fixture.script.events.Push(XR_SESSION_STATE_FOCUSED);
            RequireValue(fixture.play->PollEvents());
            CHECK(native.stops == 1);
            RequireValue(fixture.play->PollEvents());
            fixture.play->PumpHapticPreview();
            CHECK(preview.ReadHapticPreview(request) == Cancelled);
            CHECK(native.stops == 1);
        }
        SECTION("shutdown stops submitted output before releasing the native session") {
            fixture.play->PumpHapticPreview();
            REQUIRE(preview.ReadHapticPreview(request) == Submitted);
            REQUIRE(RequireValue(fixture.ClosePlay()));
            CHECK(native.stops == 1);
            CHECK(fixture.script.native.liveSessions == 0);
        }
        SECTION("retirement reports the first event failure and retries outstanding native cleanup") {
            fixture.play->PumpHapticPreview();
            REQUIRE(preview.ReadHapticPreview(request) == Submitted);
            scene->Shutdown();
            fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
            native.stopResult = XR_ERROR_RUNTIME_FAILURE;
            const auto failed = fixture.play->PollEvents();
            native.stopResult = XR_SUCCESS;
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().message.find("stop vibration") != std::string::npos);
            CHECK(native.stops == 1);
            CHECK(fixture.script.native.liveSessions == 1);
            CHECK_FALSE(fixture.play->UsesXrScene());
            RequireValue(fixture.play->PollEvents());
            CHECK(native.stops == 2);
            CHECK(fixture.script.native.liveSessions == 0);
        }
        fixture.scheduler->Shutdown();
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Installed Play exposes exact player-scoped interaction geometry from its committed tracking cutoff",
              "[xr][application][interaction]") {
        using enum XR::XRProjectFeaturePolicy;
        auto document = OpenXRProduct::Json::parse(OpenXRProduct::InstalledProductFixture::BindingDocument());
        document["worldUiPointer"] = {{"action", document["actions"][0]["id"]}, {"maximumDistanceMeters", 6.0F}};
        document["poses"].push_back({{"profile", 1},
                                     {"set", 1},
                                     {"role", static_cast<unsigned>(XR::XRTrackedDeviceRole::LeftController)},
                                     {"kind", static_cast<unsigned>(XR::XRTrackedPoseKind::Aim)},
                                     {"path", "/user/hand/left/input/aim/pose"},
                                     {"label", "Aim"}});
        ApplicationFixture fixture{true, 6, Unrequested, OptionalDisable, Unrequested, false, document.dump()};
        fixture.script.actions.tracking.enabled = true;
        constexpr Input::PlayerId player = 7;
        auto scene = PublishPlay(fixture, player);
        CHECK(fixture.play->ReadInteractionSources(player).HasError());
        auto cutoff = std::make_unique<AppInputSample>(*fixture.play);
        auto *observed = cutoff.get();
        RequireValue(fixture.scheduler->AddParticipant(std::move(cutoff)));
        RequireValue(fixture.scheduler->Startup());
        const auto sample = [&] {
            RequireValue(fixture.scheduler->RunFrame());
            REQUIRE(observed->result.has_value());
            RequireValue(*observed->result);
        };
        sample();
        CHECK(fixture.play->ReadInteractionSources(2).HasError());
        const auto sources = RequireValue(fixture.play->ReadInteractionSources(player));
        const auto source = [&](const XR::XRTrackedDeviceRole role) {
            const auto found = std::ranges::find(sources.Devices(), role, &XR::XRTrackedDeviceRecord::role);
            REQUIRE(found != sources.Devices().end());
            return found->id;
        };
        const auto controller = source(XR::XRTrackedDeviceRole::LeftController);
        const auto hand = source(XR::XRTrackedDeviceRole::LeftHand);
        const auto confidence = XR::XRTrackingConfidence::High;
        const std::array probes{XR::XRInteractionProbe{controller, XR::XRControllerInteractionPose::Aim,
                                                       XR::XRInteractionRayProbe{{0, 0, -1}, 6}, confidence},
                                XR::XRInteractionProbe{hand, XR::XRHandJoint::IndexTip, XR::XRInteractionDirectProbe{}, confidence},
                                XR::XRInteractionProbe{hand, XR::XRHandJoint::IndexTip, XR::XRInteractionProximityProbe{0.3F}, confidence}};
        const auto syncs = fixture.script.actions.syncs;
        const auto handLocates = fixture.script.hands.locates;
        const auto evidence = RequireValue(fixture.play->ReadInteractionCandidates(sources.Scope(), probes));
        CHECK(fixture.script.actions.syncs == syncs);
        CHECK(fixture.script.hands.locates == handLocates);
        REQUIRE(evidence.Candidates().size() == probes.size());
        CHECK(evidence.Cutoff() == sources.Scope().cutoff);
        CHECK(evidence.Candidates()[0].device == controller);
        CHECK(evidence.Candidates()[1].device == hand);
        CHECK(evidence.Candidates()[0].space.kind == XR::XRSpaceKind::World);
        CHECK(evidence.Candidates()[0].space == evidence.Candidates()[1].space);
        CHECK(std::get<Math::Ray>(evidence.Candidates()[0].shape).maximumDistance == 6.0F);
        const auto point = std::get<Math::Vec3>(evidence.Candidates()[1].shape);
        const auto proximity = std::get<Math::BoundingSphere>(evidence.Candidates()[2].shape);
        CHECK(proximity.center == point);
        CHECK(proximity.radius == 0.3F);

        const auto &selection = fixture.router.Actions().front().id;
        const Application::Internal::OpenXRPlayControllerPointerRequest pointer{controller,
                                                                                selection,
                                                                                XR::XRControllerInteractionPose::Aim,
                                                                                {{0, 0, -1}, 6},
                                                                                confidence};
        const auto pressed = RequireValue(fixture.play->ReadControllerPointer(sources.Scope(), pointer));
        CHECK(pressed.selectDown);
        CHECK(pressed.ray.maximumDistance == 6.0F);
        const auto worldUi = fixture.play->ReadWorldUiPointer(player);
        REQUIRE(worldUi);
        CHECK(worldUi->source.device == controller);
        CHECK(worldUi->source.scene == sources.Scope().scene);
        CHECK(worldUi->source.assignmentRevision != 0);
        CHECK(worldUi->sample.selectDown);
        CHECK(fixture.play->UsesXrScene());
        bool foreignScene{};
        bool foreignPointer{};
        {
            const std::jthread worker{[&] {
                foreignScene = fixture.play->UsesXrScene();
                foreignPointer = fixture.play->ReadWorldUiPointer(player).has_value();
            }};
        }
        CHECK_FALSE(foreignScene);
        CHECK_FALSE(foreignPointer);
        CHECK(worldUi->sample.ray.maximumDistance == 6.0F);
        CHECK_FALSE(fixture.play->ReadWorldUiPointer(2));
        const auto wrongDevice =
            fixture.play->ReadControllerPointer(sources.Scope(), {hand, selection, pointer.pose, pointer.ray, pointer.minimumConfidence});
        REQUIRE(wrongDevice.HasError());
        CHECK(wrongDevice.ErrorValue().code.Value() == XR::XRErrors::OperationUnavailable.code.Value());
        const Input::ActionId foreignAction{"unregistered"};
        const auto wrongAction = fixture.play->ReadControllerPointer(sources.Scope(), {controller, foreignAction, pointer.pose, pointer.ray,
                                                                                       pointer.minimumConfidence});
        REQUIRE(wrongAction.HasError());
        CHECK(wrongAction.ErrorValue().code.Value() == XR::XRErrors::OperationUnsupported.code.Value());
        {
            const auto modal = fixture.router.PushContext(Input::InputContextId{"test-modal"}, Input::InputContextKind::ModalRoot);
            CHECK_FALSE(fixture.play->ReadWorldUiPointer(player));
            const auto blocked = fixture.play->ReadControllerPointer(sources.Scope(), pointer);
            REQUIRE(blocked.HasError());
            CHECK(blocked.ErrorValue().code.Value() == XR::XRErrors::OperationUnavailable.code.Value());
        }
        CHECK(fixture.play->ReadControllerPointer(sources.Scope(), pointer).HasError());
        CHECK(fixture.script.actions.syncs == syncs);
        sample();
        CHECK(fixture.play->ReadWorldUiPointer(player));

        fixture.script.actions.sample.boolean = XR_FALSE;
        sample();
        const auto newer = RequireValue(fixture.play->ReadInteractionSources(player));
        CHECK(newer.Scope().cutoff.tracking > sources.Scope().cutoff.tracking);
        CHECK_FALSE(RequireValue(fixture.play->ReadControllerPointer(newer.Scope(), pointer)).selectDown);
        CHECK(fixture.play->ReadControllerPointer(sources.Scope(), pointer).HasError());
        const auto stale = fixture.play->ReadInteractionCandidates(sources.Scope(), probes);
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == XR::XRErrors::TrackingSnapshotStale.code.Value());
        fixture.script.actions.sample.active = XR_FALSE;
        sample();
        const auto inactive = RequireValue(fixture.play->ReadInteractionSources(player));
        CHECK(fixture.play->ReadControllerPointer(inactive.Scope(), pointer).HasError());
        fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
        RequireValue(fixture.play->PollEvents());
        CHECK(fixture.play->ReadInteractionSources(player).HasError());
        CHECK_FALSE(fixture.play->ReadWorldUiPointer(player));
        CHECK(fixture.play->ReadInteractionCandidates(newer.Scope(), probes).HasError());
        CHECK(fixture.play->ReadControllerPointer(inactive.Scope(), pointer).HasError());
        fixture.scheduler->Shutdown();
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
        CHECK(fixture.play->ReadInteractionCandidates(newer.Scope(), probes).HasError());
    }

    TEST_CASE("Play retirement during hand classification rejects the revoked Input cutoff", "[xr][application][play][hands]") {
        using enum XR::XRProjectFeaturePolicy;

        struct RetiringPolicy final : XR::IXRHandGesturePolicy {
            Host::OpenXRPlayHost *play{};
            XR::XRContactPinchGesturePolicy pinch;

            std::optional<std::pair<XR::XRHandGestureKind, XR::XRTrackingConfidence>> Classify(
                const std::span<const XR::XRHandJointRecord> joints) const noexcept override {
                if (play)
                    play->RevokeInput();
                return pinch.Classify(joints);
            }
        } policy;

        ApplicationFixture fixture{true,        5,     Unrequested, OptionalDisable, Unrequested, false, std::nullopt,
                                   Unrequested, false, {},          &policy};
        auto scene = PublishPlay(fixture);
        auto cutoff = std::make_unique<AppInputSample>(*fixture.play);
        auto *observed = cutoff.get();
        RequireValue(fixture.scheduler->AddParticipant(std::move(cutoff)));
        RequireValue(fixture.scheduler->Startup());
        RequireValue(fixture.scheduler->RunFrame());
        REQUIRE(observed->result);
        RequireValue(*observed->result);
        REQUIRE(fixture.play->ReadHandGestures().Candidates().size() == 1);

        policy.play = fixture.play.get();
        RequireValue(fixture.scheduler->RunFrame());
        REQUIRE(observed->result);
        REQUIRE(observed->result->HasError());
        CHECK(observed->result->ErrorValue().code.Value() == XR::XRErrors::TrackingSnapshotStale.code.Value());
        CHECK(fixture.play->ReadHandGestures().Candidates().empty());

        fixture.scheduler->Shutdown();
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Real installed Play samples bounded hands before Input and revokes candidates on focus or invalid evidence",
              "[xr][application][play][hands]") {
        using enum XR::XRProjectFeaturePolicy;
        ApplicationFixture fixture{true, 5, Unrequested, OptionalDisable};
        enum class Outcome {
            FocusLoss,
            Malformed,
            Replacement,
            PartialCreate,
            LocateUnavailable,
            Unsupported
        };
        auto outcome = Outcome::FocusLoss;
        SECTION("partial tracker creation falls back without retaining either native handle") {
            fixture.script.hands.failRight = true;
            outcome = Outcome::PartialCreate;
        }
        SECTION("missing native locate dispatch falls back without leaving trackers or stopping the baseline session") {
            fixture.script.hands.failLocateLookup = true;
            outcome = Outcome::LocateUnavailable;
        }
        SECTION("discovered extension on an unsupported system falls back") {
            fixture.script.census.handsSupported = false;
            outcome = Outcome::Unsupported;
        }
        SECTION("native loss of focus clears candidates without another locate") {}
        SECTION("later malformed joint count revokes") {
            outcome = Outcome::Malformed;
        }
        SECTION("session loss cancels hand evidence and permits a fresh tracker pair") {
            outcome = Outcome::Replacement;
        }
        using Editor::EditorXrInspectionStatus;
        auto scene = PublishPlay(fixture);
        const auto waiting = fixture.play->ReadInspection();
        CHECK(waiting.status == EditorXrInspectionStatus::WaitingForSample);
        CHECK(waiting.focused);
        CHECK(waiting.sessionGeneration > 0);
        CHECK(waiting.capabilityRevision > 0);
        CHECK(waiting.trackingRevision == 0);
        auto cutoff = std::make_unique<AppInputSample>(*fixture.play);
        auto *observed = cutoff.get();
        REQUIRE(fixture.scheduler->AddParticipant(std::move(cutoff)).HasValue());
        REQUIRE(fixture.scheduler->Startup().HasValue());
        REQUIRE(fixture.scheduler->RunFrame().HasValue());
        REQUIRE(observed->result.has_value());
        INFO((observed->result->HasError() ? OpenXRProduct::ErrorText(observed->result->ErrorValue()) : "Input cutoff committed"));
        REQUIRE(observed->result->HasValue());
        const auto handGestures = fixture.play->ReadHandGestures();
        const auto nativeCalls = fixture.script.native.calls.size();
        const auto handLocates = fixture.script.hands.locates;
        const auto inspection = fixture.play->ReadInspection();
        CHECK(fixture.script.native.calls.size() == nativeCalls);
        CHECK(fixture.script.hands.locates == handLocates);
        CHECK(inspection.status == EditorXrInspectionStatus::Current);
        CHECK(inspection.privacy == Editor::EditorXrInspectionPrivacy::Redacted);
        CHECK(inspection.trackingRevision > 0);
        CHECK(inspection.originRevision > 0);
        CHECK(inspection.bindingRevision > 0);
        CHECK(inspection.sampleNanoseconds > 0);
        CHECK(inspection.sessionGeneration == waiting.sessionGeneration);
        CHECK(inspection.spaces[static_cast<std::size_t>(XR::XRSpaceKind::View)] > 0);
        CHECK(inspection.deviceCount > 0);
        CHECK(fixture.script.hands.locateLookups == (outcome == Outcome::Unsupported ? 0 : 1));
        if (outcome != Outcome::Unsupported && outcome != Outcome::PartialCreate && outcome != Outcome::LocateUnavailable) {
            REQUIRE(handGestures.Candidates().size() == 1);
            CHECK(handGestures.Candidates().front().hand == XR::XRHandedness::Left);
            CHECK(handGestures.Candidates().front().kind == XR::XRHandGestureKind::Pinch);
            CHECK(handGestures.Candidates().front().confidence == XR::XRTrackingConfidence::High);
            CHECK(fixture.script.hands.locates == 2);
            CHECK(inspection.hands[0].validity.position[2] == XR::XRHandTrackingHardLimits::JointsPerHand);
            CHECK(inspection.hands[0].validity.confidence[2] == XR::XRHandTrackingHardLimits::JointsPerHand);
            CHECK(inspection.hands[1].validity.position[2] == 0);
            CHECK(inspection.hands[0].generation > 0);
            CHECK(inspection.hands[0].state == XR::XRTrackedDeviceState::Tracked);
            CHECK_FALSE(inspection.hands[1].state.has_value());
            if (outcome == Outcome::FocusLoss) {
                fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);
                REQUIRE(fixture.play->PollEvents().HasValue());
                CHECK(fixture.play->ReadHandGestures().Candidates().empty());
                const auto unfocused = fixture.play->ReadInspection();
                CHECK(unfocused.status == EditorXrInspectionStatus::Unfocused);
                CHECK(unfocused.trackingRevision == 0);
                CHECK(unfocused.hands[0].validity.position[2] == 0);
                REQUIRE(fixture.scheduler->RunFrame().HasValue());
                CHECK(fixture.script.hands.locates == 2);
            } else if (outcome == Outcome::Malformed) {
                fixture.script.hands.malformed = true;
                REQUIRE(fixture.scheduler->RunFrame().HasValue());
                REQUIRE(observed->result->HasValue());
                CHECK(fixture.play->ReadInspection().trackingRevision > inspection.trackingRevision);
                CHECK(fixture.play->ReadInspection().hands[0].validity.position[2] == 0);
                CHECK(fixture.play->ReadHandGestures().Candidates().empty());
            } else {
                fixture.script.events.pollResult = XR_ERROR_SESSION_LOST;
                REQUIRE(fixture.play->PollEvents().HasValue());
                CHECK(fixture.play->ReadHandGestures().Candidates().empty());
                const auto lost = fixture.play->ReadInspection();
                CHECK(lost.status == EditorXrInspectionStatus::Stale);
                CHECK(lost.trackingRevision == 0);
                REQUIRE(fixture.play->PollEvents().HasValue());
                CHECK_FALSE(fixture.script.hands.live[0]);
                CHECK_FALSE(fixture.script.hands.live[1]);
                REQUIRE(fixture.scheduler->RunFrame().HasValue());  // Canonical Input retirement precedes new reservations.
                fixture.script.events.pollResult = XR_SUCCESS;
                auto next = RequireValue(Runtime::RuntimeScene::Create(fixture.definition, {2}));
                auto participant = fixture.play->MakeSceneParticipant();
                auto replacement = RequireValue(participant->Prepare(fixture.definition, next->View()));
                REQUIRE(replacement->ValidatePublication().HasValue());
                replacement->Publish();
                scene->Shutdown();
                CHECK(fixture.play->UsesXrScene());
                const auto nextInspection = fixture.play->ReadInspection();
                CHECK(nextInspection.sessionGeneration > inspection.sessionGeneration);
                CHECK(nextInspection.trackingRevision == 0);
                CHECK(fixture.script.hands.live[0]);
                CHECK(fixture.script.hands.live[1]);
                CHECK(fixture.script.hands.locateLookups == 2);
                CHECK(fixture.play->ReadHandGestures().Candidates().empty());
                replacement->Shutdown();
            }
        } else {
            CHECK(handGestures.Candidates().empty());
            CHECK(fixture.script.hands.locates == 0);
            CHECK(fixture.script.actions.syncs == 1);
            CHECK_FALSE(fixture.script.hands.live[0]);
            CHECK_FALSE(fixture.script.hands.live[1]);
        }
        fixture.scheduler->Shutdown();
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Installed XR Play separates optional hand outages from the exact held controller action",
              "[xr][application][play][hands][fallback]") {
        using enum XR::XRProjectFeaturePolicy;
        enum class HandFault {
            Inactive,
            Malformed,
            Denial,
            UnsupportedDispatch,
            SessionLoss,
            RequiredMalformed
        };
        using enum HandFault;
        const auto fault = GENERATE(Inactive, Malformed, Denial, UnsupportedDispatch, SessionLoss, RequiredMalformed);
        auto document = OpenXRProduct::Json::parse(OpenXRProduct::InstalledProductFixture::BindingDocument());
        document["poses"].push_back({{"profile", 1},
                                     {"set", 1},
                                     {"role", static_cast<unsigned>(XR::XRTrackedDeviceRole::LeftController)},
                                     {"kind", static_cast<unsigned>(XR::XRTrackedPoseKind::Grip)},
                                     {"path", "/user/hand/left/input/grip/pose"},
                                     {"label", "Grip"}});
        ApplicationFixture fixture{true,        6,     Unrequested,    fault == RequiredMalformed ? Required : OptionalDisable,
                                   Unrequested, false, document.dump()};
        fixture.script.actions.tracking.enabled = true;
        constexpr Input::PlayerId player = 7;
        auto scene = PublishPlay(fixture, player);
        auto cutoff = std::make_unique<AppInputSample>(*fixture.play);
        auto *observed = cutoff.get();
        REQUIRE(fixture.scheduler->AddParticipant(std::move(cutoff)).HasValue());
        RequireValue(fixture.scheduler->Startup());
        const auto readAction = [&] {
            return fixture.router.ReadAction(fixture.inputContext, Input::ActionId{"select"}, player);
        };
        const auto runCutoff = [&] {
            RequireValue(fixture.scheduler->RunFrame());
            REQUIRE(observed->result.has_value());
        };
        runCutoff();
        RequireValue(*observed->result);
        REQUIRE(readAction().down);
        REQUIRE(fixture.play->ReadHandGestures().Candidates().size() == 1);
        const auto first = fixture.play->ReadInspection().trackingRevision;

        auto expected = XR::XRCapabilityState::Incompatible;
        auto &hands = fixture.script.hands;
        switch (fault) {
            case Inactive:
                hands.leftActive = false;
                expected = XR::XRCapabilityState::TemporarilyUnavailable;
                break;
            case Malformed:
            case RequiredMalformed:
                hands.malformed = true;
                break;
            case Denial:
                hands.rightLocateResult = XR_ERROR_PERMISSION_INSUFFICIENT;
                expected = XR::XRCapabilityState::Denied;
                break;
            case UnsupportedDispatch:
                hands.rightLocateResult = XR_ERROR_FUNCTION_UNSUPPORTED;
                expected = XR::XRCapabilityState::Unsupported;
                break;
            case SessionLoss:
                hands.rightLocateResult = XR_SESSION_LOSS_PENDING;
                break;
        }
        runCutoff();
        CHECK(fixture.play->ReadHandGestures().Candidates().empty());
        if (fault == SessionLoss || fault == RequiredMalformed) {
            REQUIRE(observed->result->HasError());
            const auto &error = fault == SessionLoss ? XR::XRErrors::RuntimeUnavailable : XR::XRErrors::TrackingSnapshotInvalid;
            CHECK(observed->result->ErrorValue().code.Value() == error.code.Value());
            CHECK_FALSE(readAction().down);
            CHECK(fixture.play->ReadInspection().trackingRevision == 0);
        } else {
            RequireValue(*observed->result);
            CHECK(readAction().down);
            CHECK_FALSE(readAction().pressed);
            const auto after = fixture.play->ReadInspection();
            CHECK(after.trackingRevision > first);
            CHECK(after.hands[0].validity.position[2] == 0);
            const auto hand =
                std::ranges::find(after.capabilities, XR::XRCapability::HandTracking, &Editor::EditorXrInspectionCapability::kind);
            REQUIRE(hand != after.capabilities.end());
            CHECK(hand->observed == expected);
            CHECK(hand->decision == XR::XRFeatureDecisionState::OptionalEnabled);

            const auto priorLocates = hands.locates;
            hands.malformed = false;
            hands.leftActive = true;
            hands.rightLocateResult = XR_SUCCESS;
            runCutoff();
            RequireValue(*observed->result);
            CHECK(readAction().down);
            const bool blocked = fault == Denial || fault == UnsupportedDispatch;
            CHECK(hands.locates == priorLocates + (blocked ? 0 : 2));
            CHECK(fixture.play->ReadHandGestures().Candidates().size() == (blocked ? 0 : 1));
        }
        fixture.scheduler->Shutdown();
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
        CHECK_FALSE(hands.live[0]);
        CHECK_FALSE(hands.live[1]);
    }

    TEST_CASE("Signed Play observes a tracked native reference only for its exact render frame and generation",
              "[xr][application][play][spaces]") {
        const bool fullyTracked = GENERATE(true, false);
        ApplicationFixture fixture{true};
        if (!fullyTracked)
            fixture.script.actions.tracking.flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        auto scene = PublishPlay(fixture);
        const auto session = *fixture.session->RetainedSession();
        fixture.script.clock.sample = 1;
        CHECK(fixture.play->ReadTrackedReference(session, 1).HasError());
        REQUIRE(RequireValue(fixture.play->PrepareRenderFrame(1)));
        const auto observed = fixture.play->ReadTrackedReference(session, 1);
        if (fullyTracked) {
            REQUIRE(observed.HasValue());
            CHECK(observed.Value().frame.session == session);
            CHECK(observed.Value().referenceToWorld.Source() == RequireValue(fixture.play->ReadReferences(session)).local);
            CHECK(observed.Value().referenceToWorld.Target() == RequireValue(fixture.session->WorldSpace(session)));
            CHECK(observed.Value().referenceToWorld.Purpose() == XR::XRPosePurpose::PresentationPrediction);
            CHECK(observed.Value().referenceToWorld.Time().renderPrediction.has_value());
        } else {
            REQUIRE(observed.HasError());
            CHECK(observed.ErrorValue().code.Value() == XR::XRErrors::OperationUnavailable.code.Value());
        }
        CHECK(fixture.play->ReadTrackedReference(session, 2).HasError());

        XrEventDataReferenceSpaceChangePending change{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
        change.session = fixture.script.events.session;
        change.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        change.changeTime = fixture.script.clock.sample + 100;
        fixture.script.events.referenceChange = change;
        REQUIRE(fixture.play->PollEvents().HasValue());
        CHECK(fixture.play->ReadTrackedReference(session, 1).HasError());
        CHECK_FALSE(fixture.play->UsesXrScene());
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Real installed Play recenter retires reference identities and replacement rereads Stage bounds",
              "[xr][application][play][spaces]") {
        using enum XR::XRProjectFeaturePolicy;
        ApplicationFixture fixture{true, 6, Unrequested, Unrequested, Required};
        fixture.script.census.spaces = 3;
        auto first = PublishPlay(fixture);
        const auto session = *fixture.session->RetainedSession();
        const auto references = RequireValue(fixture.play->ReadReferences(session));
        REQUIRE(references.stage);
        REQUIRE(references.stageBounds);
        CHECK(references.stageBounds->widthMeters == 3.0F);
        CHECK(fixture.script.actions.tracking.boundsReads == 1);
        unsigned admissionCalls = 0;
        fixture.onCheckAdmission = [&] {
            ++admissionCalls;
        };
        CHECK(fixture.play->ReadReferences(session).HasValue());
        CHECK(fixture.script.actions.tracking.boundsReads == 1);
        CHECK(admissionCalls == 0);
        fixture.onCheckAdmission = {};

        XrEventDataReferenceSpaceChangePending change{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
        change.session = fixture.script.events.session;
        change.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        change.changeTime = fixture.script.clock.sample + 100;
        fixture.script.events.referenceChange = change;
        REQUIRE(fixture.play->PollEvents().HasValue());
        CHECK_FALSE(fixture.play->UsesXrScene());
        const auto stale = fixture.play->ReadReferences(session);
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == XR::XRErrors::IdentityStale.code.Value());
        const auto observed = fixture.play->ReadReferenceChange(session);
        REQUIRE(observed);
        CHECK(observed->kind == XR::XRSpaceKind::Stage);
        CHECK(observed->effectiveTime.Nanoseconds() == change.changeTime);
        CHECK_FALSE(observed->relativePoseKnown);
        CHECK(fixture.script.events.running);

        SECTION("a later notice extends replacement while old native work retires") {
            change.changeTime += 100;
            fixture.script.events.referenceChange = change;
            REQUIRE(fixture.play->PollEvents().HasValue());
            const auto latest = fixture.play->ReadReferenceChange(session);
            REQUIRE(latest);
            CHECK(latest->effectiveTime.Nanoseconds() == change.changeTime);
            REQUIRE(fixture.play->PollEvents().HasValue());
            CHECK(fixture.script.events.exits == 1);
            fixture.script.events.Push(XR_SESSION_STATE_STOPPING);
            fixture.script.events.Push(XR_SESSION_STATE_IDLE);
            REQUIRE(fixture.play->PollEvents().HasValue());
            CHECK(fixture.script.events.ends == 1);
        }
        SECTION("native session loss still fences replacement in the retained instance clock") {
            fixture.script.events.loseEvents = true;
            REQUIRE(fixture.play->PollEvents().HasValue());
        }
        CHECK(fixture.script.actions.tracking.spaces == 0);
        CHECK_FALSE(fixture.session->RetainedSession());
        CHECK(fixture.script.native.liveSessions == 0);
        REQUIRE(fixture.play->ReadReferenceChange(session));
        first->Shutdown();
        auto cutoff = std::make_unique<AppInputSample>(*fixture.play);
        REQUIRE(fixture.scheduler->AddParticipant(std::move(cutoff)).HasValue());
        REQUIRE(fixture.scheduler->Startup().HasValue());
        REQUIRE(fixture.scheduler->RunFrame().HasValue());  // Real neutral Input commit before replacement reservations.
        fixture.script.actions.tracking.bounds.width = 5.0F;
        auto next = RequireValue(Runtime::RuntimeScene::Create(fixture.definition, {2}));
        auto participant = fixture.play->MakeSceneParticipant();
        const auto nativeStarts = std::ranges::count(fixture.script.native.calls, "session");
        const auto premature = participant->Prepare(fixture.definition, next->View());
        REQUIRE(premature.HasError());
        CHECK(premature.ErrorValue().code.Value() == XR::XRErrors::OperationUnavailable.code.Value());
        CHECK(std::ranges::count(fixture.script.native.calls, "session") == nativeStarts);
        CHECK(fixture.script.actions.tracking.boundsReads == 1);
        CHECK(fixture.script.native.liveSessions == 0);
        fixture.script.clock.sample = change.changeTime;
        auto replacement = RequireValue(participant->Prepare(fixture.definition, next->View()));
        REQUIRE(replacement->ValidatePublication().HasValue());
        replacement->Publish();
        const auto current = *fixture.session->RetainedSession();
        CHECK(current != session);
        const auto replaced = RequireValue(fixture.play->ReadReferences(current));
        REQUIRE(replaced.stage);
        REQUIRE(replaced.stageBounds);
        CHECK(replaced.stage->id != references.stage->id);
        CHECK(replaced.stageBounds->widthMeters == 5.0F);
        CHECK(references.stageBounds->widthMeters == 3.0F);
        CHECK(fixture.script.actions.tracking.boundsReads == 2);
        CHECK(fixture.play->ReadReferences(session).HasError());
        CHECK_FALSE(fixture.play->ReadReferenceChange(session));
        replacement->Shutdown();
        fixture.scheduler->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Real installed Play does not create or query a disabled Stage", "[xr][application][play][spaces]") {
        using enum XR::XRProjectFeaturePolicy;
        ApplicationFixture fixture{true, 6, Unrequested, Unrequested, Disabled};
        fixture.script.census.spaces = 3;
        auto candidate = PublishPlay(fixture);
        const auto session = *fixture.session->RetainedSession();
        const auto references = RequireValue(fixture.play->ReadReferences(session));
        CHECK_FALSE(references.stage);
        CHECK_FALSE(references.stageBounds);
        CHECK(fixture.script.actions.tracking.boundsReads == 0);
        XrEventDataReferenceSpaceChangePending change{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
        change.session = fixture.script.events.session;
        change.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        change.changeTime = fixture.script.clock.sample + 100;
        fixture.script.events.referenceChange = change;
        REQUIRE(fixture.play->PollEvents().HasValue());
        CHECK(fixture.play->UsesXrScene());
        CHECK_FALSE(fixture.play->ReadReferenceChange(session));
        candidate->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Real installed Play rejects an admitted Stage beyond its reference-space budget", "[xr][application][play][spaces]") {
        using enum XR::XRProjectFeaturePolicy;
        ApplicationFixture fixture{true, 5, Unrequested, Unrequested, Required};
        fixture.script.census.spaces = 3;
        fixture.CreatePlay();
        REQUIRE(fixture.play->SetPlayState(Editor::EditorPlayState::Playing).HasValue());
        REQUIRE(fixture.play->AttachInput(fixture.InputPort(), fixture.source.Port()).HasValue());
        auto participant = fixture.play->MakeSceneParticipant();
        const auto result = participant->Prepare(fixture.definition, fixture.scene->View());
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == XR::XRErrors::CapacityExceeded.code.Value());
        CHECK(fixture.script.actions.tracking.references.empty());
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Real installed Play degrades optional depth and rejects required depth on a color-only renderer",
              "[xr][application][play][features]") {
        auto depth = XR::XRProjectFeaturePolicy::OptionalProjection;
        SECTION("optional depth uses the admitted color projection") {}
        SECTION("required depth rejects the candidate before publication") {
            depth = XR::XRProjectFeaturePolicy::Required;
        }
        ApplicationFixture fixture{true, 5, depth};
        fixture.CreatePlay();
        REQUIRE(fixture.play->SetPlayState(Editor::EditorPlayState::Playing).HasValue());
        REQUIRE(fixture.play->AttachInput(fixture.InputPort(), fixture.source.Port()).HasValue());
        auto participant = fixture.play->MakeSceneParticipant();
        auto candidate = participant->Prepare(fixture.definition, fixture.scene->View());
        CHECK(std::ranges::find(fixture.script.native.enabledExtensions, "XR_KHR_composition_layer_depth") ==
              fixture.script.native.enabledExtensions.end());
        if (depth == XR::XRProjectFeaturePolicy::Required) {
            REQUIRE(candidate.HasError());
            CHECK(candidate.ErrorValue().code.Value() == XR::XRErrors::OperationUnsupported.code.Value());
            CHECK_FALSE(fixture.play->UsesXrScene());
        } else {
            auto scene = RequireValue(std::move(candidate));
            REQUIRE(scene->ValidatePublication().HasValue());
            scene->Publish();
            CHECK(fixture.play->UsesXrScene());
            scene->Shutdown();
        }
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Selected Play cold prepares the staged Vulkan motion pipeline without activating synthesis",
              "[xr][application][play][reprojection]") {
        using enum XR::XRProjectFeaturePolicy;
        constexpr std::array<std::int64_t, 3> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_D16_UNORM};
        bool stageBundle = true;
        bool rejectPipeline = false;
        bool request = true;
        bool advertise = true;
        bool refuseExtension = false;
        bool invalidExtent = false;
        SECTION("matching advertised tuple") {}
        SECTION("packaged motion pair absent") {
            stageBundle = false;
        }
        SECTION("selected native pipeline creation failure") {
            rejectPipeline = true;
        }
        SECTION("runtime advertisement is ignored without explicit project policy") {
            request = false;
        }
        SECTION("runtime does not advertise the optional route") {
            advertise = false;
        }
        SECTION("runtime refuses the optional route during instance creation") {
            refuseExtension = true;
        }
        SECTION("runtime supplies an out-of-bounds auxiliary extent") {
            invalidExtent = true;
        }
        ApplicationFixture fixture{true, 5, Unrequested, Unrequested, Unrequested, request};
        if (advertise != fixture.script.native.synthesisExtension) {
            fixture.script.native.synthesisExtension = advertise;
            if (advertise)
                ++fixture.script.native.extensionCount;
            else
                --fixture.script.native.extensionCount;
        }
        fixture.script.native.refuseReprojectionInstance = refuseExtension;
        fixture.script.census.invalidReprojectionExtent = invalidExtent;
        fixture.script.census.formats = formats.size();
        fixture.script.census.formatPublication = formats;
        if (stageBundle) {
            const auto motion = ApplicationVulkanFixture::CookMotion();
            fixture.viewport->StageXrMotionProgram(RequireValue(
                Editor::LoadEditorVulkanMultiviewMotionCook(motion.compiled.payload,
                                                            RequireValue(Render::EncodeReflectedShaderArtifact(motion.reflected)))));
        }
        auto &driver = fixture.vulkan->shader.state;
        const auto pipelines = driver.pipelines;
        const auto destroyedShaders = driver.destroyedShaders;
        if (rejectPipeline)
            driver.pipelineResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        auto scene = PublishPlay(fixture);
        const bool enabled = request && advertise && !refuseExtension;
        const bool warmed = enabled && !invalidExtent && stageBundle;
        CHECK(driver.pipelines == pipelines + (warmed ? 1U : 0U));
        CHECK((std::ranges::find(fixture.script.native.enabledExtensions, XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME) !=
               fixture.script.native.enabledExtensions.end()) == enabled);
        CHECK((fixture.script.census.synthesisEyeQueries > 0) == enabled);
        CHECK(std::ranges::count(fixture.script.native.calls, "instance") == (refuseExtension ? 2 : 1));
        if (warmed) {
            CHECK(driver.colorFormat == VK_FORMAT_R32G32B32A32_SFLOAT);
            CHECK(driver.depthFormat == VK_FORMAT_D16_UNORM);
            CHECK(driver.depthTestEnabled);
            CHECK(driver.depthWriteEnabled);
        }
        if (rejectPipeline)
            CHECK(driver.destroyedShaders >= destroyedShaders + 2);
        driver.pipelineResult = VK_SUCCESS;
        fixture.script.clock.sample = 1;
        REQUIRE(RequireValue(fixture.play->PrepareRenderFrame(1)));
        auto frame = RequireValue(fixture.frontend->BeginFrame({1, {8, 8}}));
        RequireValue(fixture.play->ExecuteRenderFrame(frame, {}));
        RequireValue(frame.Present());
        RequireValue(fixture.play->FinishGui(true));
        fixture.vulkan->native.commandState.state.Complete();
        RequireValue(fixture.play->ProgressRenderTail());
        CHECK(fixture.script.frames.ends == 1);
        CHECK(fixture.script.frames.layers == 1);
        CHECK(std::ranges::none_of(fixture.script.frames.ext, [](const auto &view) {
            return view.type == XR_TYPE_FRAME_SYNTHESIS_INFO_EXT;
        }));
        CHECK(std::ranges::none_of(fixture.script.frames.fb, [](const auto &view) {
            return view.type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB;
        }));
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Selected Play writes exact auxiliary images while projection retains ordinary compositor admission",
              "[xr][application][play][reprojection][depth]") {
        using enum XR::XRProjectFeaturePolicy;
        using namespace Render;
        bool delayAuxiliary = false;
        bool rejectPairedMask = false;
        bool useDepth16 = false;
        SECTION("asymmetric writer with an adjacent predecessor") {}
        SECTION("zero-time auxiliary wait defers release but leaves ordinary projection usable") {
            delayAuxiliary = true;
        }
        SECTION("paired mask allocation failure preserves the motion writer and ordinary projection") {
            rejectPairedMask = true;
        }
        SECTION("D16 eye-view admission falls back to ordinary projection without a paired mask") {
            useDepth16 = true;
        }
        ApplicationFixture fixture{true, 5, Unrequested, Unrequested, Unrequested, true};
        const std::array<std::int64_t, 3> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                  useDepth16 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT};
        fixture.script.census.formats = formats.size();
        fixture.script.census.formatPublication = formats;
        const auto motion = ApplicationVulkanFixture::CookMotion();
        fixture.viewport->StageXrMotionProgram(RequireValue(
            Editor::LoadEditorVulkanMultiviewMotionCook(motion.compiled.payload,
                                                        RequireValue(Render::EncodeReflectedShaderArtifact(motion.reflected)))));
        const auto coverage = ApplicationVulkanFixture::CookCoverage();
        fixture.viewport->StageXrCoverageProgram(RequireValue(
            Editor::LoadEditorVulkanMultiviewCoverageCook(coverage.compiled.payload,
                                                          RequireValue(Render::EncodeReflectedShaderArtifact(coverage.reflected)))));
        auto &vulkan = fixture.vulkan->native;
        if (rejectPairedMask)
            vulkan.resourceState.state.rejectedImage =
                VulkanResourceTest::NativeResourceState::RejectedImage{VK_FORMAT_R32_SFLOAT, {800, 512}};
        auto scene = PublishPlay(fixture);
        fixture.script.clock.sample = 1;
        const auto previous = [&] {
            return Editor::EditorVulkanXrViewportAccess::PreviousDepth(*fixture.viewport);
        };
        const auto submit = [&](std::uint64_t number) {
            auto frame = RequireValue(fixture.frontend->BeginFrame({number, {8, 8}}));
            REQUIRE(fixture.play->ExecuteRenderFrame(frame, {}).HasValue());
            REQUIRE(frame.Present().HasValue());
            REQUIRE(fixture.play->FinishGui(true).HasValue());
        };
        const auto complete = [&] {
            const auto ended = fixture.script.frames.ends;
            vulkan.commandState.state.Complete();
            REQUIRE(fixture.play->ProgressRenderTail().HasValue());
            if (fixture.script.frames.ends != ended)
                fixture.script.frames.prediction += 1'000'000;
        };

        REQUIRE(fixture.play->PrepareRenderFrame(1).Value());
        CHECK_FALSE(previous());
        submit(1);
        REQUIRE(fixture.play->ProgressRenderTail().HasValue());
        CHECK(fixture.script.frames.ends == 0);
        CHECK_FALSE(previous());
        REQUIRE(vulkan.encoded.renderingCalls.size() >= 2);
        const auto depthImage = [&](const std::size_t offset) {
            const auto barriers = std::span{vulkan.encoded.imageBarriers}.subspan(offset);
            const auto selected = std::ranges::find_if(barriers, [](const auto &barrier) {
                return barrier.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
                       (barrier.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
            });
            REQUIRE(selected != barriers.end());
            return selected->image;
        };
        const auto firstDepth = depthImage(0);
        const auto priorCalls = vulkan.encoded.renderingCalls.size();
        const auto priorBarriers = vulkan.encoded.imageBarriers.size();
        complete();
        REQUIRE(fixture.script.swapchain.creates == 3);
        REQUIRE(fixture.script.swapchain.releases == 1);
        if (delayAuxiliary)
            fixture.script.swapchain.timeoutSwapchain = OpenXR::NativeHandle<XrSwapchain>(3);
        REQUIRE(fixture.play->PrepareRenderFrame(2).Value());
        const auto first = previous();
        REQUIRE(first);
        CHECK(first->frame.projectionSequence == 1);
        CHECK(first->eyes.front() != first->eyes.back());
        CHECK(first->state.layout == RenderGraphTextureLayout::DepthStencilAttachment);
        CHECK(RequireValue(fixture.frontend->ResourceState(first->texture)) == RenderResourceState::Ready);

        auto builder = RequireValue(RenderGraphBuilder::Create());
        const auto sample = RequireValue(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto retained = RequireValue(builder.ImportTexture(first->texture, RenderGraphResourceClass::Persistent));
        REQUIRE(builder.AddUsage({sample, retained, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled}).HasValue());
        const auto authored = RequireValue(builder.Finalize());
        const auto scheduled = RequireValue(CompileRenderGraph(authored));
        const std::array queue{RenderQueueAssignment{RenderQueueRole::Graphics, first->state.queue}};
        const std::array initial{RenderGraphImportedState{retained, first->state}};
        const auto transition = RequireValue(SynthesizeRenderGraphSynchronization(authored, scheduled, queue, initial));
        REQUIRE(transition.Transitions().size() == 1);
        CHECK(transition.Transitions().front().oldState == first->state);
        CHECK(transition.Transitions().front().newState.layout == RenderGraphTextureLayout::ShaderReadOnly);

        submit(2);
        const auto commands = std::span{vulkan.encoded.renderingCalls}.subspan(priorCalls);
        const bool wroteAuxiliary = !delayAuxiliary && !useDepth16;
        const bool pairedCoverage = wroteAuxiliary && !rejectPairedMask;
        REQUIRE(commands.size() == (pairedCoverage ? 6 : 4));
        CHECK(depthImage(priorBarriers) != firstDepth);
        CHECK_FALSE(previous());
        if (wroteAuxiliary) {
            CHECK(commands[2].extent.width == 640);
            CHECK(commands[3].extent.width == 800);
            CHECK(commands[2].extent.height == 512);
            CHECK(commands[3].extent.height == 512);
            CHECK(commands[2].depth);
            CHECK(commands[3].depth);
            if (pairedCoverage) {
                for (std::size_t eye = 0; eye < 2; ++eye) {
                    const auto &motionRaster = commands[2 + eye];
                    const auto &mask = commands[4 + eye];
                    CHECK(mask.extent.width == motionRaster.extent.width);
                    CHECK(mask.extent.height == motionRaster.extent.height);
                    CHECK(mask.colorView != motionRaster.colorView);
                    CHECK(mask.depth);
                }
            }
            CHECK(fixture.vulkan->port.motionUniformWrites == (pairedCoverage ? 2U : 1U));
        } else {
            for (std::size_t eye = 0; eye < 2; ++eye) {
                CHECK(commands[2 + eye].extent.width == commands[eye].extent.width);
                CHECK(commands[2 + eye].extent.height == commands[eye].extent.height);
            }
            CHECK(fixture.vulkan->port.motionUniformWrites == 1);
        }
        REQUIRE(fixture.play->ProgressRenderTail().HasValue());
        CHECK(fixture.script.frames.ends == 1);
        if (!delayAuxiliary)
            CHECK(fixture.script.swapchain.releases == (useDepth16 ? 3 : 1));
        complete();
        if (delayAuxiliary) {
            CHECK(fixture.script.frames.ends == 1);
            fixture.script.swapchain.timeoutSwapchain = XR_NULL_HANDLE;
            REQUIRE(fixture.play->ProgressRenderTail().HasValue());
            fixture.script.frames.prediction += 1'000'000;
            CHECK(fixture.vulkan->port.motionUniformWrites == 1);
        }
        CHECK(fixture.script.swapchain.releases == 4);
        CHECK(fixture.script.frames.ends == 2);
        CHECK(fixture.script.frames.layers == 1);
        CHECK(std::ranges::all_of(fixture.script.frames.encoded, [](const auto &view) {
            return view.next == nullptr;
        }));
        REQUIRE(fixture.play->PrepareRenderFrame(3).Value());
        const auto second = previous();
        REQUIRE(second);
        CHECK(second->texture != first->texture);
        CHECK(second->frame.projectionSequence == 2);
        REQUIRE(fixture.play->FinishGui(false).HasValue());
        complete();
        REQUIRE(fixture.play->PrepareRenderFrame(4).Value());
        CHECK_FALSE(previous());
        REQUIRE(fixture.play->FinishGui(false).HasValue());
        complete();
        CHECK(fixture.script.frames.ends == 4);
        CHECK(fixture.script.frames.layers == 0);
        CHECK(vulkan.commandState.state.waitCalls == 0);
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    namespace {
        /** @brief Scripted external allocation accepted by the actual selected Vulkan registry and command recorder. */
        struct ViewportWriterImage final : RenderNative::IVulkanExternalTextureSource {
            RenderNative::VulkanEditorDevice device;
            std::uint64_t incarnation{};
            std::uintptr_t id{};
            Render::RenderTextureDescriptor allocation;
            bool depth{};

            Result<void> Validate() const override {
                return Result<void>::Success();
            }

            std::uint64_t DeviceIncarnation() const noexcept override {
                return incarnation;
            }

            const Render::RenderTextureDescriptor &Descriptor() const noexcept override {
                return allocation;
            }

            VkDevice NativeDevice() const noexcept override {
                return device.device;
            }

            VkImage NativeImage() const noexcept override {
                return OpenXR::NativeHandle<VkImage>(id);
            }

            std::uint32_t NativeQueueFamily() const noexcept override {
                return device.graphicsFamily;
            }

            void RecordNativeRetirement(const Render::RenderGraphLogicalState &, bool) const noexcept override {}

            Render::RenderGraphLogicalState AcquiredState() const noexcept override {
                using namespace Render;
                return {RenderGraphSynchronizationAccess::ReadWrite,
                        depth ? RenderGraphSynchronizationOperation::DepthStencilAttachment
                              : RenderGraphSynchronizationOperation::ColorAttachment,
                        RenderGraphPipelineScope::Graphics,
                        depth ? RenderGraphTextureLayout::DepthStencilAttachment : RenderGraphTextureLayout::ColorAttachment,
                        device.graphicsIdentity};
            }

            Render::RenderGraphLogicalState RequiredFinalState() const noexcept override {
                auto state = AcquiredState();
                state.scope = Render::RenderGraphPipelineScope::External;
                return state;
            }
        };

        /** @brief Owns fixture handles for one exact external acquisition before it is queued. */
        struct ViewportWriterImport final {
            Render::RenderTextureHandle texture;
            std::array<Render::RenderTextureViewHandle, 2> views;
        };

        ViewportWriterImport ImportWriterImage(Render::RenderFrontend &frontend, std::shared_ptr<const ViewportWriterImage> source) {
            using namespace Render;
            const auto texture = RequireValue(RenderNative::RenderExternalTextureAccess::Import(frontend, source));
            ViewportWriterImport imported{texture};
            for (std::uint32_t eye = 0; eye < imported.views.size(); ++eye) {
                const auto view = RequireValue(
                    frontend.CreateTextureView({.texture = texture,
                                                .format = source->allocation.format,
                                                .aspect = source->depth ? RenderTextureAspect::Depth : RenderTextureAspect::Color,
                                                .baseLayer = eye,
                                                .layerCount = 1}));
                RequireValue(frontend.ProcessResourceRequests());
                RequireValue(frontend.ResourceOperationResult(view.operation));
                imported.views[eye] = view.handle;
            }
            return imported;
        }
    }  // namespace

    TEST_CASE("Selected Play stages the loaded paired reducer before native swapchains and preserves projection fallback",
              "[xr][application][play][reprojection][preactivation]") {
        using namespace Render;
        using enum XR::XRProjectFeaturePolicy;
        using Reduction = std::shared_ptr<IResidentStereoCoverageReducer>;

        struct ScriptedColdKernel final : IResidentStereoCoverageReducer {};

        enum class ColdOutcome {
            Prepared,
            MissingReducer,
            RejectedDepth
        };
        const auto outcome = GENERATE(ColdOutcome::Prepared, ColdOutcome::MissingReducer, ColdOutcome::RejectedDepth);
        const bool reject = outcome == ColdOutcome::MissingReducer;
        const bool rejectDepth = outcome == ColdOutcome::RejectedDepth;
        ApplicationFixture fixture{true, 5, Unrequested, Unrequested, Unrequested, true};
        constexpr std::array<std::int64_t, 3> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT};
        fixture.script.census.formats = formats.size();
        fixture.script.census.formatPublication = formats;
        if (rejectDepth)
            fixture.script.swapchain.rejectCreateFormat = VK_FORMAT_D32_SFLOAT;
        const auto motion = ApplicationVulkanFixture::CookMotion();
        const auto coverage = ApplicationVulkanFixture::CookCoverage();
        fixture.viewport->StageXrMotionProgram(
            RequireValue(Editor::LoadEditorVulkanMultiviewMotionCook(motion.compiled.payload,
                                                                     RequireValue(EncodeReflectedShaderArtifact(motion.reflected)))));
        fixture.viewport->StageXrCoverageProgram(
            RequireValue(Editor::LoadEditorVulkanMultiviewCoverageCook(coverage.compiled.payload,
                                                                       RequireValue(EncodeReflectedShaderArtifact(coverage.reflected)))));
        // The injected adapter exercises cold ownership and ordering only. Native shader/dispatch qualification is separate.
        fixture.viewport->StageXrCoverageReductionProgram({{1}});
        std::size_t coldPreparations{};
        std::weak_ptr<IResidentStereoCoverageReducer> reducer;
        fixture.vulkan->port.prepareColdReduction = [&](const auto &request) -> Result<Reduction> {
            ++coldPreparations;
            CHECK(fixture.script.swapchain.creates == 0);
            CHECK(request.eyes.front() != request.eyes.back());
            CHECK(request.result != request.reset);
            if (reject)
                return Result<Reduction>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
            auto compiled = std::make_shared<ScriptedColdKernel>();
            reducer = compiled;
            return Result<Reduction>::Success(std::move(compiled));
        };
        auto scene = PublishPlay(fixture);
        REQUIRE(coldPreparations == 1);
        CHECK(reducer.expired() == reject);
        REQUIRE(fixture.script.swapchain.creates == (rejectDepth ? 2 : 3));
        CHECK(fixture.script.swapchain.destroys == (rejectDepth ? 1 : 0));
        CHECK(fixture.script.swapchain.created.format == (reject ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM));
        fixture.script.clock.sample = 1;
        const auto submit = [&](const std::uint64_t sequence) {
            CAPTURE(sequence);
            REQUIRE(RequireValue(fixture.play->PrepareRenderFrame(sequence)));
            auto frame = RequireValue(fixture.frontend->BeginFrame({sequence, {8, 8}}));
            RequireValue(fixture.play->ExecuteRenderFrame(frame, {}));
            RequireValue(frame.Present());
            RequireValue(fixture.play->FinishGui(true));
            fixture.vulkan->native.commandState.state.Complete();
            RequireValue(fixture.play->ProgressRenderTail());
            fixture.script.frames.prediction += 1'000'000;
        };
        submit(1);
        CHECK(fixture.script.frames.ends == 1);
        CHECK(fixture.script.frames.layers == 1);
        CHECK(std::ranges::none_of(fixture.script.frames.ext, [](const auto &view) {
            return view.type == XR_TYPE_FRAME_SYNTHESIS_INFO_EXT;
        }));
        if (outcome == ColdOutcome::Prepared) {
            // The actual coordinated path must not block ordinary native ends while its staged depth owner owes a wait.
            fixture.script.swapchain.timeoutSwapchain = OpenXR::NativeHandle<XrSwapchain>(2);
            submit(2);
            CHECK(fixture.script.frames.ends == 2);
            CHECK(fixture.script.frames.layers == 1);
            CHECK(fixture.script.swapchain.releases == 3);
            const auto acquires = std::ranges::count(fixture.script.swapchain.calls, "acquire");
            CHECK(acquires == 4);
            submit(3);
            CHECK(fixture.script.frames.ends == 3);
            CHECK(fixture.script.frames.layers == 1);
            CHECK(std::ranges::count(fixture.script.swapchain.calls, "acquire") == acquires + 1);
            CHECK(fixture.script.swapchain.releases == 4);
            fixture.script.swapchain.timeoutSwapchain = XR_NULL_HANDLE;
            RequireValue(fixture.play->ProgressRenderTail());
            CHECK(fixture.script.swapchain.releases == 5);
        }
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
        CHECK(fixture.script.swapchain.destroys == fixture.script.swapchain.creates);
        CHECK(reducer.expired());
    }

    TEST_CASE("Selected Vulkan viewport records asymmetric motion depth only after completed projection history",
              "[xr][application][play][reprojection][renderer]") {
        using namespace Render;
        using Viewport = Editor::EditorVulkanXrViewportAccess;
        ApplicationFixture fixture{true};
        auto &frontend = *fixture.frontend;
        auto &viewport = *fixture.viewport;
        auto &vulkan = fixture.vulkan->native;
        const auto device = RequireValue(Viewport::Device(viewport));
        const auto &support = frontend.Capabilities().support;
        const auto cook = ApplicationVulkanFixture::CookMotion();
        enum class CoverageMode {
            Absent,
            Fits,
            Probe,
            SceneGrew
        };
        const auto coverageMode = GENERATE(CoverageMode::Absent, CoverageMode::Fits, CoverageMode::Probe, CoverageMode::SceneGrew);
        const bool withCoverage = coverageMode != CoverageMode::Absent;
        const bool expectProbe = coverageMode == CoverageMode::Probe;
        const bool expectCoverage = coverageMode == CoverageMode::Fits || expectProbe;
        const std::array probe{MultiviewRenderRectangle{1, 2, 3, 4}, MultiviewRenderRectangle{2, 1, 4, 2}};
        viewport.StageXrMotionProgram(
            RequireValue(Editor::LoadEditorVulkanMultiviewMotionCook(cook.compiled.payload,
                                                                     RequireValue(EncodeReflectedShaderArtifact(cook.reflected)))));
        REQUIRE(
            RequireValue(Viewport::PrepareMotionPipeline(viewport, RenderTextureFormat::Rgba16Float, RenderTextureFormat::Depth32Float)));
        if (withCoverage) {
            const auto coverage = ApplicationVulkanFixture::CookCoverage();
            viewport.StageXrCoverageProgram(RequireValue(
                Editor::LoadEditorVulkanMultiviewCoverageCook(coverage.compiled.payload,
                                                              RequireValue(EncodeReflectedShaderArtifact(coverage.reflected)))));
        }
        if (expectProbe)
            REQUIRE(Viewport::RequestCoverageProbe(viewport, probe));
        auto geometry = RequireValue(fixture.source.Port().Capture(fixture.definition.Revision(), fixture.scene->View()));
        const auto initialInstances = RequireValue(geometry->Update(fixture.scene->View()));
        REQUIRE(initialInstances.size() == 1);
        std::vector<RenderStaticMeshInstance> instances(initialInstances.begin(), initialInstances.end());
        const auto initialObjects = geometry->MotionObjects();
        std::vector<RenderMotionObjectId> objects(initialObjects.begin(), initialObjects.end());
        if (coverageMode == CoverageMode::SceneGrew) {
            instances.push_back(instances.front());
            objects.push_back(RenderMotionObjectId{objects.front().value + 1});
        }
        const auto makeImage = [&](const std::uintptr_t native, const FramebufferExtent extent, const RenderTextureFormat format,
                                   const bool isDepth) {
            auto source = std::make_shared<ViewportWriterImage>();
            source->device = device;
            source->incarnation = support.deviceIncarnation;
            source->id = native;
            source->allocation = {.extent = extent,
                                  .format = format,
                                  .layerCount = 2,
                                  .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
            source->depth = isDepth;
            return source;
        };
        const auto firstSource = makeImage(900, {8, 8}, RenderTextureFormat::Rgba8Unorm, false);
        const auto secondSource = makeImage(901, {8, 8}, RenderTextureFormat::Rgba8Unorm, false);
        const auto motionSource = makeImage(902, {6, 5}, RenderTextureFormat::Rgba16Float, false);
        const auto depthSource = makeImage(903, {6, 5}, RenderTextureFormat::Depth32Float, true);
        const Viewport::AuxiliaryPlan plan{{motionSource->allocation, motionSource->AcquiredState(), motionSource->RequiredFinalState()},
                                           {depthSource->allocation, depthSource->AcquiredState(), depthSource->RequiredFinalState()},
                                           {{{4, 5}, {6, 3}}}};
        if (expectProbe)
            vulkan.resourceState.state.rejectedImage = VulkanResourceTest::NativeResourceState::RejectedImage{VK_FORMAT_R32_SFLOAT, {6, 5}};
        const auto prepared = RequireValue(Viewport::PreparePairedTargets(viewport, geometry->Resources(), firstSource->allocation,
                                                                          firstSource->AcquiredState(), firstSource->RequiredFinalState(),
                                                                          plan, initialInstances.size()));
        CHECK(Viewport::IsPrepared(viewport, prepared));
        CHECK_FALSE(Viewport::HasPreparedReduction(viewport, prepared));
        const auto first = ImportWriterImage(frontend, firstSource);
        const auto second = ImportWriterImage(frontend, secondSource);
        const auto motion = ImportWriterImage(frontend, motionSource);
        const auto depth = ImportWriterImage(frontend, depthSource);
        const Viewport::AuxiliaryImages pair{motion.texture, depth.texture, motion.views, depth.views};

        auto clip = Math::Mat4::Identity();
        clip.values[12] = -2.0F;
        clip.values[13] = -3.0F;
        clip.values[14] = 0.5F;
        std::array eyes{MultiviewSceneView{0, clip}, MultiviewSceneView{1, clip}};
        MultiviewPublication publication{1, 1, 1, 1, support.deviceIncarnation, support.capabilityRevision};
        Viewport::MotionInput motionFrame{{publication, 1, 1'000'000}, objects};
        const auto predecessor = motionFrame.frame;
        CHECK(Viewport::Bind(viewport, first.texture, first.views, publication, eyes, instances, &motionFrame,
                             Editor::EditorXrProjectionAlpha::Opaque, &pair)
                  .HasError());
        const std::array colorRects{MultiviewRenderRectangle{1, 1, 5, 7}, MultiviewRenderRectangle{2, 0, 6, 4}};
        auto outside = colorRects;
        ++outside.back().width;
        CHECK(Viewport::Bind(viewport, first.texture, first.views, publication, eyes, instances, &motionFrame,
                             Editor::EditorXrProjectionAlpha::Opaque, nullptr, nullptr, &outside)
                  .HasError());
        RequireValue(Viewport::Bind(viewport, first.texture, first.views, publication, eyes, instances, &motionFrame,
                                    Editor::EditorXrProjectionAlpha::Opaque, nullptr, nullptr, &colorRects));
        const auto mirror = Viewport::Mirror(viewport, publication.frame, first.views.front());
        REQUIRE(mirror.sampledView == first.views.front());
        CHECK(mirror.u0 == 1.0F / 8.0F);
        CHECK(mirror.u1 == 6.0F / 8.0F);
        CHECK(mirror.v0 == 1.0F / 8.0F);
        CHECK(mirror.v1 == 1.0F);
        const auto submit = [&](const std::uint64_t sequence) {
            CHECK_FALSE(Viewport::TakeCoverage(viewport, motionFrame.frame));
            auto frame = RequireValue(frontend.BeginFrame({sequence, {8, 8}}));
            const auto encoded = RequireValue(Viewport::Execute(viewport, frame, {}));
            CHECK(RequireValue(Viewport::ResolveSubmission(viewport, encoded)) == Viewport::SubmissionState::Pending);
            RequireValue(frame.Present());
            CHECK(RequireValue(Viewport::ResolveSubmission(viewport, encoded)) == Viewport::SubmissionState::Accepted);
            auto other = encoded;
            ++other.signal.value;
            CHECK(Viewport::ResolveSubmission(viewport, other).HasError());
            CHECK_FALSE(Viewport::TakeCoverage(viewport, motionFrame.frame));
            vulkan.commandState.state.Complete();
            RequireValue(Viewport::Poll(viewport));
            if (expectCoverage && sequence == 2) {
                CHECK_FALSE(Viewport::TakeCoverage(viewport, predecessor));
                const auto coverage = Viewport::TakeCoverage(viewport, motionFrame.frame);
                REQUIRE(coverage);
                CHECK(coverage->frame == motionFrame.frame);
                CHECK(coverage->predecessor == predecessor);
                CHECK(coverage->texture.IsValid());
                CHECK(coverage->eyes.front() != coverage->eyes.back());
                const std::array expectedRectangles{MultiviewRenderRectangle{0, 0, expectProbe ? 8U : 4U, expectProbe ? 8U : 5U},
                                                    MultiviewRenderRectangle{0, 0, expectProbe ? 8U : 6U, expectProbe ? 8U : 3U}};
                CHECK(coverage->rectangles == expectedRectangles);
                CHECK(coverage->extent == (expectProbe ? firstSource->allocation.extent : motionSource->allocation.extent));
                CHECK(coverage->completion == encoded.signal);
                CHECK(coverage->pixels.has_value() == expectProbe);
                CHECK(coverage->auxiliary.has_value() == !expectProbe);
                if (expectProbe) {
                    REQUIRE(coverage->pixels);
                    CHECK(coverage->pixels->tiles == probe);
                    CHECK(coverage->pixels->count == 20);
                } else {
                    REQUIRE(coverage->auxiliary);
                    CHECK(coverage->auxiliary->motion == pair.motion);
                    CHECK(coverage->auxiliary->depth == pair.depth);
                    CHECK(coverage->auxiliary->motionViews == pair.motionViews);
                    CHECK(coverage->auxiliary->depthViews == pair.depthViews);
                }
            }
            CHECK_FALSE(Viewport::TakeCoverage(viewport, motionFrame.frame));
            // This is the fixture's caller-supplied projection result, independent of any GPU coverage claim.
            Viewport::CompleteMotion(viewport, motionFrame.frame, true);
        };
        submit(1);
        REQUIRE(vulkan.encoded.scissors.size() >= colorRects.size());
        for (std::size_t eye = 0; eye < colorRects.size(); ++eye) {
            const auto &scissor = vulkan.encoded.scissors[eye];
            const auto &rectangle = colorRects[eye];
            CHECK(scissor.offset.x == static_cast<std::int32_t>(rectangle.x));
            CHECK(scissor.offset.y == static_cast<std::int32_t>(rectangle.y));
            CHECK(scissor.extent.width == rectangle.width);
            CHECK(scissor.extent.height == rectangle.height);
        }
        const auto previousDraws = vulkan.encoded.drawCalls.size();
        REQUIRE(previousDraws == instances.size() * 2);
        CHECK(fixture.vulkan->port.motionUniformWrites == 0);
        const auto begins = vulkan.encoded.renderingCalls.size();
        ++publication.frame;
        ++motionFrame.frame.projectionSequence;
        motionFrame.frame.predictedDisplayTimeNanoseconds += 1'000'000;
        motionFrame.frame.publication = publication;
        eyes.back().worldToClip.values[12] += 0.125F;
        RequireValue(Viewport::Bind(viewport, second.texture, second.views, publication, eyes, instances, &motionFrame,
                                    Editor::EditorXrProjectionAlpha::Opaque, expectProbe ? nullptr : &pair));
        submit(2);
        REQUIRE(vulkan.encoded.coverageCopies.size() == (expectProbe ? 2 : 0));
        if (expectProbe) {
            const auto &copy = vulkan.encoded.coverageCopies;
            CHECK(copy[0].imageSubresource.baseArrayLayer == 0);
            CHECK(copy[0].imageOffset.x == 1);
            CHECK(copy[1].imageSubresource.baseArrayLayer == 1);
            CHECK(copy[1].imageOffset.x == 2);
            CHECK(copy[1].bufferOffset == 12 * sizeof(float));
        }
        const auto expected = coverageMode == CoverageMode::Fits ? 6U : 4U;
        CHECK(vulkan.encoded.drawCalls.size() == previousDraws + expected * instances.size());
        REQUIRE(vulkan.encoded.renderingCalls.size() == begins + expected);
        const auto commands = std::span{vulkan.encoded.renderingCalls}.subspan(begins);
        CHECK(commands[2].extent.width == (expectProbe ? 8U : 4U));
        CHECK(commands[2].extent.height == (expectProbe ? 8U : 5U));
        CHECK(commands[3].extent.width == (expectProbe ? 8U : 6U));
        CHECK(commands[3].extent.height == (expectProbe ? 8U : 3U));
        CHECK(commands[2].depth);
        CHECK(commands[3].depth);
        CHECK(commands[2].colorClear.color.float32[3] == 0.0F);
        CHECK(commands[3].colorClear.color.float32[3] == 0.0F);
        CHECK(fixture.vulkan->port.motionUniformWrites == (coverageMode == CoverageMode::Fits ? 2 : 1) * instances.size());
        CHECK(vulkan.commandState.state.accepted.size() == 2);
        CHECK(vulkan.commandState.state.waitCalls == 0);
        RequireValue(Viewport::Retire(viewport));
        CHECK_FALSE(Viewport::IsPrepared(viewport, prepared));
        for (const auto &image : {first, second, motion, depth}) {
            for (const auto view : image.views)
                RequireValue(frontend.ReleaseTextureView(view));
            RequireValue(frontend.ReleaseTexture(image.texture));
        }
        RequireValue(frontend.ProcessResourceRequests());
    }

    TEST_CASE("Real installed Play suspends XR admission while the bounded event poll has unread work", "[xr][application][play][events]") {
        ApplicationFixture fixture{true};
        auto scene = PublishPlay(fixture);
        using enum Host::OpenXRPlayPresentation;
        REQUIRE(fixture.play->PresentationState() == FrameAdmitted);
        fixture.script.clock.sample = 1;
        REQUIRE(fixture.play->PrepareRenderFrame(1).Value());
        REQUIRE(fixture.play->FinishGui(false).HasValue());
        const auto polls = fixture.script.events.polls;
        for (std::uint32_t index = 0; index < Native::OpenXRSessionEvents::MaximumEventsPerPoll; ++index)
            fixture.script.events.Push(XR_SESSION_STATE_FOCUSED);
        fixture.script.events.Push(XR_SESSION_STATE_VISIBLE);

        REQUIRE(fixture.play->PollEvents().HasValue());
        CHECK(fixture.play->PresentationState() == Pending);
        const auto inspecting = fixture.play->ReadInspection();
        CHECK(inspecting.status == Editor::EditorXrInspectionStatus::EventsPending);
        CHECK(inspecting.trackingRevision == 0);
        CHECK_FALSE(inspecting.focused);
        CHECK(fixture.script.events.polls - polls == Native::OpenXRSessionEvents::MaximumEventsPerPoll);
        REQUIRE(fixture.play->ProgressRenderTail().HasValue());
        CHECK(fixture.script.frames.ends == 0);
        const auto prepared = fixture.play->PrepareRenderFrame(2);
        REQUIRE(prepared.HasValue());
        CHECK_FALSE(prepared.Value());
        CHECK(fixture.script.frames.waits == 1);
        CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
        REQUIRE(fixture.play->PollEvents().HasValue());
        CHECK(fixture.play->PresentationState() == FrameAdmitted);
        CHECK(fixture.script.frames.ends == 1);
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Real installed Play consumes runtime loss before the frame tail and fences future preparation",
              "[xr][application][play][events]") {
        const Input::RawInputSnapshot raw{};
        const CancellationSource cancellation;
        ApplicationFixture fixture{true};
        auto scene = PublishPlay(fixture);
        fixture.script.clock.sample = 1;
        REQUIRE(fixture.play->PrepareRenderFrame(1).Value());
        REQUIRE(fixture.play->FinishGui(false).HasValue());
        bool instanceLost = false;
        SECTION("session loss preserves the same selected discovery for a new Play occurrence") {
            fixture.script.events.pollResult = XR_ERROR_SESSION_LOST;
        }
        SECTION("instance loss rejects reuse of the startup-selected graphics binding") {
            fixture.script.events.loseInstance = true;
            instanceLost = true;
        }
        REQUIRE(fixture.play->PollEvents().HasValue());
        CHECK_FALSE(fixture.play->UsesXrScene());
        CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
        CHECK(fixture.script.frames.ends == 0);
        CHECK(fixture.script.swapchain.releases == 0);
        REQUIRE(fixture.play->PollEvents().HasValue());
        CHECK(fixture.script.native.liveSessions == 0);
        CHECK(fixture.script.native.liveInstances == 1);
        CHECK(fixture.script.frames.ends == 0);
        CHECK(fixture.script.swapchain.releases == 0);
        CHECK(fixture.script.events.ends == 0);
        CHECK(fixture.vulkan->native.commandState.state.waitCalls == 0);
        // The application must commit the neutral Input cutoff before another occurrence can reserve a source.
        REQUIRE(fixture.play->BuildInputSnapshot({.frameNumber = 2, .cancellation = cancellation.Token()}, raw).HasValue());
        REQUIRE(fixture.play->SetPlayState(Editor::EditorPlayState::Idle).HasValue());
        auto replacement = RequireValue(Runtime::RuntimeScene::Create(fixture.definition, {2}));
        auto participant = fixture.play->MakeSceneParticipant();
        auto prepared = participant->Prepare(fixture.definition, replacement->View());
        if (instanceLost) {
            REQUIRE(prepared.HasError());
            CHECK(prepared.ErrorValue().code.Value() == XR::XRErrors::RuntimeUnavailable.code.Value());
            CHECK_FALSE(fixture.play->ReadSetup().rendererReady);
            CHECK(fixture.play->RequestSetupTest().HasError());
        } else {
            auto preparedScene = RequireValue(std::move(prepared));
            REQUIRE(preparedScene->ValidatePublication().HasValue());
            preparedScene->Publish();
            scene->Shutdown();  // A delayed outgoing candidate cannot retire the replacement.
            CHECK(fixture.play->UsesXrScene());
            CHECK(fixture.script.native.liveSessions == 1);
            preparedScene->Shutdown();
        }
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }

    TEST_CASE("Real installed Play closes nonrendering and failed predictions through the native zero-layer protocol",
              "[xr][application][play][frame]") {
        ApplicationFixture fixture{true};
        auto scene = PublishPlay(fixture);
        fixture.script.clock.sample = 1;
        SECTION("native runtime declines rendering") {
            INFO("native runtime declines rendering");
            fixture.script.frames.render = false;
        }
        SECTION("native wait failure retains no borrowed eye") {
            INFO("native wait failure retains no borrowed eye");
            fixture.script.frames.waitResult = XR_ERROR_RUNTIME_FAILURE;
        }
        SECTION("prediction misses actual converted deadline") {
            INFO("prediction misses actual converted deadline");
            fixture.script.clock.sample = 100'000'000;
        }
        SECTION("successful acquisition is explicitly aborted before submission") {
            INFO("successful acquisition is explicitly aborted before submission");
            REQUIRE(fixture.play->PrepareRenderFrame(1).Value());
            CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
            REQUIRE(fixture.play->FinishGui(false).HasValue());
            REQUIRE(fixture.play->ProgressRenderTail().HasValue());
            CHECK(fixture.script.frames.layers == 0);
            scene->Shutdown();
            REQUIRE(RequireValue(fixture.ClosePlay()));
            CHECK(fixture.script.events.exits == 1);
            CHECK(fixture.script.events.ends == 1);
            return;
        }
        const auto prepared = fixture.play->PrepareRenderFrame(1);
        CHECK((prepared.HasError() || !prepared.Value()));
        CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
        static_cast<void>(fixture.play->ProgressRenderTail());
        scene->Shutdown();
        fixture.script.frames.waitResult = XR_SUCCESS;
        fixture.script.frames.beginResult = XR_SUCCESS;
        REQUIRE(RequireValue(fixture.ClosePlay()));
        CHECK(fixture.script.events.exits == 1);
        CHECK(fixture.script.events.ends == 1);
    }

    TEST_CASE("Real installed Play recovers a terminal native begin fault through repeated bounded Close",
              "[xr][application][play][frame][recovery]") {
        ApplicationFixture fixture{true};
        auto scene = PublishPlay(fixture);
        fixture.script.clock.sample = 1;
        fixture.script.frames.beginResult = XR_ERROR_RUNTIME_FAILURE;
        const auto prepared = fixture.play->PrepareRenderFrame(1);
        INFO((prepared.HasError() ? OpenXRProduct::ErrorText(prepared.ErrorValue()) : "unexpected preparation success"));
        REQUIRE(prepared.HasError());
        CHECK(prepared.ErrorValue().code.Value() == XR::XRErrors::OperationUnavailable.code.Value());
        CHECK(prepared.ErrorValue().message.find("begin frame") != std::string::npos);
        CHECK(fixture.script.frames.begins == 1);
        CHECK(fixture.script.frames.ends == 0);
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
        REQUIRE(RequireValue(fixture.ClosePlay()));
        CHECK(fixture.script.frames.ends == 0);
        CHECK(fixture.script.events.ends == 0);
        CHECK(fixture.script.events.exits == 0);
        CHECK(fixture.script.native.liveSessions == 0);
        CHECK(fixture.script.actions.liveSets == 0);
        CHECK(fixture.script.actions.liveActions == 0);
        CHECK(fixture.script.actions.tracking.spaces == 0);
        CHECK(fixture.vulkan->native.commandState.state.waitCalls == 0);
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
        RequireValue(frame.ComposeGui(gui));
        CHECK(gui.records == 1);
        REQUIRE(frame.Present().HasValue());
        REQUIRE(fixture.play->FinishGui(true).HasValue());
        CHECK_FALSE(fixture.play->MirrorView(1).IsValid());
        RequireValue(fixture.play->ProgressRenderTail());
        CHECK(fixture.script.swapchain.releases == 0);
        CHECK(fixture.script.frames.ends == 0);
        fixture.vulkan->native.commandState.state.Complete();
        RequireValue(fixture.play->ProgressRenderTail());
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
        REQUIRE(RequireValue(fixture.ClosePlay()));
        CHECK(fixture.script.events.exits == 1);
        CHECK(fixture.script.events.ends == 1);
        CHECK(fixture.script.native.liveSessions == 0);
    }

    TEST_CASE("Real installed Play admits only the exact native queue-accepted projection", "[xr][application][play][frame][submission]") {
        ApplicationFixture fixture{true};
        auto scene = PublishPlay(fixture);
        auto &graphics = fixture.vulkan->native.commandState.state;
        fixture.script.clock.sample = 1;
        enum class NativeOutcome {
            Cancel,
            Accept,
            Reject
        };
        std::uint64_t number{};
        for (const auto outcome : {NativeOutcome::Cancel, NativeOutcome::Accept, NativeOutcome::Reject}) {
            ++number;
            CAPTURE(number);
            REQUIRE(RequireValue(fixture.play->PrepareRenderFrame(number)));
            auto frame = RequireValue(fixture.frontend->BeginFrame({number, {8, 8}}));
            RequireValue(fixture.play->ExecuteRenderFrame(frame, {}));
            // Sealing GUI precedes the product host's native Present.
            RequireValue(fixture.play->FinishGui(true));
            RequireValue(fixture.play->ProgressRenderTail());
            CHECK(fixture.script.frames.ends == number - 1);
            CHECK(fixture.script.swapchain.releases == number - 1);
            switch (outcome) {
                case NativeOutcome::Cancel:
                    frame.Cancel();
                    break;
                case NativeOutcome::Accept:
                    RequireValue(frame.Present());
                    graphics.Complete();
                    break;
                case NativeOutcome::Reject:
                    // A real native submit failure closes this Vulkan graph owner to further submissions.
                    graphics.submitResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                    REQUIRE(frame.Present().HasError());
                    break;
            }
            RequireValue(fixture.play->ProgressRenderTail());
            CHECK(fixture.script.frames.ends == number);
            CHECK(fixture.script.swapchain.releases == number);
            CHECK(fixture.script.frames.layers == (outcome == NativeOutcome::Accept ? 1 : 0));
            fixture.script.frames.prediction += 1'000'000;
        }
        scene->Shutdown();
        REQUIRE(RequireValue(fixture.ClosePlay()));
    }
}  // namespace Horo::Tests::OpenXRApplication

#include "OpenXRRealVulkanReprojectionSmoke.inl"

namespace Horo::Tests::OpenXRApplication {
    TEST_CASE("Selected XR depth history keeps ordinary projection on optional second allocation failure", "[xr][vulkan][host][depth]") {
        using namespace Render;
        ApplicationFixture fixture{true};
        const auto cooked = ApplicationVulkanFixture::CookMotion();
        fixture.viewport->StageXrMotionProgram(
            RequireValue(Editor::LoadEditorVulkanMultiviewMotionCook(cooked.compiled.payload,
                                                                     RequireValue(EncodeReflectedShaderArtifact(cooked.reflected)))));
        REQUIRE(fixture.viewport->PrepareXrMotionPipeline(RenderTextureFormat::Rgba16Float, RenderTextureFormat::Depth32Float).Value());
        const auto baseline = fixture.frontend->MemorySnapshot();
        auto &native = fixture.vulkan->native.resourceState.state;
        const auto priorViews = native.createViewCount;
        native.failCreateViewCall = priorViews + 4;
        const auto device = RequireValue(Editor::EditorVulkanXrViewportAccess::Device(*fixture.viewport));
        const RenderTextureDescriptor color{.extent = {8, 8},
                                            .layerCount = 2,
                                            .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled};
        const RenderGraphLogicalState acquired{RenderGraphSynchronizationAccess::ReadWrite,
                                               RenderGraphSynchronizationOperation::ColorAttachment, RenderGraphPipelineScope::Graphics,
                                               RenderGraphTextureLayout::ColorAttachment, device.graphicsIdentity};
        auto final = acquired;
        final.scope = RenderGraphPipelineScope::External;
        REQUIRE(fixture.viewport->PrepareXrGraph({}, color, acquired, final, true).HasValue());
        CHECK(native.createViewCount == priorViews + 4);
        REQUIRE(Editor::EditorVulkanXrViewportAccess::Retire(*fixture.viewport).HasValue());
        CHECK(fixture.frontend->MemorySnapshot().allocationCount == baseline.allocationCount);
        CHECK(fixture.frontend->MemorySnapshot().committedBackingBytes == baseline.committedBackingBytes);
        CHECK(fixture.frontend->UploadSnapshot().pendingRequests == 0);
    }

    TEST_CASE("Actual XR viewport retires failed depth prefixes and admits a fresh preparation", "[xr][vulkan][host][lifecycle]") {
        ApplicationFixture fixture{true};
        auto &frontend = *fixture.frontend;
        auto &state = fixture.vulkan->native.resourceState.state;
        const auto baseline = frontend.MemorySnapshot();
        int failingCall = 1;
        SECTION("depth native realization") {}
        SECTION("first eye view") {
            failingCall = 2;
        }
        SECTION("second eye view after a successful prefix") {
            failingCall = 3;
        }
        state.failCreateViewCall = state.createViewCount + failingCall;
        const auto device = OpenXRProduct::RequireValue(Editor::EditorVulkanXrViewportAccess::Device(*fixture.viewport));
        const Render::RenderTextureDescriptor color{.extent = {8, 8},
                                                    .layerCount = 2,
                                                    .usage =
                                                        Render::RenderTextureUsage::RenderAttachment | Render::RenderTextureUsage::Sampled};
        const Render::RenderGraphLogicalState acquired{Render::RenderGraphSynchronizationAccess::ReadWrite,
                                                       Render::RenderGraphSynchronizationOperation::ColorAttachment,
                                                       Render::RenderGraphPipelineScope::Graphics,
                                                       Render::RenderGraphTextureLayout::ColorAttachment, device.graphicsIdentity};
        auto final = acquired;
        final.scope = Render::RenderGraphPipelineScope::External;
        const auto failed = fixture.viewport->PrepareXrGraph({}, color, acquired, final);
        INFO((failed.HasError() ? OpenXRProduct::ErrorText(failed.ErrorValue()) : "unexpected successful injected realization"));
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == Render::VulkanBackendErrors::ResourceCreationFailed.code.Value());
        const auto original = failed.ErrorValue();
        state.failCreateViewCall = 0;
        REQUIRE(Editor::EditorVulkanXrViewportAccess::Retire(*fixture.viewport).HasValue());
        CHECK(frontend.UploadSnapshot().pendingRequests == 0);
        CHECK(frontend.MemorySnapshot().reservationCount == baseline.reservationCount);
        CHECK(frontend.MemorySnapshot().allocationCount == baseline.allocationCount);
        CHECK(frontend.MemorySnapshot().committedBackingBytes == baseline.committedBackingBytes);
        REQUIRE(fixture.viewport->PrepareXrGraph({}, color, acquired, final).HasValue());
        CHECK(failed.ErrorValue().code.Value() == original.code.Value());
        CHECK(failed.ErrorValue().message == original.message);
        REQUIRE(Editor::EditorVulkanXrViewportAccess::Retire(*fixture.viewport).HasValue());
        CHECK(frontend.MemorySnapshot().allocationCount == baseline.allocationCount);
        CHECK(frontend.MemorySnapshot().committedBackingBytes == baseline.committedBackingBytes);
        CHECK(fixture.vulkan->native.commandState.state.waitCalls == 0);
    }
}  // namespace Horo::Tests::OpenXRApplication
