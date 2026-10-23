#include "EditorFrameAdmission.h"
#include "EditorPresentationSurface.h"
#include "Horo/Runtime/Render/RenderSurfaceLifecycleErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Editor {
    TEST_CASE("Editor defers only occupied renderer frame capacity", "[editor][surface]") {
        const Error openGl{.code = ErrorCode{"render.opengl.frame_backpressure"}, .domain = ErrorDomainId{"horo.render.opengl"}};
        const Error metal{.code = ErrorCode{"render.metal.submission_busy"}, .domain = ErrorDomainId{"horo.render.metal"}};
        const Error synchronization{.code = ErrorCode{"render.opengl.synchronization_failed"},
                                    .domain = ErrorDomainId{"horo.render.opengl"}};
        const Error foreign{.code = ErrorCode{"render.opengl.frame_backpressure"}, .domain = ErrorDomainId{"horo.render.other"}};

        CHECK(IsDeferredFrameAdmission(openGl));
        CHECK(IsDeferredFrameAdmission(metal));
        CHECK_FALSE(IsDeferredFrameAdmission(WithCause(synchronization, openGl)));
        CHECK_FALSE(IsDeferredFrameAdmission(synchronization));
        CHECK_FALSE(IsDeferredFrameAdmission(foreign));
    }

    namespace {
        constexpr Render::ResolvedPresentMode Mode{Render::PresentMode::Fifo, Render::PresentMode::Fifo,
                                                   Render::PresentModeResolution::Exact, 0};

        struct NativeSurface final : EditorSurfaceRealizer {
            EditorPresentationSurface *host{};
            unsigned calls{};
            bool fail{};
            bool frozenBeforeNative{};
            bool nativeAlive{true};
            Render::FramebufferExtent last{};

            Result<void> Resize(const Render::FramebufferExtent extent) override {
                auto &self = *this;
                ++self.calls;
                self.last = extent;
                const auto publication = self.host->Snapshot();
                self.frozenBeforeNative = publication.HasValue() &&
                                          publication.Value().state == Render::RenderSurfaceState::Reconfiguring &&
                                          publication.Value().inFlightSequence.has_value();
                if (self.fail)
                    return Result<void>::Failure(
                        WithCause(MakeError(Render::RenderSurfaceLifecycleErrors::InvalidOutcome, "Injected native activation failure."),
                                  MakeError(Render::RenderSurfaceLifecycleErrors::InvalidCommand, "Injected native cause.")));
                return Result<void>::Success();
            }
        };

        EditorPresentationSurface Surface(NativeSurface &native) {
            auto result = EditorPresentationSurface::Create(42, native);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

    TEST_CASE("Production host surface freezes attach and coalesces unchanged native observations", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        const auto ready = host.Update(Render::FramebufferExtent{640, 480}, Mode);
        REQUIRE(ready.HasValue());
        CHECK(ready.Value().state == Render::RenderSurfaceState::Ready);
        CHECK(ready.Value().active->extent == native.last);
        CHECK(native.frozenBeforeNative);
        CHECK(native.calls == 1);
        CHECK(host.Update(Render::FramebufferExtent{640, 480}, Mode).Value() == ready.Value());
        CHECK(native.calls == 1);
    }

    TEST_CASE("Production host surface admits initial zero size and suspend resume without native work while suspended",
              "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        const auto initial = host.Update(std::nullopt, Mode);
        REQUIRE(initial.HasValue());
        CHECK(initial.Value().state == Render::RenderSurfaceState::Suspended);
        CHECK_FALSE(initial.Value().active);
        CHECK(native.calls == 0);
        REQUIRE(host.Update(Render::FramebufferExtent{640, 480}, Mode).HasValue());
        const auto suspended = host.Update(std::nullopt, Mode);
        REQUIRE(suspended.HasValue());
        CHECK(suspended.Value().state == Render::RenderSurfaceState::Suspended);
        CHECK(suspended.Value().active->extent == Render::FramebufferExtent{640, 480});
        CHECK(native.calls == 1);
        CHECK(host.Update(std::nullopt, Mode).Value() == suspended.Value());
        REQUIRE(host.Update(Render::FramebufferExtent{800, 600}, Mode).HasValue());
        CHECK(native.calls == 2);
        CHECK(native.last == Render::FramebufferExtent{800, 600});
    }

    TEST_CASE("Production host native resize failure publishes Lost and explicit recovery retains native owner", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        REQUIRE(host.Update(Render::FramebufferExtent{640, 480}, Mode).HasValue());
        const auto oldGeneration = host.Snapshot().Value().surface.generation;
        native.fail = true;
        const auto failure = host.Update(Render::FramebufferExtent{800, 600}, Mode);
        REQUIRE(failure.HasError());
        CHECK(failure.ErrorValue().message == "Injected native activation failure.");
        CHECK(host.Snapshot().Value().state == Render::RenderSurfaceState::Lost);
        CHECK_FALSE(host.Snapshot().Value().active);
        CHECK_FALSE(host.Snapshot().Value().inFlightSequence);
        CHECK(host.Update(Render::FramebufferExtent{800, 600},
                          {Render::PresentMode::Immediate, Render::PresentMode::Immediate, Render::PresentModeResolution::Exact, 0})
                  .HasError());
        REQUIRE(host.Update(std::nullopt, Mode).HasValue());
        CHECK(host.Snapshot().Value().state == Render::RenderSurfaceState::Lost);
        native.fail = false;
        const auto recovered = host.Update(Render::FramebufferExtent{800, 600}, Mode);
        REQUIRE(recovered.HasValue());
        CHECK(recovered.Value().state == Render::RenderSurfaceState::Ready);
        CHECK(recovered.Value().surface.generation > oldGeneration);
        CHECK(native.nativeAlive);
        CHECK(native.calls == 3);
    }

    TEST_CASE("Production host surface closes before resource destruction and rejects later work", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        REQUIRE(host.Update(Render::FramebufferExtent{640, 480}, Mode).HasValue());
        const auto closed = host.Close();
        REQUIRE(closed.HasValue());
        CHECK(closed.Value().state == Render::RenderSurfaceState::Unattached);
        CHECK(native.nativeAlive);
        native.nativeAlive = false;  // Host destroys the backend only after Close completes.
        CHECK(host.Close().Value() == closed.Value());
        CHECK(host.Update(Render::FramebufferExtent{800, 600}, Mode).HasError());
        CHECK(native.calls == 1);
    }

    TEST_CASE("Production host surface rejects wrong thread cancellation and active frame before native mutation", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        REQUIRE(host.Update(Render::FramebufferExtent{640, 480}, Mode).HasValue());
        const auto before = host.Snapshot().Value();
        for (const auto admission : {EditorSurfaceAdmission::FrameActive, EditorSurfaceAdmission::Cancelled}) {
            CHECK(host.Update(Render::FramebufferExtent{800, 600}, Mode, admission).HasError());
            CHECK(host.Snapshot().Value() == before);
        }
        CHECK(host.Close(EditorSurfaceAdmission::FrameActive).HasError());
        CHECK(host.Snapshot().Value() == before);
        bool rejectedUpdate = false;
        bool rejectedClose = false;
        std::jthread wrongThread([&host, &rejectedUpdate, &rejectedClose] {
            rejectedUpdate = host.Update(Render::FramebufferExtent{800, 600}, Mode).HasError();
            rejectedClose = host.Close().HasError();
        });
        wrongThread.join();
        CHECK(rejectedUpdate);
        CHECK(rejectedClose);
        CHECK(host.Snapshot().Value() == before);
        CHECK(native.calls == 1);
        CHECK(host.Close(EditorSurfaceAdmission::Cancelled).HasValue());
    }

    TEST_CASE("Production host rejects invalid extent and unnegotiated present change without invoking native resize",
              "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        CHECK(host.Update(Render::FramebufferExtent{0, 480}, Mode).HasError());
        CHECK(native.calls == 0);
        REQUIRE(host.Update(Render::FramebufferExtent{640, 480}, Mode).HasValue());
        const auto before = host.Snapshot().Value();
        CHECK(host.Update(Render::FramebufferExtent{640, 480},
                          {Render::PresentMode::Immediate, Render::PresentMode::Immediate, Render::PresentModeResolution::Exact, 0})
                  .HasError());
        CHECK(host.Snapshot().Value() == before);
        CHECK(native.calls == 1);
    }

    TEST_CASE("Production host observes typed native loss and waits for an explicit retained-owner recovery", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        REQUIRE(host.Observe(Render::FramebufferExtent{64, 32}, Mode, EditorSurfaceAdmission::Idle).HasValue());
        native.fail = true;
        const auto lost = host.Observe(Render::FramebufferExtent{96, 48}, Mode, EditorSurfaceAdmission::Idle);
        REQUIRE(lost.HasValue());
        REQUIRE(lost.Value().nativeFailure);
        CHECK(lost.Value().nativeFailure->domain.Value() == Render::RenderSurfaceLifecycleErrors::InvalidOutcome.domain.Value());
        CHECK(lost.Value().nativeFailure->code.Value() == Render::RenderSurfaceLifecycleErrors::InvalidOutcome.code.Value());
        CHECK(lost.Value().nativeFailure->message == "Injected native activation failure.");
        REQUIRE(lost.Value().nativeFailure->cause.Get());
        CHECK(lost.Value().nativeFailure->cause.Get()->message == "Injected native cause.");
        CHECK(lost.Value().snapshot.state == Render::RenderSurfaceState::Lost);
        CHECK_FALSE(lost.Value().snapshot.active);
        const auto calls = native.calls;
        for (unsigned frame = 0; frame < 4; ++frame) {
            const auto deferred = host.Observe(Render::FramebufferExtent{96, 48}, Mode, EditorSurfaceAdmission::Idle);
            REQUIRE(deferred.HasValue());
            CHECK(deferred.Value().snapshot == lost.Value().snapshot);
            CHECK_FALSE(deferred.Value().nativeFailure);
        }
        CHECK(native.calls == calls);
        const auto retryFailed =
            host.Observe(Render::FramebufferExtent{96, 48}, Mode, EditorSurfaceAdmission::Idle, EditorSurfaceRecovery::Retry);
        REQUIRE(retryFailed.HasValue());
        REQUIRE(retryFailed.Value().nativeFailure);
        REQUIRE(retryFailed.Value().nativeFailure->cause.Get());
        CHECK(retryFailed.Value().nativeFailure->cause.Get()->message == "Injected native cause.");
        CHECK(retryFailed.Value().snapshot.state == Render::RenderSurfaceState::Lost);
        CHECK_FALSE(retryFailed.Value().snapshot.active);
        CHECK(native.calls == calls + 1);
        native.fail = false;
        const auto recovered =
            host.Observe(Render::FramebufferExtent{96, 48}, Mode, EditorSurfaceAdmission::Idle, EditorSurfaceRecovery::Retry);
        REQUIRE(recovered.HasValue());
        CHECK_FALSE(recovered.Value().nativeFailure);
        REQUIRE(recovered.Value().snapshot.active);
        CHECK((recovered.Value().snapshot.active->extent == Render::FramebufferExtent{96, 48}));
        CHECK(recovered.Value().snapshot.state == Render::RenderSurfaceState::Ready);
        CHECK(native.calls == calls + 2);
        CHECK(native.nativeAlive);
        CHECK(native.frozenBeforeNative);
    }

    TEST_CASE("Lost surface recovery preserves cancellation frame and owner admission errors", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        native.fail = true;
        REQUIRE(host.Observe(Render::FramebufferExtent{64, 32}, Mode, EditorSurfaceAdmission::Idle).Value().nativeFailure);
        const auto lost = host.Snapshot().Value();
        const auto calls = native.calls;
        for (const auto admission : {EditorSurfaceAdmission::Cancelled, EditorSurfaceAdmission::FrameActive}) {
            CHECK(host.Observe(Render::FramebufferExtent{64, 32}, Mode, admission, EditorSurfaceRecovery::Retry).HasError());
        }
        bool rejected = false;
        std::jthread foreign([&host, &rejected] {
            rejected = host.Observe(Render::FramebufferExtent{64, 32}, Mode, EditorSurfaceAdmission::Idle, EditorSurfaceRecovery::Retry)
                           .HasError();
        });
        foreign.join();
        CHECK(rejected);
        CHECK(native.calls == calls);
        CHECK(host.Snapshot().Value() == lost);
        REQUIRE(host.Close(EditorSurfaceAdmission::Cancelled).HasValue());
        CHECK(host.Observe(Render::FramebufferExtent{64, 32}, Mode, EditorSurfaceAdmission::Idle, EditorSurfaceRecovery::Retry).HasError());
        CHECK(native.calls == calls);
    }
}  // namespace Horo::Editor
