#pragma once

/** @file ConsoleStartupExecution.h @brief Bounded complete startup preparation and explicit nonblocking owner admission. */
#include "Horo/Debug/CommandExecutor.h"
#include "Horo/Debug/ConsoleStartup.h"

namespace Horo::Debug {
    /** @brief Aggregate startup budgets, checked before dispatch and without silently truncating inputs. */
    struct ConsoleStartupLimits final {
        std::size_t commands{256};       /**< Positive, at most 256 expanded commands. */
        std::size_t commandBytes{65536}; /**< Positive aggregate command bytes, at most 1 MiB. */
        std::size_t scripts{16};         /**< At most sixteen existing 64 KiB rooted script captures. */
    };

    /** @brief Borrowed capture authority, never retained by the prepared plan.
     * Provide a project context for project-scoped direct commands as well as scripts.
     */
    struct ConsoleStartupFiles final {
        const Platform::IProjectReadFiles *files{};      /**< Required only when a script is requested. */
        const Platform::ProjectReadContext *context{};   /**< Required for scripts; callback is borrowed for this call only. */
        std::span<const std::string_view> approvedPaths; /**< Exact paths, not directory-prefix grants. */
    };

    /** @brief Every command was admitted; asynchronous completion remains solely in the existing stores. */
    struct ConsoleStartupAccepted final {
        std::vector<CommandReceipt> receipts;
    };

    /** @brief Admission stopped after an explicit accepted prefix; no hidden retry or rollback occurs.
     * The host retains every operation/job receipt, cancels through its normal controls and aborts unpublished startup as appropriate.
     * Already completed user-approved side effects are not undone by this adapter.
     */
    struct ConsoleStartupPartialFailure final {
        std::vector<CommandReceipt> receipts;
        Error error;
    };

    /** @brief Closed admission outcome; callers must distinguish all-admitted from partial failure. */
    using ConsoleStartupDispatch = std::variant<ConsoleStartupAccepted, ConsoleStartupPartialFailure>;

    /** @brief One owned, single-attempt startup sequence, bound to its immutable registry and configuration capture.
     * Preparation is startup/load/I/O work; never call it in a render/update hot path. Move to the host owner before Submit.
     * The plan retains no filesystem service, query callback, UI, job scheduler or raw subsystem pointer.
     * Moves and Submit are serialized by the host; this move-only value is not a concurrent submission queue.
     */
    class PreparedConsoleStartup final {
    public:
        PreparedConsoleStartup(const PreparedConsoleStartup &) = delete;
        PreparedConsoleStartup &operator=(const PreparedConsoleStartup &) = delete;
        PreparedConsoleStartup(PreparedConsoleStartup &&) noexcept = default;
        PreparedConsoleStartup &operator=(PreparedConsoleStartup &&) noexcept = default;
        /** @brief Expands ordered inputs and validates the whole sequence before any command is submitted.
         * @param launch Snapshot-derived request with matching revision. @param configuration Exact canonical captured snapshot.
         * @param registry Same immutable inventory that will own execution. @param principal Current host principal.
         * @param files Optional borrowed rooted/project authority. @param limits Aggregate finite budgets.
         * @return Complete owned plan or original typed failure, with zero command-handler side effects.
         */
        [[nodiscard]] static Result<PreparedConsoleStartup> Prepare(const ConsoleLaunchConfiguration &launch,
                                                                    const ConfigurationSnapshotRef &configuration,
                                                                    std::shared_ptr<const CommandRegistry> registry,
                                                                    const DebugCommandContext &principal,
                                                                    const ConsoleStartupFiles &files = {},
                                                                    ConsoleStartupLimits limits = {});
        /** @brief Revalidates all current grants, then submits once through the common executor on its owner, never waiting.
         * @param executor Exact bound dispatcher. @param principal Current grants, rechecked before any handler runs.
         * @param currentProject Fresh borrowed context matching a captured project fence, required when one was captured.
         * @return Preflight failure with no dispatch, or the closed all/partial admission outcome retaining every receipt.
         * @note All successful/failed admission attempts consume the plan. Preflight rejection does not consume it.
         * The host closes this executor before project replacement and keeps it alive while submitted handles are observed.
         */
        [[nodiscard]] Result<ConsoleStartupDispatch> Submit(CommandExecutor &executor, const DebugCommandContext &principal,
                                                            const Platform::ProjectReadContext *currentProject = nullptr);
        /** @brief Reports the finite expanded command count without exposing raw sensitive inputs. @return Plan cardinality. */
        [[nodiscard]] std::size_t CommandCount() const noexcept;

    private:
        /** @brief Retains validated immutable composition captures; reads and activates no service.
         * @param registry Owned inventory lifetime. @param configuration Owned immutable revision.
         */
        PreparedConsoleStartup(std::shared_ptr<const CommandRegistry> registry, ConfigurationSnapshotRef configuration);
        /** @brief Checks captured cancellation and fresh project authority without retaining the supplied callback. */
        [[nodiscard]] Result<void> CheckProject(const Platform::ProjectReadContext *currentProject) const;
        std::shared_ptr<const CommandRegistry> registry_;
        ConfigurationSnapshotRef configuration_;
        std::vector<std::string> commands_;
        std::string projectIdentity_;
        std::uint64_t projectGeneration_{};
        CancellationToken projectCancellation_;
        bool consumed_{};
    };
}  // namespace Horo::Debug
