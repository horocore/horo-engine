#include "HeadlessSceneLoadBoundary.h"

#include <thread>
#include <utility>

namespace Horo::Application::Internal {
    namespace HeadlessSceneLoadErrors {
        const ErrorCodeDescriptor Invalid{ErrorDomainId{"application.scene"},
                                          ErrorCode{"application.scene.load.invalid"},
                                          ErrorSeverity::Error,
                                          "Invalid private Scene load boundary",
                                          "Supply a fresh Scene service and a finite caller deadline.",
                                          false,
                                          false,
                                          {}};
        const ErrorCodeDescriptor Cancelled{ErrorDomainId{"application.scene"},
                                            ErrorCode{"application.scene.load.cancelled"},
                                            ErrorSeverity::Error,
                                            "Private Scene loading was cancelled",
                                            "Retry with a fresh admitted world candidate.",
                                            true,
                                            false,
                                            {}};
        const ErrorCodeDescriptor Deadline{ErrorDomainId{"application.scene"},
                                           ErrorCode{"application.scene.load.deadline"},
                                           ErrorSeverity::Error,
                                           "Private Scene loading exceeded its deadline",
                                           "Check the required assets and explicit load budget.",
                                           true,
                                           false,
                                           {}};
    }  // namespace HeadlessSceneLoadErrors

    namespace {
        /** @brief Checks the original shared load budget without extending it during progress. */
        Result<void> CheckLoadBoundary(const HeadlessSceneLoadBoundary &boundary) {
            if (boundary.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(HeadlessSceneLoadErrors::Cancelled));
            if (std::chrono::steady_clock::now() >= boundary.deadline)
                return Result<void>::Failure(MakeError(HeadlessSceneLoadErrors::Deadline));
            return Result<void>::Success();
        }

        /** @brief Retires only the admitted private candidate and preserves its primary failure. */
        Result<Runtime::RuntimeSceneView> RetireLoad(Runtime::RuntimeSceneService &service, Error failure) {
            service.Shutdown();
            return Result<Runtime::RuntimeSceneView>::Failure(std::move(failure));
        }
    }  // namespace

    /** @copydoc PrepareHeadlessSceneAtLoadBoundary */
    Result<Runtime::RuntimeSceneView> PrepareHeadlessSceneAtLoadBoundary(Runtime::RuntimeSceneService &service,
                                                                         Runtime::RuntimeSceneDefinition definition,
                                                                         const HeadlessSceneLoadBoundary &boundary) {
        if (service.ActiveScene() || boundary.deadline == std::chrono::steady_clock::time_point{} ||
            boundary.deadline == std::chrono::steady_clock::time_point::max())
            return Result<Runtime::RuntimeSceneView>::Failure(MakeError(HeadlessSceneLoadErrors::Invalid));
        if (const auto admitted = CheckLoadBoundary(boundary); admitted.HasError())
            return Result<Runtime::RuntimeSceneView>::Failure(admitted.ErrorValue());
        if (const auto started = service.Startup(boundary.cancellation); started.HasError())
            return Result<Runtime::RuntimeSceneView>::Failure(started.ErrorValue());
        if (const auto queued = service.QueuePreparation(std::move(definition)); queued.HasError())
            return RetireLoad(service, queued.ErrorValue());
        std::uint64_t frame = 1;
        for (;;) {
            if (const auto stop = CheckLoadBoundary(boundary); stop.HasError())
                return RetireLoad(service, stop.ErrorValue());
            const Runtime::FrameContext context{.frameNumber = frame++, .cancellation = boundary.cancellation};
            if (const auto advanced = service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, context); advanced.HasError())
                return RetireLoad(service, advanced.ErrorValue());
            if (auto failure = service.TakeOperationError())
                return RetireLoad(service, std::move(*failure));
            if (const auto ready = service.ActiveScene()) {
                if (const auto stop = CheckLoadBoundary(boundary); stop.HasError())
                    return RetireLoad(service, stop.ErrorValue());
                return Result<Runtime::RuntimeSceneView>::Success(*ready);
            }
            // Load-time owner polling matches other bounded CLI preparation paths. Background
            // providers honor the Scene service's cooperative handles; normal ticks never wait here.
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
}  // namespace Horo::Application::Internal
