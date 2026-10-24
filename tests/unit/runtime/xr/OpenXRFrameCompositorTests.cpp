#include "OpenXRFrameCompositorTestFixture.h"

namespace Horo::Tests::OpenXR {
    namespace {
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
            CompositorFixture f;
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
