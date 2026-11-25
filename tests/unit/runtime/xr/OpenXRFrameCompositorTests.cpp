#include "OpenXRFrameCompositorTestFixture.h"
#include "support/OpenXREventScriptTestSupport.h"

namespace Horo::Tests::OpenXR {
    namespace {
        TEST_CASE("Native reconstruction underlay requires elected runtime support and explicit consent",
                  "[unit][xr][compositor][passthrough]") {
            const bool selected = GENERATE(false, true);
            CompositorFixture f{false, false, false, NativeReprojectionRoute::None, selected, 2};
            auto world = f.local;
            world.kind = XRSpaceKind::World;
            XRPassthroughScope scope{f.session, world, IdentityValue<XRPassthroughGrant>(1)};
            if (!selected || !NativePassthroughExtensionRequest()) {
                RequireFailureIdentity(f.native.owner.StartPassthrough(scope), XRErrors::OperationUnsupported);
                CHECK(f.passthrough.featuresCreated == 0);
                return;
            }

            SECTION("invalid grant is rejected before provider creation") {
                scope.grant = {};
                RequireFailureIdentity(f.native.owner.StartPassthrough(scope), XRErrors::OperationInvalid);
            }
            SECTION("native properties reject advertised capability without reconstruction") {
                f.census.passthroughCapabilities = 0;
                RequireFailureIdentity(f.native.owner.StartPassthrough(scope), XRErrors::OperationUnsupported);
            }
            SECTION("native layer capacity rejects the extra layer before acquisition") {
                f.census.maximumLayers = 1;
                RequireFailureIdentity(f.native.owner.StartPassthrough(scope), XRErrors::CapacityExceeded);
            }
            SECTION("platform denial does not create any native object") {
                f.passthrough.createResult = XR_ERROR_NOT_PERMITTED_PASSTHROUGH_FB;
                RequireFailureIdentity(f.native.owner.StartPassthrough(scope), XRErrors::PermissionDenied);
            }
            CHECK_FALSE(f.passthrough.liveFeature);
            CHECK(f.passthrough.featuresCreated == 0);
            CHECK(f.script.ends == 0);
        }

        TEST_CASE("Current native passthrough precedes the premultiplied projection on the exact released frame",
                  "[unit][xr][compositor][passthrough]") {
            if (!NativePassthroughExtensionRequest())
                SKIP("This test requires the Meta Quest provider.");
            const std::uint32_t maximumLayers = GENERATE(1U, 2U);
            CompositorFixture f{false, false, false, NativeReprojectionRoute::None, true, maximumLayers};
            f.Bind();
            const XRPassthroughScope scope{f.session, f.local, IdentityValue<XRPassthroughGrant>(1)};
            REQUIRE(f.native.owner.StartPassthrough(scope).HasValue());
            CHECK(f.passthrough.running);
            auto differentGrant = scope;
            differentGrant.grant = IdentityValue<XRPassthroughGrant>(2);
            RequireFailureIdentity(f.native.owner.StartPassthrough(differentGrant), XRErrors::CapacityExceeded);

            const auto frame = f.Begin();
            const auto color = f.Acquire(frame);
            const std::array completed{color};
            const auto plan = f.Plan(frame, color);
            const std::array targets{NativeProjectionTarget{&f.color, color}};
            REQUIRE(f.frames.Submit(frame, completed) == XRFrameStatus::Ok);
            f.images.complete = true;
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, color) == XRFrameStatus::Ok);
            const NativePassthroughProjection receipt{
                .frame = frame, .scope = scope, .authorization = {nullptr, [](void *, const XRPassthroughScope &) { return true; }}};
            if (maximumLayers == 1) {
                RequireFailureIdentity(f.compositor.EndPassthroughProjection(frame, plan, targets, receipt), XRErrors::CapacityExceeded);
                CHECK(f.script.ends == 0);
                f.Empty(frame);
                return;
            }

            SECTION("correct receipt submits runtime underlay first and virtual alpha second") {
                REQUIRE(f.compositor.EndPassthroughProjection(frame, plan, targets, receipt).HasValue());
                CHECK(f.script.layers == 2);
                CHECK(f.script.passthroughLayer == NativeHandle<XrPassthroughLayerFB>(30));
                CHECK(f.script.projectionFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
                CHECK(f.frames.Snapshot().phase == XRFramePhase::Idle);
            }
            SECTION("consent withdrawn during native submission pauses the provider before returning") {
                f.script.onEnd = [&] {
                    f.native.owner.RevokePassthrough(scope);
                    CHECK(f.native.owner.ReadPassthrough(scope) == NativePassthroughState::Retiring);
                    CHECK(f.passthrough.running);
                };
                REQUIRE(f.compositor.EndPassthroughProjection(frame, plan, targets, receipt).HasValue());
                CHECK(f.frames.Snapshot().phase == XRFramePhase::Idle);
                CHECK_FALSE(f.passthrough.running);
                CHECK(f.passthrough.pauses == 1);
                REQUIRE(f.native.owner.StopPassthrough(scope).HasValue());
                CHECK(f.passthrough.pauses == 1);
                CHECK_FALSE(f.passthrough.liveFeature);
                RequireFailureIdentity(f.native.owner.StartPassthrough(scope), XRErrors::IdentityStale);
            }
            SECTION("foreign frame and World origin leave the transaction available for abort") {
                auto wrong = receipt;
                wrong.frame = {};
                RequireFailureIdentity(f.compositor.EndPassthroughProjection(frame, plan, targets, wrong), XRErrors::IdentityStale);
                wrong = receipt;
                wrong.scope.world.worldOriginRevision = IdentityValue<XRWorldOriginRevision>(2);
                RequireFailureIdentity(f.compositor.EndPassthroughProjection(frame, plan, targets, wrong), XRErrors::IdentityStale);
                CHECK(f.script.ends == 0);
                f.Empty(frame);
                CHECK(f.script.layers == 0);
            }
            SECTION("focus or provider loss withdraws the layer before another frame end") {
                EventScript events;

                struct ActiveEvents final {
                    explicit ActiveEvents(EventScript &script) {
                        EventScript::active = &script;
                    }

                    ~ActiveEvents() {
                        EventScript::active = nullptr;
                    }
                } active{events};

                events.session = f.native.script.lastSession;
                OpenXRSessionEvents owner{f.native.owner, f.sessions, f.frames};
                REQUIRE(owner.Bind({&EventScript::GetProc}, {}).HasValue());
                SECTION("native focus leaves even when session is still visible") {
                    events.Push(XR_SESSION_STATE_VISIBLE);
                }
                SECTION("native provider failure removes even a still-focused layer") {
                    events.passthroughState = XR_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT_FB;
                }
                REQUIRE(owner.Poll(true).HasValue());
                CHECK_FALSE(f.passthrough.running);
                CHECK(f.passthrough.pauses == 1);
                RequireFailureIdentity(f.compositor.EndPassthroughProjection(frame, plan, targets, receipt),
                                       XRErrors::OperationUnavailable);
                CHECK(f.script.ends == 0);
                f.Empty(frame);
            }
        }

        TEST_CASE("Passthrough revocation during the final native borrow pauses without submitting",
                  "[unit][xr][compositor][passthrough]") {
            if (!NativePassthroughExtensionRequest())
                SKIP("This test requires the Meta Quest provider.");
            CompositorFixture f{false, false, false, NativeReprojectionRoute::None, true, 2};
            f.Bind();
            const XRPassthroughScope scope{f.session, f.local, IdentityValue<XRPassthroughGrant>(1)};
            REQUIRE(f.native.owner.StartPassthrough(scope).HasValue());
            const auto frame = f.Begin();
            const auto color = f.Acquire(frame);
            const auto plan = f.Plan(frame, color);
            const std::array targets{NativeProjectionTarget{&f.color, color}};
            const std::array completed{color};
            REQUIRE(f.frames.Submit(frame, completed) == XRFrameStatus::Ok);
            f.images.complete = true;
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, color) == XRFrameStatus::Ok);
            bool permitted = true;
            const NativePassthroughProjection receipt{
                .frame = frame,
                .scope = scope,
                .authorization = {&permitted, [](void *context, const XRPassthroughScope &) { return *static_cast<const bool *>(context); }}};

            auto unrelated = scope;
            unrelated.grant = IdentityValue<XRPassthroughGrant>(2);
            const bool platform = GENERATE(false, true);
            bool withdrawn = false;
            f.native.fence.onValidate = [&f, &scope, &unrelated, &withdrawn, &permitted, platform] {
                // Only the final borrow holds the passthrough dispatch guard.
                const auto probe = f.native.owner.StopPassthrough(unrelated);
                REQUIRE(probe.HasError());
                if (probe.ErrorValue().code.Value() != XRErrors::OperationIncompatible.code.Value())
                    return;
                if (platform)
                    permitted = false;
                else
                    f.native.owner.RevokePassthrough(scope);
                withdrawn = true;
            };
            const auto ended = f.compositor.EndPassthroughProjection(frame, plan, targets, receipt);
            f.native.fence.onValidate = {};
            REQUIRE(withdrawn);
            RequireFailureIdentity(ended, platform ? XRErrors::PermissionDenied : XRErrors::OperationUnavailable);
            CHECK(f.script.ends == 0);
            CHECK(f.native.owner.ReadPassthrough(scope) == NativePassthroughState::Retiring);
            CHECK_FALSE(f.passthrough.running);
            CHECK(f.passthrough.pauses == 1);
            f.Empty(frame);
            REQUIRE(f.native.owner.StopPassthrough(scope).HasValue());
            CHECK(f.passthrough.pauses == 1);
            CHECK_FALSE(f.passthrough.liveFeature);
        }

        TEST_CASE("Passthrough withdrawal during provider startup stops the exact grant at the next boundary",
                  "[unit][xr][compositor][passthrough]") {
            if (!NativePassthroughExtensionRequest())
                SKIP("This test requires the Meta Quest provider.");
            CompositorFixture f{false, false, false, NativeReprojectionRoute::None, true, 2};
            const XRPassthroughScope scope{f.session, f.local, IdentityValue<XRPassthroughGrant>(1)};
            auto unrelated = scope;
            unrelated.grant = IdentityValue<XRPassthroughGrant>(2);
            const std::string_view withdrawnAt = GENERATE("feature", "layer", "start", "resume", "validation");
            f.passthrough.onStartup = [&](std::string_view phase) {
                CHECK(f.native.owner.ReadPassthrough(scope) == NativePassthroughState::Absent);
                f.native.owner.RevokePassthrough(unrelated);
                if (withdrawnAt == phase)
                    f.native.owner.RevokePassthrough(scope);
                if (withdrawnAt == "validation" && phase == "resume")
                    f.native.fence.onValidate = [&] {
                        f.native.owner.RevokePassthrough(scope);
                    };
            };
            RequireFailureIdentity(f.native.owner.StartPassthrough(scope), XRErrors::PermissionDenied);
            f.native.fence.onValidate = {};
            const bool reachedResume = withdrawnAt == "resume" || withdrawnAt == "validation";
            CHECK(f.passthrough.starts == (withdrawnAt == "start" || reachedResume ? 1U : 0U));
            CHECK(f.passthrough.resumes == (reachedResume ? 1U : 0U));
            CHECK(f.passthrough.pauses == f.passthrough.starts);
            CHECK_FALSE(f.passthrough.running);
            CHECK_FALSE(f.passthrough.liveLayer);
            CHECK_FALSE(f.passthrough.liveFeature);
            CHECK(f.native.owner.ReadPassthrough(scope) == NativePassthroughState::Absent);
            RequireFailureIdentity(f.native.owner.StartPassthrough(scope), XRErrors::IdentityStale);

            f.passthrough.onStartup = [&](std::string_view) {
                f.native.owner.RevokePassthrough(scope);
            };
            REQUIRE(f.native.owner.StartPassthrough(unrelated).HasValue());
            CHECK(f.native.owner.ReadPassthrough(unrelated) == NativePassthroughState::Active);
            REQUIRE(f.native.owner.StopPassthrough(unrelated).HasValue());
        }

        TEST_CASE("Passthrough rollback redacts immediately and retains exact teardown debt", "[unit][xr][compositor][passthrough]") {
            if (!NativePassthroughExtensionRequest())
                SKIP("This test requires the Meta Quest provider.");
            CompositorFixture f{false, false, false, NativeReprojectionRoute::None, true, 2};
            const XRPassthroughScope scope{f.session, f.local, IdentityValue<XRPassthroughGrant>(1)};
            f.passthrough.resumeResult = XR_ERROR_NOT_PERMITTED_PASSTHROUGH_FB;
            const bool cleanupFails = GENERATE(false, true);
            f.passthrough.failLayerDestroy = cleanupFails;
            const auto denied = f.native.owner.StartPassthrough(scope);
            f.passthrough.failLayerDestroy = false;
            RequireFailureIdentity(denied, cleanupFails ? XRErrors::OperationUnavailable : XRErrors::PermissionDenied);
            CHECK(ErrorChainContains(denied.ErrorValue(), XRErrors::PermissionDenied.domain, XRErrors::PermissionDenied.code));
            CHECK(f.passthrough.pauses == 1);
            CHECK_FALSE(f.passthrough.running);
            CHECK(f.passthrough.liveLayer == cleanupFails);
            CHECK(f.passthrough.liveFeature == cleanupFails);
            if (cleanupFails) {
                REQUIRE(f.native.owner.StopPassthrough(scope).HasValue());
                CHECK_FALSE(f.passthrough.liveLayer);
                CHECK_FALSE(f.passthrough.liveFeature);
            }
            CHECK(f.passthrough.layersDestroyed == 1);
            RequireFailureIdentity(f.native.owner.StartPassthrough(scope), XRErrors::IdentityStale);
        }

        TEST_CASE("Lost XR frame retains image and native destruction debt before explicit quiescence reset",
                  "[unit][xr][compositor][retirement]") {
            CompositorFixture f;
            f.Bind();
            const auto frame = f.Begin();
            const auto image = f.Acquire(frame);
            const std::array images{image};
            REQUIRE(f.frames.Submit(frame, images) == XRFrameStatus::Ok);
            REQUIRE(f.sessions.ApplyEvent(f.session, XRSessionEvent::SessionLost).HasValue());
            REQUIRE(f.compositor.BeginShutdown().HasValue());
            CHECK(f.frames.Abort(frame) != XRFrameStatus::Ok);
            CHECK(f.frames.Release(frame, image) != XRFrameStatus::Ok);
            CHECK(f.compositor.EndEmpty(frame).HasError());
            CHECK(f.script.ends == 0);

            auto closed = f.color.Close();
            REQUIRE(closed.HasValue());
            CHECK_FALSE(closed.Value());
            CHECK(f.swapScript.releases == 0);
            CHECK(f.swapScript.destroys == 0);
            CHECK(f.frames.Snapshot().frame == frame);
            CHECK(f.frames.Snapshot().phase != XRFramePhase::Idle);

            f.images.complete = true;  // Scripted exact image/scene/GUI completion, never a loss-event receipt.
            f.swapScript.fail = "release";
            f.swapScript.failure = XR_ERROR_SESSION_LOST;
            CHECK(f.color.Close().HasError());
            CHECK(f.frames.Snapshot().phase != XRFramePhase::Idle);
            f.swapScript.fail = {};
            closed = f.color.Close();
            REQUIRE(closed.HasValue());
            REQUIRE(closed.Value());
            CHECK(f.swapScript.releases == 0);  // Loss did not invent a successful xrReleaseSwapchainImage.
            CHECK(f.swapScript.destroys == 1);
            CHECK(f.color.BorrowReleased(image).HasError());

            f.native.script.failRetirement = true;
            CHECK(f.native.owner.RetireSession().HasError());
            CHECK(f.native.script.liveSessions == 1);
            CHECK(f.frames.Snapshot().frame == frame);
            CHECK(f.compositor.Wait().HasError());
            CHECK(f.script.ends == 0);
            f.native.script.failRetirement = false;
            REQUIRE(f.native.owner.RetireSession().HasValue());
            REQUIRE(f.native.script.liveSessions == 0);
            REQUIRE(f.compositor.ResetAfterQuiescence().HasValue());
            f.frames.ResetAfterQuiescence();
            CHECK(f.frames.Snapshot().phase == XRFramePhase::Idle);
            CHECK(f.sessions.Snapshot().state == XRSessionState::Lost);
            CHECK(f.sessions.ApplyEvent(f.session, XRSessionEvent::Ready).HasError());
            CHECK(f.script.ends == 0);
            CHECK(f.native.script.liveInstances == 1);
        }

        TEST_CASE("Optional native depth timeout keeps color projection and retains exact cleanup debt",
                  "[unit][xr][reprojection][compositor]") {
            CompositorFixture f{false, true, true};
            f.CreateSynthesisPair();
            f.Bind();
            OpenXRReprojectionFrameImages pair{f.frames, f.motionOwner, f.synthesisDepthOwner};
            const auto frame = f.Begin();
            const auto color = f.Acquire(frame);
            const std::array completed{color};
            const auto plan = f.Plan(frame, color);
            const std::array targets{NativeProjectionTarget{&f.color, color}};
            f.swapScript.timeoutSwapchain = NativeHandle<XrSwapchain>(f.swapScript.creates);
            const auto candidate = pair.Prepare(frame, completed);
            REQUIRE(candidate.HasValue());
            CHECK_FALSE(candidate.Value());
            CHECK(f.frames.Snapshot().acquiredImages == 1);
            CHECK(f.motionOwner.Phase() == NativeSwapchainPhase::Imported);
            CHECK(f.synthesisDepthOwner.Phase() == NativeSwapchainPhase::Acquired);
            REQUIRE(f.frames.Submit(frame, completed) == XRFrameStatus::Ok);
            f.images.complete = true;
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, color) == XRFrameStatus::Ok);
            REQUIRE(f.compositor.EndProjection(frame, plan, targets).HasValue());
            CHECK(f.script.layers == 1);

            f.motionImages.complete = true;
            REQUIRE_FALSE(pair.PollRetirement().Value());
            CHECK(f.swapScript.releases == 2);
            CHECK(f.synthesisDepthOwner.Phase() == NativeSwapchainPhase::Acquired);
            const auto next = f.Begin();
            const auto newColor = f.Acquire(next);
            const std::array nextImages{newColor};
            const auto blocked = pair.Prepare(next, nextImages);
            REQUIRE(blocked.HasValue());
            CHECK_FALSE(blocked.Value());
            const auto nextPlan = f.Plan(next, newColor);
            const std::array nextTargets{NativeProjectionTarget{&f.color, newColor}};
            f.swapScript.timeoutSwapchain = XR_NULL_HANDLE;
            REQUIRE(pair.PollRetirement().Value());
            CHECK(f.swapScript.releases == 3);
            CHECK(f.frames.Snapshot().releasedImages == 0);
            REQUIRE(f.frames.Submit(next, nextImages) == XRFrameStatus::Ok);
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(next, newColor) == XRFrameStatus::Ok);
            REQUIRE(f.compositor.EndProjection(next, nextPlan, nextTargets).HasValue());
            CHECK(f.script.ends == 2);
        }

        TEST_CASE("Same-frame auxiliary pair registers atomically and color fallback waits only for registered release",
                  "[unit][xr][reprojection][compositor]") {
            const bool omit = GENERATE(false, true);
            CompositorFixture f{false, true, true};
            f.CreateSynthesisPair();
            f.Bind();
            OpenXRReprojectionFrameImages pair{f.frames, f.motionOwner, f.synthesisDepthOwner};
            const auto frame = f.Begin();
            const auto color = f.Acquire(frame);
            const std::array completed{color};
            const auto plan = f.Plan(frame, color);
            const std::array targets{NativeProjectionTarget{&f.color, color}};
            const auto result = pair.Prepare(frame, completed);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value());
            const auto &candidate = *result.Value();
            CHECK(candidate.frame == frame);
            CHECK(candidate.motion.evidence.kind == XRReprojectionImageKind::Motion);
            CHECK(candidate.depth.evidence.kind == XRReprojectionImageKind::Depth);
            CHECK(f.frames.Snapshot().acquiredImages == 1);
            auto wrongFrame = frame;
            ++wrongFrame.sequence;
            CHECK(pair.Submit(wrongFrame, completed).HasError());
            CHECK(pair.Submit(frame, {}).HasError());
            CHECK(f.frames.Snapshot().acquiredImages == 1);
            CHECK((f.frames.ValidateReprojectionSubmission(frame, completed, candidate.motion.evidence, candidate.motion.evidence) !=
                   XRFrameStatus::Ok));

            if (omit) {
                REQUIRE(pair.Omit(frame).HasValue());
                REQUIRE(f.frames.Submit(frame, completed) == XRFrameStatus::Ok);
                CHECK(f.frames.Snapshot().acquiredImages == 1);
            } else {
                REQUIRE(pair.Submit(frame, completed).HasValue());
                CHECK(f.frames.Snapshot().acquiredImages == 3);
                CHECK(pair.Omit(frame).HasError());
            }
            f.images.complete = true;
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, color) == XRFrameStatus::Ok);
            f.motionImages.complete = true;
            REQUIRE_FALSE(pair.PollRetirement().Value());
            CHECK(f.swapScript.releases == 2);
            CHECK(f.frames.Snapshot().releasedImages == (omit ? 1 : 2));
            CHECK(f.compositor.EndProjection(frame, plan, targets).HasValue() == omit);
            f.synthesisDepthImages.complete = true;
            REQUIRE(pair.PollRetirement().Value());
            CHECK(f.swapScript.releases == 3);
            if (!omit)
                REQUIRE(f.compositor.EndProjection(frame, plan, targets).HasValue());
            CHECK(f.script.layers == 1);
        }

        TEST_CASE("OpenXR synthesis provider requires the accepted renderer policy and exact native attempt",
                  "[unit][xr][compositor][reprojection]") {
            CompositorFixture fallback{false, true};
            CHECK(fallback.native.owner.RegisterReprojectionProvider(fallback.session, fallback.native.preflight.Attempt()).HasError());
            CHECK(fallback.native.owner.RegisterReprojectionProvider(fallback.session, {}).HasError());
        }

        TEST_CASE("OpenXR ends the frame once with typed native synthesis chains or baseline color",
                  "[unit][xr][compositor][reprojection]") {
            enum class Handoff {
                Exact,
                Revoked,
                LateRevoke,
                ForeignOwner,
                UnavailableCoverage,
                UnresolvedSample,
                WrongExtent,
                ExplicitApplicationDelta,
                MissingCurrentPeriod,
                ShortPreviousPeriod,
                MissingPreviousReference,
                ReplacedReference,
                MovedReference,
                ContradictoryDelta,
                NativeFailure
            };
            const auto handoff =
                GENERATE(Handoff::Exact, Handoff::Revoked, Handoff::LateRevoke, Handoff::ForeignOwner, Handoff::UnavailableCoverage,
                         Handoff::UnresolvedSample, Handoff::WrongExtent, Handoff::ExplicitApplicationDelta, Handoff::MissingCurrentPeriod,
                         Handoff::ShortPreviousPeriod, Handoff::MissingPreviousReference, Handoff::ReplacedReference,
                         Handoff::MovedReference, Handoff::ContradictoryDelta, Handoff::NativeFailure);
            const auto route = GENERATE(NativeReprojectionRoute::FrameSynthesisExt, NativeReprojectionRoute::SpaceWarpFb);
            const bool explicitDelta = handoff == Handoff::ExplicitApplicationDelta;
            const auto cadence =
                explicitDelta ? XRReprojectionCadenceMode::FixedInterval : XRReprojectionCadenceMode::ObservedNativePeriods;
            CompositorFixture f{false, true, true, route, false, 1, cadence};
            const bool extension = route == NativeReprojectionRoute::FrameSynthesisExt;
            CHECK(f.synthesisExtents[0] == Render::FramebufferExtent{640, 512});
            CHECK(f.synthesisExtents[1] == Render::FramebufferExtent{extension ? 800U : 640U, 512});
            f.CreateSynthesisPair();
            f.Bind();
            auto provider = f.native.owner.RegisterReprojectionProvider(f.session, f.native.preflight.Attempt());
            REQUIRE(provider.HasValue());
            CHECK(f.native.owner.RegisterReprojectionProvider(f.session, f.native.preflight.Attempt()).HasError());
            OpenXRReprojectionFrameImages pair{f.frames, f.motionOwner, f.synthesisDepthOwner};

            const auto predictionPeriod = f.script.period;
            if (handoff == Handoff::ShortPreviousPeriod)
                f.script.period = 4'000'000;
            const auto prior = f.Begin();
            const auto priorColor = f.Acquire(prior);
            if (!explicitDelta && handoff != Handoff::MissingPreviousReference)
                f.RecordReference(prior);
            const std::array priorImages{priorColor};
            const auto priorPlan = f.Plan(prior, priorColor);
            f.images.complete = true;
            REQUIRE(f.frames.Submit(prior, priorImages) == XRFrameStatus::Ok);
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(prior, priorColor) == XRFrameStatus::Ok);
            const std::array priorTargets{NativeProjectionTarget{&f.color, priorColor}};
            REQUIRE(f.compositor.EndProjection(prior, priorPlan, priorTargets).HasValue());

            f.script.prediction += 5'000'000;
            f.script.period = handoff == Handoff::MissingCurrentPeriod ? 0 : predictionPeriod;
            const auto frame = f.Begin();
            const auto color = f.Acquire(frame);
            if (!explicitDelta)
                f.RecordReference(frame, handoff == Handoff::MovedReference ? Math::Vec3{0.25F, 0.0F, 0.0F} : Math::Vec3{},
                                  handoff == Handoff::ReplacedReference ? 2 : 1);
            const std::array images{color};
            const auto plan = f.Plan(frame, color);
            const std::array targets{NativeProjectionTarget{&f.color, color}};
            const auto prepared = pair.Prepare(frame, images);
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value());
            const auto &acquired = *prepared.Value();
            REQUIRE(pair.Submit(frame, images).HasValue());
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, color) == XRFrameStatus::Ok);
            f.motionImages.complete = true;
            f.synthesisDepthImages.complete = true;
            REQUIRE(pair.PollRetirement().Value());

            std::array<XRReprojectionViewInput, 2> eyes{};
            for (std::uint32_t eye = 0; eye < eyes.size(); ++eye) {
                const auto [width, height] = f.synthesisExtents[eye];
                const XRRenderRectangle rectangle{0, 0, width, height};
                eyes[eye] = {plan.Views()[eye].id,
                             {acquired.motion.evidence.image, rectangle, eye},
                             {acquired.depth.evidence.image, rectangle, eye},
                             0.25F,
                             0.75F,
                             0.2F,
                             300.0F};
            }
            if (handoff == Handoff::WrongExtent) {
                const auto wrongWidth = extension ? eyes.front().motion.rectangle.width : eyes.back().motion.rectangle.width - 1;
                eyes.back().motion.rectangle.width = wrongWidth;
                eyes.back().depth.rectangle.width = wrongWidth;
            }
            const float delta = explicitDelta || handoff == Handoff::ContradictoryDelta ? 0.125F : 0.0F;
            XRReprojectionInputs input{priorPlan.PredictedDisplayTime(), prior.sequence,
                                       XRReprojectionSpaceDelta{f.local, {delta, 0.0F, 0.0F}, Math::Quaternion::Identity()}, eyes};
            // These completed classification values are scripted with the other native fixture inputs.
            std::array<XRReprojectionViewCoverage, 2> reported{};
            for (std::size_t eye = 0; eye < eyes.size(); ++eye) {
                const auto &rect = eyes[eye].motion.rectangle;
                reported[eye] = {eyes[eye].view, eyes[eye].motion, eyes[eye].depth, std::uint64_t{rect.width} * rect.height, 0};
            }
            if (handoff != Handoff::UnavailableCoverage)
                input.coverage.emplace(XRReprojectionCoverage{.session = f.session,
                                                              .frameSequence = frame.sequence,
                                                              .prediction = plan.PredictedDisplayTime(),
                                                              .previousFrameSequence = prior.sequence,
                                                              .previousPrediction = priorPlan.PredictedDisplayTime(),
                                                              .views = reported});
            if (handoff == Handoff::UnresolvedSample)
                reported.back().unresolvedSamples = 1;
            auto expected = f.frames.ValidateReprojection(frame, plan, input);
            if (handoff == Handoff::UnavailableCoverage)
                CHECK(expected == XRReprojectionStatus::CoverageUnavailable);
            else if (handoff == Handoff::UnresolvedSample)
                CHECK(expected == XRReprojectionStatus::CoverageRejected);
            else if (handoff == Handoff::WrongExtent || handoff == Handoff::ContradictoryDelta)
                CHECK(expected == XRReprojectionStatus::InputsInvalid);
            else if (handoff == Handoff::MissingCurrentPeriod || handoff == Handoff::ShortPreviousPeriod ||
                     handoff == Handoff::MissingPreviousReference || handoff == Handoff::ReplacedReference ||
                     handoff == Handoff::MovedReference)
                CHECK(expected == XRReprojectionStatus::HistoryUnavailable);
            else
                REQUIRE(expected == XRReprojectionStatus::Ready);
            if (handoff == Handoff::Revoked)
                f.native.owner.RevokeReprojectionProvider(provider.Value());
            if (handoff == Handoff::LateRevoke) {
                f.spaces.providerOwner = &f.native.owner;
                f.spaces.revokeDuringBorrow = &provider.Value();
            }
            if (handoff == Handoff::NativeFailure)
                f.script.endResult = XR_ERROR_RUNTIME_FAILURE;
            const NativeReprojectionSubmission synthesis{provider.Value(), f.motionOwner,
                                                         handoff == Handoff::ForeignOwner ? f.motionOwner : f.synthesisDepthOwner, input};
            const auto actual = f.compositor.EndReprojectedProjection(frame, plan, targets, synthesis);
            CHECK(f.script.ends == 2);
            if (handoff == Handoff::NativeFailure) {
                CHECK(actual.HasError());
                CHECK(f.compositor.RetainedFault() != nullptr);
                CHECK(f.compositor.EndReprojectedProjection(frame, plan, targets, synthesis).HasError());
                CHECK(f.script.ends == 2);
                f.Recover();
            } else {
                REQUIRE(actual.HasValue());
                if (expected == XRReprojectionStatus::Ready && handoff != Handoff::Exact && !explicitDelta)
                    expected = XRReprojectionStatus::InputsInvalid;
                CHECK(actual.Value() == expected);
            }
            for (std::size_t eye = 0; eye < eyes.size(); ++eye) {
                const auto &ext = f.script.ext[eye];
                const auto &fb = f.script.fb[eye];
                const bool synthesized = handoff == Handoff::Exact || explicitDelta || handoff == Handoff::NativeFailure;
                if (!synthesized) {
                    CHECK(f.script.encoded[eye].next == nullptr);
                    continue;
                }
                if (extension) {
                    CHECK(ext.type == XR_TYPE_FRAME_SYNTHESIS_INFO_EXT);
                    CHECK(ext.next == nullptr);
                    CHECK(ext.motionVectorScale.x == 1.0F);
                    CHECK(ext.motionVectorScale.y == 1.0F);
                    CHECK(ext.motionVectorScale.z == 1.0F);
                    CHECK(ext.motionVectorOffset.z == 0.0F);
                    CHECK(fb.type == XR_TYPE_UNKNOWN);
                } else {
                    CHECK(fb.type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB);
                    CHECK(fb.next == nullptr);
                    CHECK(ext.type == XR_TYPE_UNKNOWN);
                }
                const auto motion = extension ? ext.motionVectorSubImage : fb.motionVectorSubImage;
                const auto depth = extension ? ext.depthSubImage : fb.depthSubImage;
                CHECK(motion.swapchain != depth.swapchain);
                CHECK(motion.swapchain != f.script.encoded[eye].subImage.swapchain);
                CHECK(motion.imageArrayIndex == eye);
                CHECK(depth.imageArrayIndex == eye);
                const auto [width, height] = f.synthesisExtents[eye];
                CHECK(motion.imageRect.extent.width == static_cast<std::int32_t>(width));
                CHECK(motion.imageRect.extent.height == static_cast<std::int32_t>(height));
                CHECK(depth.imageRect.extent.width == static_cast<std::int32_t>(width));
                CHECK(depth.imageRect.extent.height == static_cast<std::int32_t>(height));
                CHECK((extension ? ext.appSpaceDeltaPose.position.x : fb.appSpaceDeltaPose.position.x) == delta);
                CHECK((extension ? ext.minDepth : fb.minDepth) == 0.25F);
                CHECK((extension ? ext.maxDepth : fb.maxDepth) == 0.75F);
                CHECK((extension ? ext.nearZ : fb.nearZ) == 0.2F);
                CHECK((extension ? ext.farZ : fb.farZ) == 300.0F);
            }
        }

        TEST_CASE("Native XR compositor snapshots admitted pacing before loader callbacks", "[unit][xr][compositor][pacing]") {
            CompositorFixture f;
            NativeFramePacingPolicy pacing;
            f.script.mutatePacing = &pacing;
            REQUIRE(f.compositor.Bind({&FrameScript::GetProc}, pacing).HasValue());
            CHECK(pacing.maximumFramesInFlight == 2);
            const auto frame = f.Begin();
            const auto observation = f.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(8'500'000));
            REQUIRE(observation.HasValue());
            CHECK(observation.Value().cpuSubmissionDeadlineNanoseconds == 8'500'000);
            CHECK(observation.Value().deadline == NativeFrameDeadline::WithinBudget);
            f.Empty(frame);
        }

        TEST_CASE("Native XR adaptive resolution uses consecutive completed frames and resets for a new plan",
                  "[unit][xr][compositor][resolution]") {
            CompositorFixture f;
            f.Bind();
            const auto nativeFrame = f.Begin();
            auto frame = nativeFrame;
            NativeProjectionResolution resolution;
            const auto begin = [&](const XRFrameId &id, bool adaptive = true, std::uint64_t rendererRevision = 1) {
                return resolution.BeginFrame(id, adaptive, 1, rendererRevision);
            };
            const auto observe = [&resolution, &frame](const NativeFrameDeadline deadline, const std::int64_t gpuNanoseconds) {
                const NativeFramePacingObservation sample{.frame = frame, .predictedPeriodNanoseconds = 10'000'000, .deadline = deadline};
                resolution.ObserveCompletion(sample, std::chrono::nanoseconds{gpuNanoseconds});
                resolution.ObserveCompletion(sample, std::chrono::nanoseconds{gpuNanoseconds});
                CHECK_FALSE(resolution.AwaitsCompletion(frame));
                ++frame.sequence;
            };
            using enum NativeFrameDeadline;
            for (std::uint32_t index = 0; index < 2; ++index) {
                CHECK(begin(frame) == 100);
                observe(CpuSubmissionMissed, 9'000'000);
            }
            ++frame.sequence;
            CHECK(begin(frame) == 100);
            resolution.ObserveCompletion({.frame = frame, .predictedPeriodNanoseconds = 10'000'000, .deadline = DisplayMissed}, {});
            CHECK(resolution.AwaitsCompletion(frame));
            ++frame.sequence;
            CHECK(begin(frame) == 100);
            CHECK(begin({}) == 100);
            for (std::uint32_t index = 0; index < 3; ++index) {
                CHECK(begin(frame) == 100);
                observe(WithinBudget, 9'000'000);
            }
            auto last = frame;
            --last.sequence;
            CHECK(begin(last) == 100);
            CHECK(begin(frame) == 95);
            observe(DisplayMissed, 9'000'000);
            for (std::uint32_t index = 0; index < 12; ++index) {
                CHECK(begin(frame) == 95);
                observe(DisplayMissed, 1'000'000);
            }
            for (std::uint32_t index = 0; index < 12; ++index) {
                CHECK(begin(frame) == 95);
                observe(WithinBudget, index % 2 == 0 ? 7'000'000 : 9'000'000);
            }
            for (std::uint32_t index = 0; index < 24; ++index) {
                static_cast<void>(begin(frame));
                observe(DisplayMissed, 9'000'000);
            }
            CHECK(begin(frame) == 70);
            for (std::uint32_t index = 0; index < 10; ++index) {
                CHECK(begin(frame) == 70);
                observe(WithinBudget, 4'000'000);
            }
            CHECK(begin(frame) == 75);
            auto recovered = frame;
            ++recovered.session.slot.generation;
            recovered.configuration.session = recovered.session;
            CHECK(begin(recovered) == 100);
            resolution.ObserveCompletion({.frame = frame, .predictedPeriodNanoseconds = 10'000'000}, std::chrono::milliseconds{9});
            CHECK(resolution.AwaitsCompletion(recovered));
            CHECK(begin(frame, true, 2) == 100);
            auto replacement = frame;
            replacement.capabilityRevision = IdentityValue<XRCapabilityRevision>(frame.capabilityRevision.Value() + 1);
            ++replacement.sequence;
            CHECK(begin(replacement, true, 2) == 100);
            resolution.ObserveCompletion({.frame = frame, .predictedPeriodNanoseconds = 10'000'000, .deadline = DisplayMissed},
                                         std::chrono::milliseconds{9});
            CHECK(resolution.AwaitsCompletion(replacement));
            CHECK(begin(replacement, false, 2) == 100);
            CHECK_FALSE(resolution.AwaitsCompletion(replacement));
            CHECK(begin({}) == 100);
            f.Empty(nativeFrame);
        }

        TEST_CASE("Native XR projection submits scaled image rectangles inside the same allocation", "[unit][xr][compositor][resolution]") {
            CompositorFixture f;
            f.Bind();
            const auto frame = f.Begin();
            const auto image = f.Acquire(frame);
            const auto plan = f.Plan(frame, image, nullptr, false, 85);
            const auto rectangle = NativeProjectionResolution::Rectangle({800, 600}, 85);
            CHECK((rectangle == XRRenderRectangle{0, 0, 680, 510}));
            for (const auto &target : plan.Targets()) {
                CHECK((target.allocationExtent == Render::FramebufferExtent{800, 600}));
                CHECK(target.renderRectangle == rectangle);
                CHECK(target.image == image);
            }
            f.images.complete = true;
            const std::array completed{image};
            REQUIRE(f.frames.Submit(frame, completed) == XRFrameStatus::Ok);
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, image) == XRFrameStatus::Ok);
            const std::array owners{NativeProjectionTarget{&f.color, image}};
            REQUIRE(f.compositor.EndProjection(frame, plan, owners).HasValue());
            REQUIRE(f.script.views == 2);
            for (std::size_t eye = 0; eye < 2; ++eye) {
                const auto &subImage = f.script.encoded[eye].subImage;
                CHECK(subImage.imageRect.extent.width == 680);
                CHECK(subImage.imageRect.extent.height == 510);
                CHECK(subImage.imageArrayIndex == eye);
            }
        }

        TEST_CASE("Native OpenXR submits only complete valid quad projections", "[unit][xr][compositor][quad]") {
            enum class Submission {
                Valid,
                WrongOrder,
                OversizedInset,
                DisplacedInset
            };
            const auto submission =
                GENERATE(Submission::Valid, Submission::WrongOrder, Submission::OversizedInset, Submission::DisplacedInset);
            CompositorFixture f{XRFeatureProfile::FoveatedInsetProjection};
            REQUIRE(f.swapScript.created.arraySize == 4);
            f.Bind();
            const auto frame = f.Begin();
            const auto image = f.Acquire(frame);
            const auto prediction = f.frames.Snapshot().predictedDisplayTime;
            constexpr std::array roles{XRViewRole::PrimaryLeft, XRViewRole::PrimaryRight, XRViewRole::FoveatedInsetLeft,
                                       XRViewRole::FoveatedInsetRight};
            constexpr std::array layers{1U, 0U, 3U, 2U};
            std::array<XRViewDescriptor, roles.size()> views{};
            std::array<XRExternalRenderTargetDescriptor, roles.size()> targets{};
            for (std::uint32_t index = 0; index < views.size(); ++index) {
                const bool inset = index >= 2;
                views[index] = f.View(index, prediction);
                views[index].role = roles[index];
                if (inset)
                    views[index].projection = {-.3F, .3F, -.3F, .3F};
                targets[index] = {.view = views[index].id,
                                  .target = image.target,
                                  .image = image,
                                  .format = f.allocation.texture.format,
                                  .usage = f.allocation.texture.usage,
                                  .allocationExtent = f.allocation.texture.extent,
                                  .renderRectangle = inset ? XRRenderRectangle{20, 30, 400, 300} : XRRenderRectangle{0, 0, 800, 600},
                                  .arrayLayer = layers[index],
                                  .arrayLayerCount = f.allocation.texture.layerCount};
            }
            if (submission == Submission::WrongOrder)
                std::swap(views[0].role, views[2].role);
            if (submission == Submission::OversizedInset)
                views[2].projection = {-1.2F, .3F, -.3F, .3F};
            if (submission == Submission::DisplacedInset)
                views[3].pose.components.positionMeters.value = Math::Vec3{.1F, 1, 0};
            const std::array images{image};
            const auto revision = IdentityValue<XRViewConfigurationRevision>(1);
            const auto plan = XRViewRenderPlan::Create({.configuration = {.session = f.session,
                                                                          .id = frame.configuration,
                                                                          .revision = revision,
                                                                          .type = XRViewConfigurationType::QuadView,
                                                                          .viewCount = 4},
                                                        .predictedDisplayTime = prediction,
                                                        .views = views,
                                                        .targets = targets,
                                                        .acquiredImages = images},
                                                       f.session, f.configuration, revision, f.local.worldOriginRevision, 4,
                                                       {XRViewAdmissionMode::BoundedNView, 4});
            REQUIRE(plan.HasValue());
            const std::array owners{NativeProjectionTarget{&f.color, image}};
            CHECK(f.compositor.EndProjection(frame, plan.Value(), owners).HasError());
            CHECK(f.script.ends == 0);
            f.images.complete = true;
            REQUIRE(f.frames.Submit(frame, images) == XRFrameStatus::Ok);
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, image) == XRFrameStatus::Ok);
            const auto ended = f.compositor.EndProjection(frame, plan.Value(), owners);
            if (submission != Submission::Valid) {
                REQUIRE(ended.HasError());
                CHECK(ended.ErrorValue().code.Value() == MakeError(XRErrors::ViewPlanInvalid).code.Value());
                CHECK(f.script.ends == 0);
                f.Empty(frame);
                return;
            }
            REQUIRE(ended.HasValue());
            CHECK(f.script.ends == 1);
            CHECK(f.script.layers == 1);
            REQUIRE(f.script.views == views.size());
            for (std::size_t index = 0; index < views.size(); ++index) {
                const auto &subImage = f.script.encoded[index].subImage;
                CHECK(subImage.imageArrayIndex == layers[index]);
                CHECK(subImage.imageRect.offset.x == targets[index].renderRectangle.x);
                CHECK(subImage.imageRect.offset.y == targets[index].renderRectangle.y);
                CHECK(subImage.imageRect.extent.width == targets[index].renderRectangle.width);
                CHECK(subImage.imageRect.extent.height == targets[index].renderRectangle.height);
            }
        }

        TEST_CASE("Missed XR deadlines preserve pending image debt before zero-layer end and the next frame",
                  "[unit][xr][compositor][pacing]") {
            CompositorFixture f;
            f.Bind();
            const auto frame = f.Begin();
            const auto image = f.Acquire(frame);
            const auto now = GENERATE(9'000'000, 10'000'000);
            const auto pacing = f.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(now));
            REQUIRE(pacing.HasValue());
            CHECK(pacing.Value().deadline ==
                  (now < f.script.prediction ? NativeFrameDeadline::CpuSubmissionMissed : NativeFrameDeadline::DisplayMissed));
            REQUIRE(f.frames.Abort(frame) == XRFrameStatus::Ok);
            REQUIRE_FALSE(f.color.PollRelease().Value());
            CHECK(f.compositor.EndEmpty(frame).HasError());
            CHECK(f.compositor.Wait().HasError());
            CHECK(f.frames.Snapshot().frame == frame);
            CHECK(f.frames.Snapshot().phase == XRFramePhase::Aborting);
            CHECK(f.script.ends == 0);
            CHECK(f.swapScript.releases == 0);

            f.images.complete = true;
            REQUIRE(f.color.PollRelease().Value());
            REQUIRE(f.frames.Release(frame, image) == XRFrameStatus::Ok);
            REQUIRE(f.compositor.EndEmpty(frame).HasValue());
            CHECK(f.script.layers == 0);
            CHECK(f.script.ends == 1);
            CHECK(f.frames.Snapshot().phase == XRFramePhase::Idle);
            const auto next = f.Begin();
            CHECK(next != frame);
            CHECK(next.session == frame.session);
            CHECK(f.script.waits == 2);
            f.Empty(next);
        }

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
            const bool requestSpaceWarp = GENERATE(false, true);
            CompositorFixture f{false, requestSpaceWarp};
            if (requestSpaceWarp)
                REQUIRE(f.native.plan.Decision(XRCapability::SpaceWarp).state == XRFeatureDecisionState::BaselineProjection);
            f.Bind();
            const auto frame = f.Begin();
            const auto image = f.Acquire(frame);
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
            CHECK(f.script.encoded[0].next == nullptr);
            CHECK(f.script.encoded[1].next == nullptr);
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
            REQUIRE_FALSE(f.compositor.Bind({}, {}).HasValue());
            REQUIRE_FALSE(f.compositor.Bind({&FrameScript::GetProc}, {.gpuBudgetNanoseconds = 0}).HasValue());
            REQUIRE_FALSE(f.compositor.Bind({&FrameScript::GetProc}, {.maximumFramesInFlight = 2}).HasValue());
            REQUIRE_FALSE(f.compositor.Bind({&FrameScript::GetProc}, {}, NativeCompositorColorSpace::Rec709).HasValue());
            f.script.missingEnd = true;
            REQUIRE_FALSE(f.compositor.Bind({&FrameScript::GetProc}, {}).HasValue());
            REQUIRE_FALSE(f.compositor.Wait().HasValue());
            CHECK(f.script.waits == 0);
            f.script.missingEnd = false;
            f.Bind();
            const auto frame = f.Begin();
            f.Empty(frame);
        }

        TEST_CASE("Native XR logical replacement after wait fences native begin before dispatch", "[unit][xr][compositor][lifecycle]") {
            CompositorFixture f;
            f.Bind();
            const auto waited = f.compositor.Wait();
            REQUIRE(waited.HasValue());
            REQUIRE(waited.Value().status == XRFrameStatus::Ok);
            const auto replacement =
                f.sessions.Activate(f.native.capabilities, f.native.capabilities.System(), f.native.capabilities.Revision(), f.native.plan);
            REQUIRE(replacement.HasValue());
            REQUIRE_FALSE(f.compositor.Begin(waited.Value().frame).HasValue());
            CHECK(f.script.begins == 0);
            CHECK(f.frames.Snapshot().phase == XRFramePhase::Waited);
            f.Recover();
        }

        TEST_CASE("Native XR wait failure cancels only its reservation", "[unit][xr][compositor][lifecycle]") {
            CompositorFixture f;
            f.Bind();
            f.script.waitResult = XR_ERROR_RUNTIME_FAILURE;
            REQUIRE_FALSE(f.compositor.Wait().HasValue());
            CHECK(f.frames.Snapshot().phase == XRFramePhase::Idle);
            f.script.waitResult = XR_SUCCESS;
            const auto frame = f.Begin();
            f.Empty(frame);
        }

        TEST_CASE("Native XR unusable Horo prediction preserves exact native cleanup time", "[unit][xr][compositor][lifecycle]") {
            CompositorFixture f;
            f.Bind();
            f.script.prediction = GENERATE(XrTime{0}, XrTime{-10});
            const auto waited = f.compositor.Wait();
            REQUIRE(waited.HasValue());
            REQUIRE(waited.Value().status == XRFrameStatus::InvalidInput);
            REQUIRE(waited.Value().frame.IsValid());
            REQUIRE_FALSE(f.frames.Snapshot().renderAdmitted);
            const auto frame = waited.Value().frame;
            REQUIRE(f.compositor.Begin(frame).HasValue());
            CHECK(f.frames.LocateViews(frame, 2) == XRFrameStatus::Unsupported);
            REQUIRE_FALSE(f.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(1)).HasValue());
            SECTION("native cleanup succeeds") {
                REQUIRE(f.compositor.EndEmpty(frame).HasValue());
                CHECK(f.script.ends == 1);
                CHECK(f.script.layers == 0);
            }
            SECTION("failed native cleanup retains frame debt") {
                f.script.endResult = XR_ERROR_RUNTIME_FAILURE;
                REQUIRE_FALSE(f.compositor.EndEmpty(frame).HasValue());
                CHECK(f.frames.Snapshot().frame == frame);
                CHECK(f.frames.Snapshot().phase == XRFramePhase::Begun);
                REQUIRE_FALSE(f.compositor.EndEmpty(frame).HasValue());
                REQUIRE_FALSE(f.compositor.Wait().HasValue());
                CHECK(f.script.ends == 1);
                f.Recover();
            }
        }

        TEST_CASE("Native XR unusable period retains a real frame obligation", "[unit][xr][compositor][pacing]") {
            CompositorFixture f;
            f.Bind();
            f.script.period = 0;
            const auto frame = f.Begin();
            REQUIRE_FALSE(f.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(1)).HasValue());
            f.Empty(frame);
        }

        TEST_CASE("Native XR begin and end failure require explicit quiescent recovery", "[unit][xr][compositor][lifecycle]") {
            CompositorFixture f;
            f.Bind();
            SECTION("native begin failure") {
                const auto waited = f.compositor.Wait();
                REQUIRE(waited.HasValue());
                f.script.beginResult = XR_ERROR_RUNTIME_FAILURE;
                const auto failed = f.compositor.Begin(waited.Value().frame);
                REQUIRE(failed.HasError());
                REQUIRE(f.compositor.RetainedFault());
                CHECK(f.compositor.RetainedFault()->message == failed.ErrorValue().message);
                const auto repeated = f.compositor.Wait();
                REQUIRE(repeated.HasError());
                CHECK(repeated.ErrorValue().code.Value() == XRErrors::OperationUnavailable.code.Value());
                REQUIRE(repeated.ErrorValue().cause.Get());
                CHECK(repeated.ErrorValue().cause.Get()->message == failed.ErrorValue().message);
                const auto repeatedBegin = f.compositor.Begin(waited.Value().frame);
                REQUIRE(repeatedBegin.HasError());
                CHECK(repeatedBegin.ErrorValue().code.Value() == XRErrors::OperationUnavailable.code.Value());
                REQUIRE(repeatedBegin.ErrorValue().cause.Get());
                CHECK(repeatedBegin.ErrorValue().cause.Get()->message == failed.ErrorValue().message);
                CHECK(f.frames.Snapshot().phase == XRFramePhase::Waited);
                f.Recover();
            }
            SECTION("native end failure is not retried as successful") {
                const auto frame = f.Begin();
                REQUIRE(f.frames.Abort(frame) == XRFrameStatus::Ok);
                f.script.endResult = XR_ERROR_RUNTIME_FAILURE;
                const auto failed = f.compositor.EndEmpty(frame);
                REQUIRE(failed.HasError());
                REQUIRE(f.compositor.RetainedFault());
                CHECK(f.compositor.RetainedFault()->message == failed.ErrorValue().message);
                const auto repeated = f.compositor.EndEmpty(frame);
                REQUIRE(repeated.HasError());
                CHECK(repeated.ErrorValue().code.Value() == XRErrors::OperationUnavailable.code.Value());
                REQUIRE(repeated.ErrorValue().cause.Get());
                CHECK(repeated.ErrorValue().cause.Get()->message == failed.ErrorValue().message);
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
