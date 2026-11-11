#include "D3D12CommandState.h"

#include <algorithm>
#include <format>
#include <new>

namespace Horo::Render {
    namespace {
        /** @brief One startup capacity must fit a finite qualified implementation envelope. */
        bool WithinCommandCapacity(std::uint32_t value, std::uint32_t maximum) noexcept {
            return value > 0 && value <= maximum;
        }
    }  // namespace

    /** @copydoc D3D12CommandError */
    Error D3D12CommandError(const char *code, const char *message, const std::int64_t nativeCause) {
        Error error{ErrorCode{code}, ErrorDomainId{"horo.render.d3d12"}, ErrorSeverity::Error, message, {}};
        if (nativeCause != 0)
            error.message += std::format(" (HRESULT {}).", nativeCause);
        return error;
    }

    /** @copydoc D3D12CommandLimits::IsValid */
    bool D3D12CommandLimits::IsValid() const noexcept {
        return WithinCommandCapacity(maximumPendingSubmissionsPerQueue, 128) && WithinCommandCapacity(maximumTransitions, 65536) &&
               WithinCommandCapacity(maximumBufferCopies, 16384) && WithinCommandCapacity(maximumResourceReferences, 65536) &&
               WithinCommandCapacity(maximumTimelineRanges, 4096);
    }

    /** @copydoc CreateD3D12CommandService */
    Result<std::unique_ptr<ID3D12Commands>> CreateD3D12CommandService(std::unique_ptr<ID3D12CommandRuntime> runtime,
                                                                      const D3D12CommandConfiguration &configuration) {
        if (!runtime)
            return Result<std::unique_ptr<ID3D12Commands>>::Failure(
                D3D12CommandError("render.d3d12.command_runtime", "Provide the exact session-owned command runtime."));
        if (auto valid = D3D12Detail::ValidateCommandConfiguration(configuration); valid.HasError())
            return Result<std::unique_ptr<ID3D12Commands>>::Failure(valid.ErrorValue());
        try {
            auto service = std::make_unique<D3D12Detail::CommandService>(std::move(runtime), configuration);
            if (auto initialized = service->Initialize(); initialized.HasError())
                return Result<std::unique_ptr<ID3D12Commands>>::Failure(initialized.ErrorValue());
            return Result<std::unique_ptr<ID3D12Commands>>::Success(std::move(service));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<ID3D12Commands>>::Failure(
                D3D12CommandError("render.d3d12.command_capacity", "Unable to allocate the finite command preparation envelope."));
        } catch (...) {
            return Result<std::unique_ptr<ID3D12Commands>>::Failure(
                D3D12CommandError("render.d3d12.command_initialization", "The native command adapter failed during initialization."));
        }
    }
}  // namespace Horo::Render

namespace Horo::Render::D3D12Detail {
    CommandService::CommandService(std::unique_ptr<ID3D12CommandRuntime> runtime, const D3D12CommandConfiguration &configuration)
        : runtime_(std::move(runtime)), limits_(configuration.limits), mode_(configuration.barrierMode),
          deviceIncarnation_(configuration.deviceIncarnation) {
        queues_.reserve(configuration.queues.size());
        std::ranges::copy(configuration.queues, bindings_.begin());
        for (const auto binding : configuration.queues) {
            CommandQueueHistory queue{.binding = binding};
            queue.issued.reserve(limits_.maximumTimelineRanges);
            queues_.push_back(std::move(queue));
        }
    }

    D3D12CommandConfiguration CommandService::Configuration() const noexcept {
        return {deviceIncarnation_, std::span{bindings_}.first(queues_.size()), mode_, limits_};
    }

    Result<void> CommandService::Initialize() {
        if (auto initialized = runtime_->Initialize(Configuration()); initialized.HasError())
            return initialized;
        initialized_ = true;
        accepting_ = true;
        return Result<void>::Success();
    }

    Result<void> CommandService::Access(const bool admission) const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(D3D12CommandError("render.d3d12.command_thread", "Use the initializing render owner thread."));
        if (active_)
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_busy", "Finish the current command service call before reentering."));
        if (!initialized_ || (admission && !accepting_))
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_closed",
                                  "The session no longer admits command work; retain pending native ownership until drained."));
        return Result<void>::Success();
    }

    Result<void> CommandService::PollNative() {
        auto polled = runtime_->Poll();
        if (polled.HasError()) {
            accepting_ = false;
            runtime_->StopAdmission();
        }
        return polled;
    }

    Result<void> CommandService::Prepare(const RenderGraphExecutionRequest &request, const CancellationToken &cancellation) {
        try {
            auto prepared = runtime_->PrepareGraph(request, cancellation);
            if (prepared.HasError())
                runtime_->AbortPrepared();
            return prepared;
        } catch (...) {
            runtime_->AbortPrepared();
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_prepare", "Native graph preparation failed before queue execution."));
        }
    }

    Result<void> CommandService::Submit(const RenderGraphExecutionRequest &request) {
        Result<void> submitted = Result<void>::Success();
        try {
            submitted = runtime_->SubmitPrepared(request.lease, request.transfer);
        } catch (...) {
            submitted = Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_submit", "Submission failed; retain native ownership until terminal completion."));
        }
        if (submitted.HasError()) {
            accepting_ = false;
            runtime_->StopAdmission();
            return submitted;
        }
        CommitHistory(request.submissions);
        return submitted;
    }

    /** @copydoc ID3D12Commands::ExecuteGraph */
    Result<void> CommandService::ExecuteGraph(const RenderGraphExecutionRequest &request, const CancellationToken &cancellation) {
        if (auto access = Access(true); access.HasError())
            return access;
        CommandCall call{active_};
        if (auto valid = ValidateRequest(request); valid.HasError())
            return valid;
        if (auto limits = ValidateLimits(request); limits.HasError())
            return limits;
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_cancelled", "Command admission was cancelled before recording."));
        if (auto batch = ValidateBatch(request.submissions); batch.HasError())
            return batch;
        if (auto polled = PollNative(); polled.HasError())
            return polled;
        if (auto history = ReclaimHistory(request.submissions); history.HasError())
            return history;
        if (auto prepared = Prepare(request, cancellation); prepared.HasError())
            return prepared;
        if (cancellation.IsCancellationRequested()) {
            runtime_->AbortPrepared();
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_cancelled", "Command admission was cancelled before execution."));
        }
        return Submit(request);
    }

    /** @copydoc ID3D12Commands::Observe */
    Result<bool> CommandService::Observe(const RenderCpuWait &wait) {
        if (auto access = Access(false); access.HasError())
            return Result<bool>::Failure(access.ErrorValue());
        if (!wait.IsValid() || wait.expectedDeviceIncarnation != deviceIncarnation_)
            return Result<bool>::Failure(
                D3D12CommandError("render.d3d12.timeline_stale", "Use the exact device incarnation and declared finite CPU wait policy."));
        if (auto point = ValidatePoint(wait.completion); point.HasError())
            return Result<bool>::Failure(point.ErrorValue());
        CommandCall call{active_};
        return runtime_->Observe(wait);
    }

    /** @copydoc ID3D12Commands::Poll */
    Result<void> CommandService::Poll() {
        if (auto access = Access(false); access.HasError())
            return access;
        CommandCall call{active_};
        return PollNative();
    }

    /** @copydoc ID3D12Commands::BeginShutdown */
    Result<void> CommandService::BeginShutdown() {
        if (auto access = Access(false); access.HasError())
            return access;
        CommandCall call{active_};
        accepting_ = false;
        runtime_->StopAdmission();
        return Result<void>::Success();
    }

    /** @copydoc ID3D12Commands::PollDrain */
    Result<RenderBackendDrainProgress> CommandService::PollDrain() {
        if (auto access = Access(false); access.HasError())
            return Result<RenderBackendDrainProgress>::Failure(access.ErrorValue());
        if (accepting_)
            return Result<RenderBackendDrainProgress>::Failure(
                D3D12CommandError("render.d3d12.command_shutdown_required", "Close admission before observing final drain."));
        CommandCall call{active_};
        return runtime_->PollDrain();
    }
}  // namespace Horo::Render::D3D12Detail
