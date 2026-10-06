#pragma once

/**
 * @file RuntimeHost.h
 * @brief Composition owner for runtime lifecycle, frame scheduling, and cancellation.
 */

#include "Horo/Runtime/RuntimeLifecycle.h"

namespace Horo::Runtime {
    /** @brief Owns one runtime lifecycle and scheduler for a graphical or headless composition. */
    class RuntimeHost final {
    public:
        /**
         * @brief Creates a host with a validated scheduler policy.
         * @param clock Host-owned monotonic clock that outlives the returned host.
         * @param config Fixed-step and stall-normalization policy.
         * @return Owned host or a typed invalid-configuration failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<RuntimeHost>> Create(Clock &clock, FrameSchedulerConfig config = {});

        ~RuntimeHost();

        RuntimeHost(const RuntimeHost &) = delete;

        RuntimeHost &operator=(const RuntimeHost &) = delete;

        /**
         * @brief Transfers one participant into the not-yet-started lifecycle.
         * @param participant Non-null participant owned by the host after success.
         * @return Success or a typed null/state failure.
         */
        [[nodiscard]] Result<void> AddParticipant(std::unique_ptr<RuntimeLifecycleParticipant> participant);

        /** @brief Starts the lifecycle and enters Running. @return Success or the first startup/cancellation failure. */
        [[nodiscard]] Result<void> Startup();

        /** @brief Executes one frame, automatically failing and shutting down on error. @return Success or first failure.
         */
        [[nodiscard]] Result<void> RunFrame();

        /** @brief Enters explicit suspension and resets the timing baseline. @return Success or a typed state failure. */
        [[nodiscard]] Result<void> Suspend();

        /** @brief Resumes execution without accumulating suspended wall time. @return Success or a typed state failure. */
        [[nodiscard]] Result<void> Resume();

        /** @brief Cooperatively requests shutdown; safe from a participant callback. */
        void RequestShutdown() const noexcept;

        /** @brief Cancels and tears the lifecycle down once. */
        void Shutdown() noexcept;

        /** @brief Returns the current lifecycle state. @return Current state by value. */
        [[nodiscard]] RuntimeLifecycleState State() const noexcept;

        /** @brief Returns scheduler timing diagnostics by value. @return Allocation-free cumulative statistics snapshot. */
        [[nodiscard]] FrameSchedulerStatistics Statistics() const noexcept;

        /** @brief Borrows the actual host-owned simulation command capability. @return Owner-thread control; outlived by host. */
        [[nodiscard]] RuntimeSimulationControl &SimulationControl() noexcept;
        /** @brief Borrows actual read-only host timing. @return Const capability; opaque reads may retain retired storage. */
        [[nodiscard]] const RuntimeSimulationControl &SimulationControl() const noexcept;

        /** @brief Copies the actual host scheduler issuer for explicit application composition. @return Read-only producer pin. */
        [[nodiscard]] RuntimeDispatchSource DispatchSource() const noexcept;

    private:
        class ConstructionKey {
            ConstructionKey() = default;
            friend class RuntimeHost;
        };

    public:
        RuntimeHost(std::unique_ptr<FrameScheduler> scheduler, ConstructionKey) noexcept;

    private:
        CancellationSource cancellation_;
        RuntimeLifecycle lifecycle_;
        std::unique_ptr<FrameScheduler> scheduler_;
    };
}  // namespace Horo::Runtime
