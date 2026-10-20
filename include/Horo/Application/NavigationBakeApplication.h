#pragma once

/** @file NavigationBakeApplication.h
 * @brief Shared project navigation operation owner for editor, CLI and MCP adapters.
 */

#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Application/NavigationProjectBakeInput.h"

namespace Horo::Application {
    /** @brief Owned invocation fence retained by source capture through final adoption. */
    struct NavigationBakeInvocation final {
        CancellationToken cancellation;
        std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::time_point::max()};
        std::function<bool()> authorityStopped; /**< Thread-safe owned revocation view; no borrowed protocol callbacks. */
    };

    /** @brief One complete project operation; selection never filters the captured replacement closure. */
    struct NavigationBakeCommandRequest final {
        Navigation::NavigationBakeExecutionMode execution{Navigation::NavigationBakeExecutionMode::BuildAndPublish};
        std::optional<std::vector<Navigation::NavigationBakeSurfaceKey>> surfaces;
        NavigationBakeInvocation invocation;
    };

    /** @brief Explicit host binding; constructing an application does not capture source or start a bake. */
    struct NavigationBakeApplicationConfig final {
        NavigationBakeServiceConfig service;
        Navigation::NavigationTileBakeCompatibility compatibility; /**< Required nonzero provider, schema and host-settings stamps. */
        /** @brief Captures fresh source on an I/O worker using the original project trust/generation/deadline.
         * The callback owns its authority state. The returned source retains a thread-safe live authority
         * fence for final publication; accepting a stale DTO here cannot grant write authority.
         * Package source requires the host's ordinary restore/trust admission before this callback succeeds.
         */
        std::function<Result<std::shared_ptr<const NavigationProjectBakeSource>>(const NavigationBakeInvocation &)> captureSource;
        NavigationProjectBakeInputLimits limits;
    };

    /** @brief Immutable bounded operation projection. Publication is present only for this successful build. */
    struct NavigationBakeCommandSnapshot final {
        std::uint64_t id{};
        std::uint64_t revision{};
        OperationState state{OperationState::Queued};
        std::string phase;
        std::optional<float> progress;
        std::optional<Error> diagnostic;
        Navigation::NavigationBakeExecutionMode execution{Navigation::NavigationBakeExecutionMode::BuildAndPublish};
        std::optional<OperationId> bakeOperation;
        std::optional<Sha256Digest> inputFingerprint;
        std::size_t surfaceCount{};
        std::size_t tileCount{};
        std::shared_ptr<const NavigationBakePublication> publication;

        /** @brief Reports whether capture and bake have both stopped. @return True for a terminal operation. */
        [[nodiscard]] bool IsTerminal() const noexcept;
    };

    /** @brief Single-owner project operation with nonblocking capture-to-bake handoff.
     * @details All facade calls run on the construction lane. Source capture uses an owned I/O job;
     * Pump submits to the existing bake service only after that job drains, avoiding nested worker waits.
     * One active operation and its last result are retained. Jobs and operation store outlive Close/drain.
     */
    class NavigationBakeApplication final {
        struct Impl;

        struct ConstructionKey {
        private:
            friend class NavigationBakeApplication;
            ConstructionKey() = default;
        };

    public:
        /** @brief Internal factory-only construction; the private key preserves Create validation.
         * @param impl Explicit validated owned composition; only Create can produce the construction key.
         */
        NavigationBakeApplication(ConstructionKey, std::unique_ptr<Impl> impl);
        /** @brief Validates explicit composition without starting work.
         * @param config Fixed project, provider, output namespace and owned source authority.
         * @param operations Existing shared bake operation store.
         * @param jobs Scheduler with at least two CPU workers and an I/O lane.
         * @return Owned application or typed admission failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<NavigationBakeApplication>> Create(NavigationBakeApplicationConfig config,
                                                                                       OperationStore &operations, JobSystem &jobs);
        /** @brief Requires successful Close after accepted work has drained. */
        ~NavigationBakeApplication();
        NavigationBakeApplication(const NavigationBakeApplication &) = delete;
        NavigationBakeApplication &operator=(const NavigationBakeApplication &) = delete;

        /** @brief Starts a fresh complete capture without blocking the owner lane.
         * @param request Execution policy and exact optional Scene-qualified surface selection.
         * @return Nonzero operation identity, or invalid/busy/closed/scheduler admission failure.
         */
        [[nodiscard]] Result<std::uint64_t> Start(NavigationBakeCommandRequest request);
        /** @brief Advances completed capture and bake state without waiting. @return Owner-lane result. */
        [[nodiscard]] Result<void> Pump();
        /** @brief Copies the retained bounded operation. @param id Exact issued identity. @return Snapshot or unknown identity. */
        [[nodiscard]] Result<NavigationBakeCommandSnapshot> Query(std::uint64_t id) const;
        /** @brief Requests cancellation without retracting an already adopted publication.
         * @param id Exact issued identity. @return Success or owner/identity error.
         */
        [[nodiscard]] Result<void> RequestCancel(std::uint64_t id);
        /** @brief Closes admission and requests cancellation; keep pumping until success.
         * @return Success when drained, or busy while owned capture/bake work remains.
         */
        [[nodiscard]] Result<void> Close();

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Application
