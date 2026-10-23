#include "EditorPresentationSurface.h"
#include "Horo/Runtime/Render/RenderPresentationOutput.h"
#include "Horo/Runtime/Render/RenderSurfaceLifecycleErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Editor {
    namespace {
        constexpr Render::ResolvedPresentMode Mode{Render::PresentMode::Fifo, Render::PresentMode::Fifo,
                                                   Render::PresentModeResolution::Exact, 0};

        struct NativeSurface final : EditorSurfaceRealizer {
            EditorPresentationSurface *host{};
            unsigned calls{};
            bool fail{};
            bool pending{};
            bool foreignPending{};
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
                if (self.pending || self.foreignPending) {
                    auto error = MakeError(Render::RenderPresentationErrors::DrawableUnavailable);
                    if (self.foreignPending)
                        error.domain = ErrorDomainId{"test.foreign"};
                    return Result<void>::Failure(std::move(error));
                }
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

    TEST_CASE("Production surface retries exactly one frozen temporary realization", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        native.pending = true;
        const auto first = host.Observe(Render::FramebufferExtent{640, 480}, Mode, EditorSurfaceAdmission::Idle);
        REQUIRE(first.HasValue());
        CHECK_FALSE(first.Value().nativeFailure);
        CHECK(first.Value().snapshot.state == Render::RenderSurfaceState::Reconfiguring);
        const auto sequence = first.Value().snapshot.inFlightSequence;
        const auto second = host.Observe(Render::FramebufferExtent{640, 480}, Mode, EditorSurfaceAdmission::Idle);
        REQUIRE(second.HasValue());
        CHECK(second.Value().snapshot.inFlightSequence == sequence);
        CHECK(native.calls == 2);
        native.pending = false;
        const auto ready = host.Observe(Render::FramebufferExtent{640, 480}, Mode, EditorSurfaceAdmission::Idle);
        REQUIRE(ready.HasValue());
        CHECK(ready.Value().snapshot.state == Render::RenderSurfaceState::Ready);
        CHECK_FALSE(ready.Value().snapshot.inFlightSequence);
        CHECK(ready.Value().snapshot.surface.generation == 1);
    }

    TEST_CASE("Production surface cancels frozen resize before coalescing a newer extent", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        native.pending = true;
        const auto first = host.Observe(Render::FramebufferExtent{640, 480}, Mode, EditorSurfaceAdmission::Idle);
        REQUIRE(first.HasValue());
        const auto second = host.Observe(Render::FramebufferExtent{800, 600}, Mode, EditorSurfaceAdmission::Idle);
        REQUIRE(second.HasValue());
        CHECK_FALSE(second.Value().nativeFailure);
        CHECK(second.Value().snapshot.inFlightSequence != first.Value().snapshot.inFlightSequence);
        CHECK(native.last == Render::FramebufferExtent{800, 600});
        native.pending = false;
        const auto ready = host.Observe(Render::FramebufferExtent{800, 600}, Mode, EditorSurfaceAdmission::Idle);
        REQUIRE(ready.HasValue());
        CHECK(ready.Value().snapshot.active->extent == Render::FramebufferExtent{800, 600});
        CHECK(ready.Value().snapshot.surface.generation == 1);
    }

    TEST_CASE("Production surface minimize and close cancel pending logical work without deleting native debt", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        native.pending = true;
        REQUIRE(host.Observe(Render::FramebufferExtent{640, 480}, Mode, EditorSurfaceAdmission::Idle).HasValue());
        const auto minimized = host.Observe(std::nullopt, Mode, EditorSurfaceAdmission::Idle);
        REQUIRE(minimized.HasValue());
        CHECK_FALSE(minimized.Value().snapshot.inFlightSequence);
        CHECK(native.calls == 1);
        CHECK(native.nativeAlive);
        REQUIRE(host.Close(EditorSurfaceAdmission::Cancelled).HasValue());
        CHECK(host.Snapshot().Value().state == Render::RenderSurfaceState::Unattached);
        CHECK(native.nativeAlive);
    }

    TEST_CASE("Production surface rejects a temporary code from a different domain as native loss", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        native.foreignPending = true;
        const auto observed = host.Observe(Render::FramebufferExtent{640, 480}, Mode, EditorSurfaceAdmission::Idle);
        REQUIRE(observed.HasValue());
        REQUIRE(observed.Value().nativeFailure);
        CHECK(observed.Value().nativeFailure->domain.Value() == "test.foreign");
        CHECK(observed.Value().snapshot.state == Render::RenderSurfaceState::Lost);
        CHECK_FALSE(observed.Value().snapshot.inFlightSequence);
    }

    TEST_CASE("Production surface close cancels a pending native transition exactly once", "[editor][surface]") {
        NativeSurface native;
        auto host = Surface(native);
        native.host = &host;
        native.pending = true;
        REQUIRE(host.Update(Render::FramebufferExtent{640, 480}, Mode).HasValue());
        REQUIRE(host.Snapshot().Value().inFlightSequence);
        const auto closed = host.Close(EditorSurfaceAdmission::Cancelled);
        REQUIRE(closed.HasValue());
        CHECK(closed.Value().state == Render::RenderSurfaceState::Unattached);
        CHECK_FALSE(closed.Value().inFlightSequence);
        CHECK(native.calls == 1);
        CHECK(native.nativeAlive);
        CHECK(host.Close(EditorSurfaceAdmission::Cancelled).HasValue());
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
