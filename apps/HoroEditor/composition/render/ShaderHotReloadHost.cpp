#include "ShaderHotReloadHost.h"

#include <limits>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Adds development host context to the canonical reload validation identity. */
        Error HostError(const char *message) {
            return MakeError(ShaderHotReloadErrors::InvalidRequest, message);
        }
    }  // namespace

    /** @copydoc ShaderHotReloadHost::ShaderHotReloadHost */
    ShaderHotReloadHost::ShaderHotReloadHost(const JobSystem &jobs, std::unique_ptr<ShaderHotReloadService> service,
                                             std::shared_ptr<const ShaderHotReloadSourceRecipe> recipe, ShaderHotReloadFence fence)
        : jobs_(jobs), service_(std::move(service)), recipe_(std::move(recipe)), completion_(std::make_shared<Completion>()),
          fence_(fence) {}

    /** @copydoc ShaderHotReloadHost::Create */
    Result<std::unique_ptr<ShaderHotReloadHost>> ShaderHotReloadHost::Create(ShaderBuildService &compiler, const JobSystem &jobs,
                                                                             OperationStore &operations, BuildOutputStore &output,
                                                                             Render::IShaderHotReloadTarget &target,
                                                                             const ShaderHotReloadHostConfiguration &configuration) {
        using Output = Result<std::unique_ptr<ShaderHotReloadHost>>;
        if (configuration.publication.policy != ShaderHotReloadPolicy::Development)
            return Output::Failure(HostError("Explicit development policy is required for source reload."));
        if (auto valid = ValidateShaderHotReloadSourceRecipe(configuration.sources); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        auto selected = target.CaptureState();
        if (selected.HasError())
            return Output::Failure(selected.ErrorValue());
        auto publication = configuration.publication;
        // Only the composed live target can issue this incarnation; callers cannot predict the app-owned device identity.
        publication.context.deviceIncarnation = selected.Value().deviceIncarnation;
        for (const auto &program : configuration.sources.programs) {
            const auto &targets = program.blueprint.compilation.targets;
            if (targets.size() != 1 || targets.front().requirement.backend != selected.Value().backend)
                return Output::Failure(HostError("Every source program must target the actual selected viewport backend."));
        }
        auto service = ShaderHotReloadService::Create(compiler, jobs, operations, output, target, publication);
        if (service.HasError())
            return Output::Failure(service.ErrorValue());
        auto recipe = std::make_shared<const ShaderHotReloadSourceRecipe>(configuration.sources);
        return Output::Success(std::unique_ptr<ShaderHotReloadHost>(
            new ShaderHotReloadHost(jobs, std::move(service).Value(), std::move(recipe), publication.context)));
    }

    /** @copydoc ShaderHotReloadHost::~ShaderHotReloadHost */
    ShaderHotReloadHost::~ShaderHotReloadHost() {
        static_cast<void>(Shutdown());
    }

    /** @copydoc ShaderHotReloadHost::Reload */
    Result<void> ShaderHotReloadHost::Reload() {
        if (closed_)
            return Result<void>::Failure(HostError("The development shader host is closed."));
        if (capture_)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Busy));
        if (auto cancelled = Cancel(); cancelled.HasError())
            return cancelled;
        const auto recipe = recipe_;
        const auto completion = completion_;
        const auto fence = fence_;
        auto admitted = jobs_.SubmitResult({.priority = JobPriority::Interactive, .resource = JobResource::Io},
                                           [recipe, completion, fence](const CancellationToken &token) {
            auto captured = CaptureShaderHotReloadSources(*recipe, fence, token);
            {
                std::lock_guard lock(completion->mutex);
                completion->result.emplace(std::move(captured));
            }
            return Result<void>::Success();
        });
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        capture_.emplace(std::move(admitted).Value());
        return Result<void>::Success();
    }

    /** @copydoc ShaderHotReloadHost::Cancel */
    Result<void> ShaderHotReloadHost::Cancel() {
        if (closed_)
            return Result<void>::Failure(HostError("The development shader host is closed."));
        if (fence_.sourceGeneration == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(HostError("The source occurrence identity is exhausted."));
        if (capture_)
            static_cast<void>(capture_->RequestCancel());
        ++fence_.sourceGeneration;
        return service_->SetContext(fence_);
    }

    /** @copydoc ShaderHotReloadHost::Poll */
    Result<void> ShaderHotReloadHost::Poll() {
        if (closed_)
            return Result<void>::Success();
        std::optional<Result<ShaderHotReloadRequest>> captured;
        {
            std::lock_guard lock(completion_->mutex);
            captured.swap(completion_->result);
        }
        if (captured) {
            capture_.reset();
            if (captured->HasError())
                return Result<void>::Failure(captured->ErrorValue());
            if (captured->Value().fence == fence_) {
                auto requested = service_->Request(captured->Value());
                if (requested.HasError())
                    return Result<void>::Failure(requested.ErrorValue());
            }
        }
        if (capture_) {
            const auto state = capture_->Snapshot();
            if (state && state->terminalResult)
                capture_.reset();
        }
        return service_->Poll();
    }

    /** @copydoc ShaderHotReloadHost::Shutdown */
    Result<void> ShaderHotReloadHost::Shutdown() {
        if (closed_)
            return Result<void>::Success();
        closed_ = true;
        if (capture_) {
            static_cast<void>(capture_->RequestCancel());
            static_cast<void>(capture_->Wait());
            capture_.reset();
        }
        return service_->Shutdown();
    }
}  // namespace Horo::Application::Internal
