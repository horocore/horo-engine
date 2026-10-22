#include "OpenXRFrameCompositor.h"
#include "support/OpenXRSwapchainTestSupport.h"

#include <array>
#include <cmath>
#include <thread>

namespace Horo::Tests::OpenXR {
    namespace {
        /** @brief Official native frame dispatch script, never a physical-device qualification. */
        struct FrameScript final {
            static inline FrameScript *active{};
            XrResult waitResult{XR_SUCCESS};
            XrResult beginResult{XR_SUCCESS};
            XrResult endResult{XR_SUCCESS};
            XrTime prediction{10'000'000};
            XrDuration period{11'111'111};
            bool render{true};
            bool missingEnd{};
            std::uint32_t waits{};
            std::uint32_t begins{};
            std::uint32_t ends{};
            std::uint32_t layers{};
            std::uint32_t views{};
            std::array<XrCompositionLayerProjectionView, 2> encoded{};
            std::array<XrCompositionLayerDepthInfoKHR, 2> depths{};

            FrameScript() {
                active = this;
            }

            ~FrameScript() {
                active = nullptr;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Wait(XrSession, const XrFrameWaitInfo *, XrFrameState *state) {
                ++active->waits;
                state->predictedDisplayTime = active->prediction;
                state->predictedDisplayPeriod = active->period;
                state->shouldRender = active->render ? XR_TRUE : XR_FALSE;
                return active->waitResult;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Begin(XrSession, const XrFrameBeginInfo *) {
                ++active->begins;
                return active->beginResult;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL End(XrSession, const XrFrameEndInfo *info) {
                ++active->ends;
                CHECK(info->displayTime == active->prediction);
                CHECK(info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
                active->layers = info->layerCount;
                if (info->layerCount == 1) {
                    const auto *layer = reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[0]);
                    CHECK(layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
                    active->views = layer->viewCount;
                    REQUIRE(layer->viewCount == 2);
                    for (std::size_t index = 0; index < 2; ++index) {
                        active->encoded[index] = layer->views[index];
                        if (layer->views[index].next)
                            active->depths[index] = *static_cast<const XrCompositionLayerDepthInfoKHR *>(layer->views[index].next);
                    }
                }
                return active->endResult;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance, const char *name, PFN_xrVoidFunction *function) {
                *function = nullptr;
                if (std::string_view{name} == "xrWaitFrame")
                    *function = std::bit_cast<PFN_xrVoidFunction>(&Wait);
                if (std::string_view{name} == "xrBeginFrame")
                    *function = std::bit_cast<PFN_xrVoidFunction>(&Begin);
                if (std::string_view{name} == "xrEndFrame" && !active->missingEnd)
                    *function = std::bit_cast<PFN_xrVoidFunction>(&End);
                return *function ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
            }
        };

        class Resources final : public IXRSessionResources {
        public:
            Result<void> Prepare(XRSessionPreparation, const XRSessionId &, const XRFeaturePlan &) override {
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation, const XRSessionId &) noexcept override {}
        };

        class Spaces final : public IOpenXRCompositionSpace {
        public:
            bool stale{};

            Result<XrSpace> Borrow(const XRCoordinateSpace &) const override {
                return stale ? Result<XrSpace>::Failure(MakeError(XRErrors::IdentityStale))
                             : Result<XrSpace>::Success(NativeHandle<XrSpace>(8));
            }
        };

        class Images final : public IOpenXRSwapchainImages {
        public:
            bool complete{};

            Result<void> Validate(const NativeSwapchainRequest &) const override {
                return Result<void>::Success();
            }

            Result<std::int64_t> SelectFormat(const NativeSwapchainRequest &, std::span<const std::int64_t> formats) const override {
                return Result<std::int64_t>::Success(formats.front());
            }

            Result<void> Enumerate(std::uint32_t, const IOpenXRSwapchainImageEnumeration &) override {
                return Result<void>::Success();
            }

            Result<Render::RenderTextureHandle> Import(const NativeSwapchainRequest &, const XRSwapchainImageId &, std::uint32_t) override {
                return Result<Render::RenderTextureHandle>::Success({{1}, 1, 1});
            }

            Result<void> Retire(Render::RenderTextureHandle) override {
                return Result<void>::Success();
            }

            Result<bool> RetirementComplete(Render::RenderTextureHandle) override {
                return Result<bool>::Success(complete);
            }

            Result<void> Reset() override {
                return Result<void>::Success();
            }
        };

        struct CompositorFixture final {
            Fixture native;
            Resources resources;
            XRSessionLifecycle sessions{resources};
            XRFrameLifecycle frames{sessions};
            Spaces spaces;
            FrameScript script;
            SwapchainScript swapScript;
            Images images;
            OpenXRSwapchain color{native.owner, images};
            OpenXRSwapchain depthOwner{native.owner, images};
            OpenXRFrameCompositor compositor{native.owner, frames, spaces};
            XRSessionId session;
            XRViewConfigurationId configuration;
            XRCoordinateSpace local;
            NativeSwapchainRequest allocation;

            explicit CompositorFixture(bool enableDepth = false) {
                auto activated =
                    sessions.Activate(native.capabilities, native.capabilities.System(), native.capabilities.Revision(), native.plan);
                REQUIRE(activated.HasValue());
                session = activated.Value();
                REQUIRE(sessions.ApplyEvent(session, XRSessionEvent::Started).HasValue());
                auto request = native.Request();
                request.candidate = session;
                const std::array extensions{NativeExtensionRequest{XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, true}};
                if (enableDepth) {
                    native.script.depthExtension = true;
                    native.script.extensionCount = 2;
                    request.extensions = extensions;
                }
                REQUIRE(native.owner.Create(request).HasValue());
                configuration = {session, {3, 1}};
                local = {.id = {session, {7, 1}},
                         .kind = XRSpaceKind::Local,
                         .worldOriginRevision = IdentityValue<XRWorldOriginRevision>(1)};
                REQUIRE(frames.BindConfiguration(configuration, native.capabilities, {2, 2, 1}) == XRFrameStatus::Ok);
                allocation = {.target = {session, {4, 1}},
                              .texture = {.extent = {800, 600},
                                          .format = Render::RenderTextureFormat::Rgba8Unorm,
                                          .layerCount = 2,
                                          .usage = Render::RenderTextureUsage::RenderAttachment}};
                REQUIRE(color.Create(allocation, &SwapchainScript::GetProc).HasValue());
                if (enableDepth) {
                    auto depthAllocation = allocation;
                    depthAllocation.target.slot.index = 5;
                    depthAllocation.texture.format = Render::RenderTextureFormat::Depth32Float;
                    depthAllocation.role = NativeSwapchainRole::Depth;
                    REQUIRE(depthOwner.Create(depthAllocation, &SwapchainScript::GetProc).HasValue());
                }
            }

            ~CompositorFixture() {
                CHECK(frames.Snapshot().phase == XRFramePhase::Idle);
                CHECK(compositor.ResetAfterQuiescence().HasValue());
                swapScript.fail = {};
                images.complete = true;
                CHECK(color.Close().HasValue());
                CHECK(depthOwner.Close().HasValue());
            }

            void Bind() {
                REQUIRE(compositor.Bind(&FrameScript::GetProc, {}).HasValue());
            }

            XRFrameId Begin() {
                const auto waited = compositor.Wait();
                REQUIRE(waited.HasValue());
                REQUIRE(waited.Value().status == XRFrameStatus::Ok);
                REQUIRE(compositor.Begin(waited.Value().frame).HasValue());
                return waited.Value().frame;
            }

            void Empty(const XRFrameId &frame) {
                if (frames.Snapshot().renderAdmitted)
                    REQUIRE(frames.Abort(frame) == XRFrameStatus::Ok);
                REQUIRE(compositor.EndEmpty(frame).HasValue());
            }

            void Recover() {
                // Scripted native runtime is quiescent; production hosts must prove this before either reset.
                REQUIRE(compositor.ResetAfterQuiescence().HasValue());
                frames.ResetAfterQuiescence();
            }

            XRViewRenderPlan Plan(const XRFrameId &frame, const XRSwapchainImageId &image, const XRSwapchainImageId *depthImage = nullptr,
                                  bool requireSampling = false) {
                const auto prediction = frames.Snapshot().predictedDisplayTime;
                std::array<XRViewDescriptor, 2> views{};
                std::array<XRExternalRenderTargetDescriptor, 4> targets{};
                for (std::uint32_t index = 0; index < 2; ++index) {
                    const XRPoseDescriptor pose{.session = session,
                                                .source = {.id = {session, {20 + index, 1}},
                                                           .kind = XRSpaceKind::View,
                                                           .worldOriginRevision = local.worldOriginRevision},
                                                .target = local,
                                                .purpose = XRPosePurpose::PresentationPrediction,
                                                .time = {.runtimeSample = IdentityValue<XRRuntimeTime>(1), .renderPrediction = prediction},
                                                .components = {.positionMeters = {.value = Math::Vec3{0, 1, 0},
                                                                                  .validity = XRPoseComponentValidity::Tracked},
                                                               .orientation = {.value = Math::Quaternion::Identity(),
                                                                               .validity = XRPoseComponentValidity::Tracked},
                                                               .confidence = XRTrackingConfidence::High,
                                                               .loss = XRTrackingLossState::None}};
                    views[index] = {.id = {session, {10 + index, 1}},
                                    .order = index,
                                    .role = index == 0 ? XRViewRole::PrimaryLeft : XRViewRole::PrimaryRight,
                                    .pose = pose,
                                    .projection = {-1, 1, -1, 1},
                                    .recommendedExtent = {800, 600},
                                    .maximumExtent = {800, 600}};
                    const auto targetIndex = depthImage ? index * 2 : index;
                    targets[targetIndex] = {.view = views[index].id,
                                            .target = image.target,
                                            .image = image,
                                            .usage = requireSampling ? Render::RenderTextureUsage::RenderAttachment |
                                                                           Render::RenderTextureUsage::Sampled
                                                                     : Render::RenderTextureUsage::RenderAttachment,
                                            .allocationExtent = {800, 600},
                                            .renderRectangle = {0, 0, 800, 600},
                                            .arrayLayer = index,
                                            .arrayLayerCount = 2};
                    if (depthImage) {
                        targets[targetIndex + 1] = targets[targetIndex];
                        targets[targetIndex + 1].role = XRExternalRenderTargetRole::ProjectionDepth;
                        targets[targetIndex + 1].target = depthImage->target;
                        targets[targetIndex + 1].image = *depthImage;
                        targets[targetIndex + 1].format = Render::RenderTextureFormat::Depth32Float;
                    }
                }
                const std::array acquired{image, depthImage ? *depthImage : XRSwapchainImageId{}};
                auto plan = XRViewRenderPlan::Create({.configuration = {.session = session,
                                                                        .id = frame.configuration,
                                                                        .revision = IdentityValue<XRViewConfigurationRevision>(1),
                                                                        .type = XRViewConfigurationType::PrimaryStereo,
                                                                        .viewCount = 2},
                                                      .predictedDisplayTime = prediction,
                                                      .views = views,
                                                      .targets = std::span{targets}.first(depthImage ? 4 : 2),
                                                      .acquiredImages = std::span{acquired}.first(depthImage ? 2 : 1)},
                                                     session, configuration, IdentityValue<XRViewConfigurationRevision>(1),
                                                     local.worldOriginRevision, 2, {.supportsDepthTargets = depthImage != nullptr});
                REQUIRE(plan.HasValue());
                return plan.Value();
            }
        };

        TEST_CASE("Native XR compositor preserves predicted protocol and reports finite renderer deadlines",
                  "[unit][xr][compositor][pacing]") {
            CompositorFixture f;
            f.Bind();
            const auto frame = f.Begin();
            REQUIRE_FALSE(f.compositor.Wait().HasValue());
            CHECK(f.script.waits == 1);
            auto observation = f.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(8'500'000));
            REQUIRE(observation.HasValue());
            CHECK(observation.Value().deadline == NativeFrameDeadline::WithinBudget);
            CHECK(observation.Value().remainingNanoseconds == 0);
            CHECK(observation.Value().framesInFlight == 1);
            CHECK(observation.Value().predictedPeriodNanoseconds == f.script.period);
            observation = f.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(8'500'001));
            REQUIRE(observation.HasValue());
            CHECK(observation.Value().deadline == NativeFrameDeadline::CpuSubmissionMissed);
            observation = f.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(10'000'000));
            REQUIRE(observation.HasValue());
            CHECK(observation.Value().deadline == NativeFrameDeadline::DisplayMissed);
            REQUIRE_FALSE(f.compositor.EndEmpty(frame).HasValue());
            CHECK(f.script.ends == 0);
            f.Empty(frame);
            CHECK(f.script.layers == 0);
            REQUIRE_FALSE(f.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(1)).HasValue());
        }

        TEST_CASE("Native XR compositor submits complete released array views and fences reacquisition", "[unit][xr][compositor]") {
            CompositorFixture f;
            f.Bind();
            const auto frame = f.Begin();
            REQUIRE(f.frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            REQUIRE(f.color.Acquire().HasValue());
            REQUIRE(f.color.Wait(0).Value());
            const auto imported = f.color.Import();
            REQUIRE(imported.HasValue());
            const auto image = imported.Value().image;
            REQUIRE(f.frames.Acquire(frame, image) == XRFrameStatus::Ok);
            const auto plan = f.Plan(frame, image);
            const std::array owners{NativeProjectionTarget{&f.color, image}};
            REQUIRE_FALSE(f.compositor.EndProjection(frame, plan, owners).HasValue());
            CHECK(f.script.ends == 0);
            REQUIRE_FALSE(f.color.PollRelease().Value());
            REQUIRE_FALSE(f.color.BorrowReleased(image).HasValue());
            f.images.complete = true;
            const std::array completed{image};
            REQUIRE(f.frames.Submit(frame, completed) == XRFrameStatus::Ok);
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, image) == XRFrameStatus::Ok);
            f.spaces.stale = true;
            REQUIRE_FALSE(f.compositor.EndProjection(frame, plan, owners).HasValue());
            CHECK(f.script.ends == 0);
            f.spaces.stale = false;
            const auto incompatibleUsage = f.Plan(frame, image, nullptr, true);
            REQUIRE_FALSE(f.compositor.EndProjection(frame, incompatibleUsage, owners).HasValue());
            CHECK(f.script.ends == 0);
            REQUIRE(f.compositor.EndProjection(frame, plan, owners).HasValue());
            CHECK(f.script.layers == 1);
            CHECK(f.script.views == 2);
            CHECK(f.script.encoded[0].subImage.imageArrayIndex == 0);
            CHECK(f.script.encoded[1].subImage.imageArrayIndex == 1);
            CHECK(std::abs(f.script.encoded[0].fov.angleLeft + 0.78539816F) < 0.00001F);
            CHECK(f.script.encoded[0].pose.position.y == 1);
            REQUIRE(f.color.BorrowReleased(image).HasValue());
            REQUIRE(f.color.Acquire().HasValue());
            REQUIRE_FALSE(f.color.BorrowReleased(image).HasValue());
            REQUIRE(f.color.Wait(0).Value());
            REQUIRE(f.color.PollRelease().Value());
        }

        TEST_CASE("Native XR depth composition preserves explicit depth conventions and complete view chains",
                  "[unit][xr][compositor][depth]") {
            CompositorFixture f{true};
            f.Bind();
            const auto frame = f.Begin();
            REQUIRE(f.frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            REQUIRE(f.color.Acquire().HasValue());
            REQUIRE(f.color.Wait(0).Value());
            const auto color = f.color.Import();
            REQUIRE(color.HasValue());
            REQUIRE(f.depthOwner.Acquire().HasValue());
            REQUIRE(f.depthOwner.Wait(0).Value());
            const auto depth = f.depthOwner.Import();
            REQUIRE(depth.HasValue());
            const auto colorImage = color.Value().image;
            const auto depthImage = depth.Value().image;
            REQUIRE(f.frames.Acquire(frame, colorImage) == XRFrameStatus::Ok);
            REQUIRE(f.frames.Acquire(frame, depthImage) == XRFrameStatus::Ok);
            const auto plan = f.Plan(frame, colorImage, &depthImage);
            f.images.complete = true;
            const std::array complete{colorImage, depthImage};
            REQUIRE(f.frames.Submit(frame, complete) == XRFrameStatus::Ok);
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.depthOwner.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, colorImage) == XRFrameStatus::Ok);
            REQUIRE(f.frames.Release(frame, depthImage) == XRFrameStatus::Ok);
            const std::array owners{NativeProjectionTarget{&f.color, colorImage}, NativeProjectionTarget{&f.depthOwner, depthImage}};
            REQUIRE_FALSE(f.compositor.EndProjection(frame, plan, owners).HasValue());
            NativeProjectionDepth convention{.nearMeters = 1000, .farMeters = 0.1F};
            convention.maximumDepth = 2;
            REQUIRE_FALSE(f.compositor.EndProjection(frame, plan, owners, &convention).HasValue());
            CHECK(f.script.ends == 0);
            convention.maximumDepth = 1;
            REQUIRE(f.compositor.EndProjection(frame, plan, owners, &convention).HasValue());
            CHECK(f.script.depths[0].type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR);
            CHECK(f.script.depths[0].nearZ == 1000);
            CHECK(f.script.depths[0].farZ == 0.1F);
            CHECK(f.script.depths[1].subImage.imageArrayIndex == 1);
        }

        TEST_CASE("Native XR compositor rejects partial initialization budgets and unsupported color without native wait",
                  "[unit][xr][compositor]") {
            CompositorFixture f;
            REQUIRE_FALSE(f.compositor.Bind(nullptr, {}).HasValue());
            REQUIRE_FALSE(f.compositor.Bind(&FrameScript::GetProc, {.gpuBudgetNanoseconds = 0}).HasValue());
            REQUIRE_FALSE(f.compositor.Bind(&FrameScript::GetProc, {.maximumFramesInFlight = 2}).HasValue());
            REQUIRE_FALSE(f.compositor.Bind(&FrameScript::GetProc, {}, NativeCompositorColorSpace::Rec709).HasValue());
            f.script.missingEnd = true;
            REQUIRE_FALSE(f.compositor.Bind(&FrameScript::GetProc, {}).HasValue());
            REQUIRE_FALSE(f.compositor.Wait().HasValue());
            CHECK(f.script.waits == 0);
            f.script.missingEnd = false;
            f.Bind();
            const auto frame = f.Begin();
            f.Empty(frame);
        }

        TEST_CASE("Native XR compositor retains completed wait debt and native failure requires quiescent recovery",
                  "[unit][xr][compositor]") {
            CompositorFixture f;
            f.Bind();
            SECTION("wait failure cancels only reservation") {
                f.script.waitResult = XR_ERROR_RUNTIME_FAILURE;
                REQUIRE_FALSE(f.compositor.Wait().HasValue());
                CHECK(f.frames.Snapshot().phase == XRFramePhase::Idle);
                f.script.waitResult = XR_SUCCESS;
                const auto frame = f.Begin();
                f.Empty(frame);
            }
            SECTION("unusable period remains a real frame") {
                f.script.period = 0;
                const auto frame = f.Begin();
                REQUIRE_FALSE(f.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(1)).HasValue());
                f.Empty(frame);
            }
            SECTION("native begin failure") {
                const auto waited = f.compositor.Wait();
                REQUIRE(waited.HasValue());
                f.script.beginResult = XR_ERROR_RUNTIME_FAILURE;
                REQUIRE_FALSE(f.compositor.Begin(waited.Value().frame).HasValue());
                REQUIRE_FALSE(f.compositor.Wait().HasValue());
                CHECK(f.frames.Snapshot().phase == XRFramePhase::Waited);
                f.Recover();
            }
            SECTION("native end failure is not retried as successful") {
                const auto frame = f.Begin();
                REQUIRE(f.frames.Abort(frame) == XRFrameStatus::Ok);
                f.script.endResult = XR_ERROR_RUNTIME_FAILURE;
                REQUIRE_FALSE(f.compositor.EndEmpty(frame).HasValue());
                REQUIRE_FALSE(f.compositor.EndEmpty(frame).HasValue());
                CHECK(f.script.ends == 1);
                f.Recover();
            }
        }

        TEST_CASE("Native XR wrong-thread recovery cannot discard frame debt or close admission", "[unit][xr][compositor][lifecycle]") {
            CompositorFixture f;
            f.Bind();
            const auto frame = f.Begin();
            const auto now = IdentityValue<XRRenderPredictionTime>(1);
            bool rejectedReset{};
            bool rejectedShutdown{};
            bool rejectedObservation{};
            std::thread foreign{[&] {
                rejectedReset = f.compositor.ResetAfterQuiescence().HasError();
                rejectedShutdown = f.compositor.BeginShutdown().HasError();
                rejectedObservation = f.compositor.ObservePacing(frame, now).HasError();
            }};
            foreign.join();
            REQUIRE(rejectedReset);
            REQUIRE(rejectedShutdown);
            REQUIRE(rejectedObservation);
            CHECK(f.frames.Snapshot().frame == frame);
            CHECK(f.script.ends == 0);
            f.Empty(frame);
            const auto next = f.Begin();
            f.Empty(next);
        }

        TEST_CASE("Native XR logical replacement fences the outstanding native frame until quiescent recovery",
                  "[unit][xr][compositor][lifecycle]") {
            CompositorFixture f;
            f.Bind();
            const auto frame = f.Begin();
            const auto replacement =
                f.sessions.Activate(f.native.capabilities, f.native.capabilities.System(), f.native.capabilities.Revision(), f.native.plan);
            REQUIRE(replacement.HasValue());
            REQUIRE(replacement.Value() != frame.session);
            REQUIRE_FALSE(f.compositor.EndEmpty(frame).HasValue());
            CHECK(f.script.ends == 0);
            f.Recover();
        }

        TEST_CASE("Native XR shutdown closes waits while allowing the exact outstanding frame to retire", "[unit][xr][compositor]") {
            CompositorFixture f;
            f.Bind();
            f.script.render = false;
            const auto frame = f.Begin();
            REQUIRE(f.compositor.BeginShutdown().HasValue());
            REQUIRE_FALSE(f.compositor.Wait().HasValue());
            REQUIRE(f.compositor.EndEmpty(frame).HasValue());
            REQUIRE_FALSE(f.compositor.Wait().HasValue());
            CHECK(f.script.waits == 1);
            CHECK(f.script.ends == 1);
        }
    }  // namespace
}  // namespace Horo::Tests::OpenXR
