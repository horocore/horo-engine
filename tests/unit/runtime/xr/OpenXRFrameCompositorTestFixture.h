#include "support/OpenXRFrameScriptTestSupport.h"
#pragma once

/** @file OpenXRFrameCompositorTestFixture.h
 * @brief Scripted official-dispatch fixture shared by compositor lifecycle cases. */
#include "OpenXRFrameCompositor.h"
#include "support/OpenXRSwapchainTestSupport.h"

#include <array>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <thread>

namespace Horo::Tests::OpenXR {
    namespace {
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
            XRCoordinateSpace reference;

            Result<XrSpace> Borrow(const XRCoordinateSpace &space) const override {
                return stale || space != reference ? Result<XrSpace>::Failure(MakeError(XRErrors::IdentityStale))
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

            Result<void> Enumerate(const NativeSwapchainRequest &, std::uint32_t, const IOpenXRSwapchainImageEnumeration &) override {
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
                spaces.reference = local;
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
                REQUIRE(compositor.Bind({&FrameScript::GetProc}, {}).HasValue());
            }

            XRFrameId Begin() {
                const auto waited = compositor.Wait();
                REQUIRE(waited.HasValue());
                REQUIRE(waited.Value().status == XRFrameStatus::Ok);
                REQUIRE(compositor.Begin(waited.Value().frame).HasValue());
                return waited.Value().frame;
            }

            /** @brief Acquires one actual fixture image and records it in the same canonical frame gate. */
            XRSwapchainImageId Acquire(const XRFrameId &frame) {
                REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
                REQUIRE(color.Acquire().HasValue());
                REQUIRE(color.Wait(0).Value());
                const auto imported = color.Import();
                REQUIRE(imported.HasValue());
                const auto image = imported.Value().image;
                REQUIRE(frames.Acquire(frame, image) == XRFrameStatus::Ok);
                return image;
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

            /** @brief Builds one owned predicted view without borrowing temporary pose storage. */
            XRViewDescriptor View(std::uint32_t index, XRRenderPredictionTime prediction) {
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
                return {.id = {session, {10 + index, 1}},
                        .order = index,
                        .role = index == 0 ? XRViewRole::PrimaryLeft : XRViewRole::PrimaryRight,
                        .pose = pose,
                        .projection = {-1, 1, -1, 1},
                        .recommendedExtent = {800, 600},
                        .maximumExtent = {800, 600}};
            }

            XRViewRenderPlan Plan(const XRFrameId &frame, const XRSwapchainImageId &image, const XRSwapchainImageId *depthImage = nullptr,
                                  bool requireSampling = false) {
                const auto prediction = frames.Snapshot().predictedDisplayTime;
                std::array<XRViewDescriptor, 2> views{};
                std::array<XRExternalRenderTargetDescriptor, 4> targets{};
                for (std::uint32_t index = 0; index < 2; ++index) {
                    views[index] = View(index, prediction);
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

    }  // namespace
}  // namespace Horo::Tests::OpenXR
