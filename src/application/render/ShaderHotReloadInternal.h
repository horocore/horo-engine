#pragma once
#include "Horo/Application/ShaderHotReloadService.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <array>
#include <mutex>
#include <thread>

namespace Horo::Application::ShaderHotReloadDetail {
    inline constexpr std::size_t MaximumPrograms = 8;
    using CompiledSet = std::vector<Render::ShaderCompilationBatch>;

    struct Work final {
        ShaderHotReloadRequest request;
        Render::ShaderHotReloadTargetState target;
        OperationId operation{};
        std::optional<JobId> job;
        bool finished{};
        BuildOutputSessionId outputSession;
        std::uint64_t generation{};
        CancellationSource cancellation;
        bool obsolete{};  // Owner only; workers observe cancellation, never publication policy.
        std::mutex mutex;
        std::optional<Result<CompiledSet>> completed;

        Work(ShaderHotReloadRequest input, Render::ShaderHotReloadTargetState state, const CancellationToken &parent)
            : request(std::move(input)), target(state), cancellation(parent) {}
    };

    [[nodiscard]] Result<void> ValidateConfiguration(const ShaderHotReloadConfiguration &configuration);
    [[nodiscard]] Result<void> ValidateInput(const ShaderHotReloadRequest &request, const ShaderHotReloadConfiguration &configuration,
                                             Render::ShaderHotReloadTargetState target);
    [[nodiscard]] Result<void> CompileWork(const std::shared_ptr<Work> &work, ShaderBuildService &compiler,
                                           const CancellationToken &cancellation);
    [[nodiscard]] bool Terminal(const JobSnapshot &snapshot) noexcept;
    [[nodiscard]] Error CancelledError();
}  // namespace Horo::Application::ShaderHotReloadDetail

namespace Horo::Application {
    struct ShaderHotReloadService::State final {
        ShaderBuildService &compiler;
        const JobSystem &jobs;
        OperationStore &operations;
        BuildOutputStore &output;
        Render::IShaderHotReloadTarget &target;
        ShaderHotReloadConfiguration configuration;
        const std::thread::id owner{std::this_thread::get_id()};
        ShaderHotReloadSnapshot snapshot;
        std::shared_ptr<ShaderHotReloadDetail::Work> active;
        std::shared_ptr<ShaderHotReloadDetail::Work> pending;
        std::optional<JobHandle> job;
        std::uint64_t generation{};
        bool busy{};

        State(ShaderBuildService &builds, const JobSystem &scheduler, OperationStore &store, BuildOutputStore &diagnostics,
              Render::IShaderHotReloadTarget &consumer, ShaderHotReloadConfiguration policy)
            : compiler(builds), jobs(scheduler), operations(store), output(diagnostics), target(consumer), configuration(policy) {
            snapshot.context = configuration.context;
            snapshot.revision = 1;
        }

        [[nodiscard]] Result<void> RequireOwner() const;
        [[nodiscard]] Result<void> StartPending();
        [[nodiscard]] Result<void> ProcessCompletion();
        void ConsumeCompletion(const std::shared_ptr<ShaderHotReloadDetail::Work> &work, const JobSnapshot &status);
        [[nodiscard]] Result<void> Adopt(const std::shared_ptr<ShaderHotReloadDetail::Work> &work,
                                         const ShaderHotReloadDetail::CompiledSet &compiled);
        void Finish(const std::shared_ptr<ShaderHotReloadDetail::Work> &work, ShaderHotReloadOutcome outcome,
                    std::optional<Error> error = {}, std::optional<Render::ShaderHotReloadTargetState> adopted = {});
        void CancelActive(bool obsolete);
        void DiscardPending(ShaderHotReloadOutcome outcome);
        void Refresh();
        [[nodiscard]] Result<std::shared_ptr<ShaderHotReloadDetail::Work>> PrepareRequest(const ShaderHotReloadRequest &,
                                                                                          const CancellationToken &);
        [[nodiscard]] Result<void> RegisterRequest(const std::shared_ptr<ShaderHotReloadDetail::Work> &);
        void Join() noexcept;
    };
}  // namespace Horo::Application
