#include "QualificationHost.h"

#include "Horo/Platform/Internal/AndroidLifecycleAdapter.h"
#include "Horo/Platform/Internal/AndroidWindowHost.h"
#include "Horo/Runtime/Render/Internal/VulkanAndroidBackendModule.h"
#include "PresentationAdmission.h"

#include <array>
#include <exception>
#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <limits>
#include <thread>

namespace Horo::AndroidQualification {
    using namespace Horo::Platform;
    using namespace Horo::Platform::Android;
    using namespace Horo::Render;

    namespace {
        /** @brief Skips only exact native backpressure; success here is not a presented-frame receipt. */
        Result<void> RetryOrFailure(Result<void> outcome) {
            if (outcome.HasError() && IsVulkanAndroidBackpressure(outcome.ErrorValue()))
                return Result<void>::Success();
            return outcome;
        }

        const ErrorCodeDescriptor
            FrameRequestExhausted{.domain = ErrorDomainId{"horo.android.qualification"},
                                  .code = ErrorCode{"android.qualification.frame_request_exhausted"},
                                  .defaultSeverity = ErrorSeverity::Error,
                                  .summary = "Android qualification frame request sequence exhausted.",
                                  .remediationHint = "Restart the explicit qualification host before issuing another frame request."};
    }  // namespace

    struct QualificationHost::State final {
        std::unique_ptr<AndroidLifecycleController> lifecycle;
        std::unique_ptr<AndroidLifecycleAdapter> adapter;
        std::unique_ptr<AndroidWindowHost> windows;
        std::unique_ptr<PresentationAdmission> presentation;
        RenderBackendRegistry registry;
        std::unique_ptr<IRenderBackend> backend;
        AndroidActivityGeneration activity;
        AndroidWindowGeneration window;
        std::uint64_t frame{}; /**< Monotonic attempted request identity, never a presented-frame counter. */
        bool closing{}, drained{};

        /** @brief Commits the one canonical lifecycle cutoff before publishing native window facts. */
        Result<void> Drain() {
            const auto committed = lifecycle->Drain();
            if (committed.HasError())
                return Result<void>::Failure(committed.ErrorValue());
            return windows->Refresh();
        }

        /** @brief Initializes actual native device policy while the window owner remains empty. */
        Result<void> ActivateRenderer() {
            if (auto registered = RegisterVulkanAndroidRenderBackend(registry, *windows); registered.HasError())
                return registered;
            if (auto sealed = registry.Seal(); sealed.HasError())
                return sealed;
            auto created = registry.Create(RenderBackendId{"vulkan-android"});
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            backend = std::move(created).Value();
            return backend->Initialize(
                {.requirePresentation = true, .enableValidation = false, .maxFramesInFlight = 2, .presentMode = PresentMode::Fifo});
        }

        /** @brief Maps actual Activity callback timing into the existing generation-checked state machine. */
        Result<void> ActivityCommand(std::int32_t command) {
            switch (command) {
                case APP_CMD_START:
                    return adapter->OnActivityStarted(activity);
                case APP_CMD_RESUME:
                    return adapter->OnActivityResumed(activity);
                case APP_CMD_PAUSE:
                    return adapter->OnActivityPaused(activity);
                case APP_CMD_STOP:
                    return adapter->OnActivityStopped(activity);
                default:
                    return Result<void>::Success();
            }
        }

        /** @brief Commits retained native-window occurrences only after the Activity identity exists. */
        Result<void> WindowCommand(android_app &app, std::int32_t command) {
            if (command == APP_CMD_INIT_WINDOW) {
                auto admitted = windows->ObserveAvailable(activity, app.window);
                if (admitted.HasError())
                    return Result<void>::Failure(admitted.ErrorValue());
                window = admitted.Value();
            } else if (command == APP_CMD_TERM_WINDOW && window.value != 0) {
                if (auto lost = windows->ObserveLost(window); lost.HasError())
                    return lost;
                window = {};
            }
            return Result<void>::Success();
        }

        /** @brief Stops canonical host admission before beginning the selected backend's native drain. */
        Result<void> BeginShutdown() {
            if (closing)
                return Result<void>::Success();
            if (auto final = adapter->OnFinalShutdown(); final.HasError())
                return final;
            if (auto committed = Drain(); committed.HasError())
                return committed;
            if (auto closed = backend->BeginShutdown(); closed.HasError())
                return closed;
            closing = true;
            return Result<void>::Success();
        }

        /** @brief Encodes one admitted native clear; exact nonblocking backpressure skips work, never counts as a presented frame. */
        Result<void> Render(FramebufferExtent extent) {
            if (frame == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(FrameRequestExhausted));
            auto begun = backend->BeginFrame({++frame, extent});
            if (begun.HasError())
                return RetryOrFailure(Result<void>::Failure(begun.ErrorValue()));
            const auto token = begun.Value();

            struct AbortGuard {
                IRenderBackend &backend;
                FrameToken token;
                bool committed{};

                ~AbortGuard() {
                    if (!committed)
                        backend.AbortFrame(token);
                }
            } guard{*backend, token};

            const std::array passes{RenderPassDescriptor{.id = {1},
                                                         .kind = RenderPassKind::Graphics,
                                                         .primaryOutput = PrimaryOutputAttachment{
                                                             .clearColor = {.red = .08F, .green = .16F, .blue = .24F, .alpha = 1.F}}}};
            if (auto executed = backend->Execute({token, passes}); executed.HasError())
                return RetryOrFailure(std::move(executed));
            auto presented = backend->Present(token);
            guard.committed = presented.HasValue();
            return RetryOrFailure(std::move(presented));
        }
    };

    /** @copydoc QualificationHost::QualificationHost */
    QualificationHost::QualificationHost(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc QualificationHost::~QualificationHost */
    QualificationHost::~QualificationHost() {
        if (state_->backend && !state_->drained)
            std::terminate();
    }

    /** @copydoc QualificationHost::Create */
    Result<std::unique_ptr<QualificationHost>> QualificationHost::Create() {
        using Outcome = Result<std::unique_ptr<QualificationHost>>;
        auto lifecycle = AndroidLifecycleController::Create(std::this_thread::get_id());
        if (lifecycle.HasError())
            return Outcome::Failure(lifecycle.ErrorValue());
        auto state = std::make_unique<State>();
        state->lifecycle = std::move(lifecycle).Value();
        state->adapter = std::make_unique<AndroidLifecycleAdapter>(*state->lifecycle);
        auto windows = AndroidWindowHost::Create(*state->lifecycle, *state->adapter, AndroidWindowMode::Interactive);
        if (windows.HasError())
            return Outcome::Failure(windows.ErrorValue());
        state->windows = std::move(windows).Value();
        state->presentation = std::make_unique<PresentationAdmission>(*state->lifecycle, *state->adapter);
        if (auto process = state->adapter->OnProcessCreated(); process.HasError())
            return Outcome::Failure(process.ErrorValue());
        auto activity = state->adapter->OnActivityCreated();
        if (activity.HasError())
            return Outcome::Failure(activity.ErrorValue());
        state->activity = activity.Value();
        if (auto committed = state->Drain(); committed.HasError())
            return Outcome::Failure(committed.ErrorValue());
        if (auto active = state->ActivateRenderer(); active.HasError())
            return Outcome::Failure(active.ErrorValue());
        return Outcome::Success(std::unique_ptr<QualificationHost>(new QualificationHost(std::move(state))));
    }

    /** @copydoc QualificationHost::Command */
    Result<void> QualificationHost::Command(android_app &app, const std::int32_t command) {
        if (auto observed = state_->ActivityCommand(command); observed.HasError())
            return observed;
        return state_->WindowCommand(app, command);
    }

    /** @copydoc QualificationHost::Pump */
    Result<void> QualificationHost::Pump() {
        if (auto committed = state_->Drain(); committed.HasError())
            return committed;
        auto native = state_->windows->Snapshot();
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        const FramebufferExtent extent{native.Value().extent.width, native.Value().extent.height};
        const auto available = state_->backend->PollPresentationAvailability(extent);
        if (available.HasError())
            return Result<void>::Failure(available.ErrorValue());
        if (auto published = state_->presentation->Reconcile(native.Value(), available.Value()); published.HasError())
            return published;
        auto lifecycle = state_->lifecycle->Snapshot();
        if (lifecycle.HasError())
            return Result<void>::Failure(lifecycle.ErrorValue());
        if (!available.Value() || lifecycle.Value().process != AndroidProcessState::Running ||
            lifecycle.Value().activity != AndroidActivityState::Resumed)
            return Result<void>::Success();
        return state_->Render(extent);
    }

    /** @copydoc QualificationHost::PollShutdown */
    Result<bool> QualificationHost::PollShutdown() {
        if (state_->drained)
            return Result<bool>::Success(true);
        if (auto closed = state_->BeginShutdown(); closed.HasError())
            return Result<bool>::Failure(closed.ErrorValue());
        const auto drained = state_->backend->PollDrain();
        if (drained.HasError())
            return Result<bool>::Failure(drained.ErrorValue());
        if (drained.Value() != RenderBackendDrainProgress::Drained)
            return Result<bool>::Success(false);
        const auto windows = state_->windows->Close();
        if (windows.HasError())
            return Result<bool>::Failure(windows.ErrorValue());
        if (!windows.Value())
            return Result<bool>::Success(false);
        if (auto complete = state_->lifecycle->CompleteShutdown(); complete.HasError())
            return Result<bool>::Failure(complete.ErrorValue());
        state_->drained = true;
        return Result<bool>::Success(true);
    }
}  // namespace Horo::AndroidQualification
