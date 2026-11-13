#pragma once

/** @file EditorConsoleExecution.h @brief Non-installed Editor owner/render phase adapter for the same console dispatcher. */
#include "EditorConsoleLaunch.h"
#include "Horo/Debug/ConsoleVariableSet.h"

#include <chrono>

namespace Horo::Application {
    class INetworkDebuggerQuery;
}

namespace Horo::Runtime {
    class RuntimeSceneView;
}

namespace Horo::Debug::Internal {
    struct ConsoleSceneInspection;
}

namespace Horo::Application::Internal {
    /** @brief Explicitly activated local core console, retaining the existing process operation authority through shutdown.
     * Create/start, owner/render safe points and destruction run on the single Editor runtime owner.
     * The process scheduler outlives this adapter. No input focus, endpoint, renderer or alternate store is created.
     */
    class EditorConsoleExecution final {
    public:
        /** @brief Binds real admitted core/metric-query/Scene-read handlers and submits the preflighted startup plan through the common
         * executor.
         * @param preparation Owned registry/history and single-attempt plan prepared before native startup.
         * @param jobs Existing process scheduler. @param operations Same process authority used by other host operations.
         * @param configuration Same canonical configuration authority, retained by the owner-thread variable bridge.
         * @param version Privacy-reviewed build/profile identity, without source paths.
         * @param panels Initial registered-panel preferences; only existing log/operation/Scene and explicitly bound Network controls are
         * seeded, without querying stores.
         * @param network Existing retained query owner for an explicitly admitted network inspection command; null otherwise.
         * @return Active adapter or original typed admission failure; partial admission is stopped without implicit rollback/replay.
         */
        [[nodiscard]] static Result<std::unique_ptr<EditorConsoleExecution>> Start(
            EditorConsolePreparation &preparation, JobSystem &jobs, std::shared_ptr<OperationStore> operations,
            std::shared_ptr<ConfigurationService> configuration, std::string version, std::span<const Debug::OverlaySource> panels = {},
            std::shared_ptr<const INetworkDebuggerQuery> network = {});
        /** @brief Stops admission/cancels accepted work without waiting for the borrowed scheduler. */
        ~EditorConsoleExecution();
        EditorConsoleExecution(const EditorConsoleExecution &) = delete;
        EditorConsoleExecution &operator=(const EditorConsoleExecution &) = delete;
        /** @brief Applies one bounded owner cutoff, the entry-boundary canonical history preference and pending result projection.
         * @param now Existing host monotonic time. @param scene Current explicitly supplied preview view, or null when absent.
         * @return Success or original dispatch/projection failure.
         * Scene publication occurs after owner validation and before dispatch; retained views never pin Scene storage.
         * Configuration published by a command during this cutoff is consumed at the next owner boundary.
         * Previously pending NextFrame requests activate before this cutoff; new requests wait for the next boundary.
         * Activation failures remain inspectable and do not block replacement command admission.
         * Wrong-thread/stopped dispatch fails before changing history; pending result queries are capped to four Hz.
         */
        [[nodiscard]] Result<void> OwnerSafePoint(std::chrono::steady_clock::time_point now,
                                                  const Runtime::RuntimeSceneView *scene = nullptr);
        /** @brief Applies one bounded render cutoff at actual RenderExecution entry. @return Success or typed dispatch failure. */
        [[nodiscard]] Result<void> RenderSafePoint();
        /** @brief Idempotently closes admission and requests cancellation without joining the scheduler. */
        void Stop();
        /** @brief Borrows accepted startup receipts, retained through teardown. @return Original bounded admission records. */
        [[nodiscard]] std::span<const Debug::CommandReceipt> Accepted() const noexcept;
        /** @brief Borrows the same dispatcher for the shorter-lived graphical terminal. @return Host-owned command capability. */
        [[nodiscard]] Debug::CommandExecutor &Dispatcher() noexcept;
        /** @brief Borrows the exact variable inventory used by dispatcher and help/find. @return Owner-thread state capability. */
        [[nodiscard]] const Debug::ConsoleVariableSet &Variables() const noexcept;
        /** @brief Reads the actual session visibility request, independent of temporary focus/native presentation barriers.
         * @return Typed requested state or original owner/type failure; acquires no focus and creates no native surface.
         */
        [[nodiscard]] Result<bool> ConsoleVisible() const;
        /** @brief Sends a local UI visibility intent through the same bounded owner queue and process operation authority.
         * @param visible Desired session control value. @return Accepted receipt or typed denial; effective state changes at owner drain.
         * This Public control grants no additional permissions and is never a configuration persistence write.
         */
        [[nodiscard]] Result<Debug::CommandReceipt> RequestConsoleVisibility(bool visible);
        /** @brief Reads a registered process-store panel request through the same session inventory.
         * @param source Logs, Jobs, Scene or an explicitly composed Network panel. @param principal Current host grants. @return Request or
         * typed capability/permission/thread failure.
         */
        [[nodiscard]] Result<bool> StoreOverlayVisible(Debug::OverlaySource source, const Debug::DebugCommandContext &principal) const;
        /** @brief Submits a registered process-store panel preference through common admission, without enabling the overlay feature.
         * @param source Logs, Jobs, Scene or an explicitly composed Network panel. @param visible Requested preference. @param principal
         * Current host grants.
         * @return Original queued operation receipt or typed failure; unsupported sources never receive substitute controls.
         */
        [[nodiscard]] Result<Debug::CommandReceipt> RequestStoreOverlayVisibility(Debug::OverlaySource source, bool visible,
                                                                                  const Debug::DebugCommandContext &principal);
        /** @brief Preserves the original pending-variable activation failure without failing the process frame.
         * @return Owner-thread diagnostic, cleared by a later successful boundary; the variable retains its pending request.
         * A host reports this through normal logging/UI while continuing to drain replacement commands.
         */
        [[nodiscard]] const std::optional<Error> &VariableActivationError() const noexcept;

    private:
        /** @brief Retains the same explicit owners; construction executes no command. */
        EditorConsoleExecution(std::unique_ptr<Debug::CommandExecutor> executor, std::shared_ptr<Debug::ConsoleHistory> history,
                               std::shared_ptr<OperationStore> operations, std::shared_ptr<ConfigurationService> configuration,
                               std::shared_ptr<Debug::ConsoleVariableSet> variables, ConfigurationRevision configurationRevision);
        std::unique_ptr<Debug::CommandExecutor> executor_;
        std::shared_ptr<Debug::ConsoleHistory> history_;
        std::shared_ptr<OperationStore> operations_;
        std::shared_ptr<ConfigurationService> configuration_;
        std::shared_ptr<Debug::ConsoleVariableSet> variables_;
        std::shared_ptr<Debug::Internal::ConsoleSceneInspection> scene_;
        std::optional<Error> variableActivationError_;
        ConfigurationRevision configurationRevision_{};
        std::vector<Debug::CommandReceipt> receipts_;
        std::uint64_t revision_{};
        std::chrono::steady_clock::time_point nextRefresh_{};
    };
}  // namespace Horo::Application::Internal
