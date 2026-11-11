#pragma once

/** @file CommandExecutor.h
 * @brief Host-bound console execution at explicit owner/render safe points and through Foundation jobs.
 */
#include "Horo/Debug/CommandRegistry.h"
#include "Horo/Foundation/JobSystem.h"

#include <thread>

namespace Horo::Debug {
    class ConsoleVariableSet;

    /** @brief Safe bounded presentation output; persistent logs retain their original owner. */
    struct DebugCommandOutput final {
        std::string text;
    };

    /** @brief Owned execution capability bound after descriptor validation.
     * Implementations retain every borrowed service through outstanding invocations. They may not
     * discover private subsystem pointers or grant themselves thread/authority permissions.
     */
    class ICommandHandler {
    public:
        virtual ~ICommandHandler() = default;
        /** @brief Executes already validated typed arguments on the descriptor's executor.
         * @param context Captured host principal. @param arguments Schema-ordered values.
         * @param cancellation Cooperative cancellation, including host shutdown.
         * @param job Non-null only on WorkerJob; reports progress to the authoritative JobStore.
         * @return Safe presentation output or the owning capability's original typed failure.
         */
        [[nodiscard]] virtual Result<DebugCommandOutput> Execute(const DebugCommandContext &context,
                                                                 std::span<const ArgumentValue> arguments,
                                                                 const CancellationToken &cancellation, const JobExecutionContext *job) = 0;
    };

    /** @brief Explicit host-owned lifetime binding; metadata never executes this capability. */
    struct CommandBinding final {
        DebugCommandId id{};
        std::shared_ptr<ICommandHandler> handler;
        JobResource resource{JobResource::Cpu}; /**< WorkerJob-only host classification; blocking filesystem work uses Io. */
    };

    /** @brief Process observability adapter for safe command admission audits; owns no log store. */
    class ICommandAudit {
    public:
        virtual ~ICommandAudit() = default;
        /** @brief Records a redacted authorized request before dispatch, failing closed on audit failure.
         * @param id Stable command identity. @param text Prepared redacted projection.
         * @return Success or a typed observability failure; no handler runs on failure.
         * @note May be called concurrently by submitting threads. Recording does not guarantee later queue admission.
         */
        [[nodiscard]] virtual Result<void> Record(DebugCommandId id, std::string_view text) = 0;
    };

    /** @brief Submission acceptance; asynchronous state is observed through the existing stores. */
    struct CommandReceipt final {
        std::optional<OperationId> operation;
        std::optional<JobId> job;
        DebugCommandOutput output;
        std::string redactedText;
    };

    /** @brief Thread-safe bounded dispatcher. Hosts drain at canonical lifecycle boundaries.
     * The mutex protects admission, pending requests and shutdown; handlers and store callbacks
     * never run under it. Actual owner identities are fixed at construction. Stop rejects new
     * work and requests cancellation without waiting. Worker callbacks capture owned capabilities
     * and the shared operation authority, never this executor. Jobs must outlive Submit calls.
     */
    class CommandExecutor final {
    public:
        /** @brief Validates complete, unique handler bindings before publishing an executor.
         * @param registry Immutable owned metadata. @param bindings Explicit host capabilities.
         * @param jobs Process scheduler; must outlive this executor.
         * @param operations Existing process authority; no private operation store is created.
         * @param owner Fixed owner thread. @param render Fixed render thread.
         * @param capacity Maximum queued or outstanding commands, including workers.
         * @param audit Existing process logging adapter; required for AuditLogged descriptors.
         * @param variables Optional same-profile inventory owned by owner; submitted through this same dispatcher.
         * @return Inactive-side-effect-free executor or typed composition failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<CommandExecutor>> Create(
            std::shared_ptr<const CommandRegistry> registry, std::span<const CommandBinding> bindings, JobSystem &jobs,
            std::shared_ptr<OperationStore> operations, std::thread::id owner, std::thread::id render, std::size_t capacity = 128,
            std::shared_ptr<ICommandAudit> audit = {}, std::shared_ptr<ConsoleVariableSet> variables = {});
        ~CommandExecutor();
        CommandExecutor(const CommandExecutor &) = delete;
        CommandExecutor &operator=(const CommandExecutor &) = delete;
        /** @brief Parses/authorizes before dispatch; every surface uses this same entry point.
         * @param input Untrusted text. @param context Host principal. @param startup Applies startup allowlist.
         * @param configuration Explicit immutable worker configuration; absent means independent.
         * @return Immediate output or accepted operation/job identities; never waits for a worker.
         */
        [[nodiscard]] Result<CommandReceipt> Submit(std::string_view input, const DebugCommandContext &context, bool startup = false,
                                                    std::optional<ConfigurationSnapshotRef> configuration = std::nullopt);
        /** @brief Drains only the cutoff batch on the fixed owner at ApplyQueuedOwnerThreadCommands.
         * @param budget Maximum requests this phase. @return Count or wrong-thread failure with zero execution.
         */
        [[nodiscard]] Result<std::size_t> DrainOwner(std::size_t budget);
        /** @brief Drains only render work at RenderExecution entry.
         * @param budget Maximum requests this phase. @return Count or wrong-thread failure with zero execution.
         */
        [[nodiscard]] Result<std::size_t> DrainRender(std::size_t budget);
        /** @brief Idempotently rejects new submissions and cancels pending/running work without joining. */
        void Stop();

    private:
        struct State;
        explicit CommandExecutor(std::shared_ptr<State> state);
        [[nodiscard]] Result<std::size_t> Drain(CommandThreadPolicy policy, std::size_t budget);
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Debug
