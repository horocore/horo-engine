#pragma once

/** @file RuntimeSimulationTiming.h
 * @brief Host-owned simulation rate, composed pause leases and bounded single-step control.
 */
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <memory>

namespace Horo::Runtime {
    class FrameScheduler;
    class RuntimeHost;

    namespace SimulationTimingDetail {
        struct Storage;
    }

    /** @brief Positive rational host rate; gameplay pause is a separate composed state. */
    struct RuntimeSimulationRate final {
        std::uint32_t numerator{1};
        std::uint32_t denominator{1};
        [[nodiscard]] constexpr auto operator<=>(const RuntimeSimulationRate &) const noexcept = default;
    };

    /** @brief Exact sub-nanosecond accumulator remainder retained across admitted rate changes. */
    struct RuntimeSimulationRemainder final {
        std::uint32_t numerator{};
        std::uint32_t denominator{1};
        [[nodiscard]] constexpr auto operator<=>(const RuntimeSimulationRemainder &) const noexcept = default;
    };

    /** @brief Closed host pause reasons; no reason by itself grants acquisition authority. */
    enum class RuntimeSimulationPauseReason : std::uint8_t {
        Gameplay,
        Menu,
        Cinematic,
        Debugger,
        Application,
        Count
    };

    /** @brief Immutable copied host policy facts; these values cannot issue commands or manufacture commitment. */
    struct RuntimeSimulationPolicy final {
        RuntimeSimulationRate rate;
        RuntimeSimulationRemainder remainder;
        std::uint64_t rateRevision{1};
        std::uint64_t pauseRevision{1};
        std::uint64_t commandRevision{1};
        std::uint32_t pauseCount{};
        std::uint32_t pendingSteps{};
        bool paused{};
    };

    /**
     * @brief Opaque actual-control read that fences a command against its issuer and observed admission revision.
     * @details Copies are read-only pins. A read from another host, after retirement, or before a competing command is stale.
     *          Pin identity is never serialized or converted into a numerical runtime identity.
     */
    class RuntimeSimulationPolicyRead final {
    public:
        /** @brief Returns copied desired policy at the observed command boundary. @return Immutable facts. */
        [[nodiscard]] const RuntimeSimulationPolicy &Policy() const noexcept {
            return policy_;
        }

    private:
        friend class RuntimeSimulationControl;
        RuntimeSimulationPolicyRead(std::shared_ptr<const SimulationTimingDetail::Storage> owner,
                                    const RuntimeSimulationPolicy &policy) noexcept;
        std::shared_ptr<const SimulationTimingDetail::Storage> owner_;
        RuntimeSimulationPolicy policy_;
    };

    /**
     * @brief Move-only composed pause request pinning its actual host control record.
     * @details Acquisition is owner-thread only. Release/destruction sends one lock-free signal, including after host retirement;
     *          it never calls a host or module callback. The owner applies that signal at its next command cutoff. Other active
     *          reasons remain paused. The live host retains storage, so ordinary frame-hot release does not reclaim it.
     */
    class RuntimeSimulationPauseLease final {
    public:
        ~RuntimeSimulationPauseLease();
        RuntimeSimulationPauseLease(RuntimeSimulationPauseLease &&other) noexcept;
        RuntimeSimulationPauseLease &operator=(RuntimeSimulationPauseLease &&other) noexcept;
        RuntimeSimulationPauseLease(const RuntimeSimulationPauseLease &) = delete;
        RuntimeSimulationPauseLease &operator=(const RuntimeSimulationPauseLease &) = delete;
        /** @brief Signals release once and makes this lease inert; pause changes at the next owner cutoff. */
        void Release() noexcept;
        /** @brief Reports whether this lease still owns its request. @return False after move or release. */
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        friend class RuntimeSimulationControl;
        RuntimeSimulationPauseLease(std::shared_ptr<SimulationTimingDetail::Storage> owner, std::uint32_t slot,
                                    std::uint64_t generation) noexcept;
        std::shared_ptr<SimulationTimingDetail::Storage> owner_;
        std::uint32_t slot_{};
        std::uint64_t generation_{};
    };

    /** @brief Observable outcome of one explicit fixed-quantum request. */
    enum class RuntimeSingleStepState : std::uint8_t {
        Pending,
        Committed,
        Cancelled
    };
    /** @brief Typed cancellation; a cancelled request contributes no simulated duration. */
    enum class RuntimeSingleStepCancellation : std::uint8_t {
        None,
        PauseChanged,
        HostRetired
    };

    /** @brief Exact successful dispatch evidence, or typed pending/cancelled state. */
    struct RuntimeSingleStepResult final {
        RuntimeSingleStepState state{RuntimeSingleStepState::Pending};
        RuntimeSingleStepCancellation cancellation{RuntimeSingleStepCancellation::None};
        std::uint64_t requestSequence{};
        std::uint64_t simulationTick{};
        std::uint64_t attemptNumber{};
        std::uint64_t frameNumber{};
        Duration duration{};
    };

    /**
     * @brief Move-only receipt retaining one bounded step-result slot until released.
     * @details Releasing the receipt closes observation, not an already admitted step. Pending work remains ordered; terminal
     *          storage becomes reusable only after receipt release. Shutdown cancels pending work without a callback.
     */
    class RuntimeSingleStepReceipt final {
    public:
        ~RuntimeSingleStepReceipt();
        RuntimeSingleStepReceipt(RuntimeSingleStepReceipt &&other) noexcept;
        RuntimeSingleStepReceipt &operator=(RuntimeSingleStepReceipt &&other) noexcept;
        RuntimeSingleStepReceipt(const RuntimeSingleStepReceipt &) = delete;
        RuntimeSingleStepReceipt &operator=(const RuntimeSingleStepReceipt &) = delete;
        /** @brief Reads this request on its host owner thread. @return Copied result or typed stale/thread failure. */
        [[nodiscard]] Result<RuntimeSingleStepResult> ResultValue() const;
        /** @brief Releases observation once through a lock-free signal and makes this receipt inert. */
        void Release() noexcept;

    private:
        friend class RuntimeSimulationControl;
        RuntimeSingleStepReceipt(std::shared_ptr<SimulationTimingDetail::Storage> owner, std::uint32_t slot,
                                 std::uint64_t generation) noexcept;
        std::shared_ptr<SimulationTimingDetail::Storage> owner_;
        std::uint32_t slot_{};
        std::uint64_t generation_{};
    };

    /**
     * @brief Sole host timing command capability; owned inline by the actual scheduler and never independently constructible.
     * @details Commands are owner-thread only, revision-fenced and bounded. The scheduler freezes desired policy after successful
     *          ApplyQueuedOwnerThreadCommands; later commands wait for the next cutoff. Paused frames add no accumulator time.
     *          Admitted steps commit ordinary fixed quanta once on success, never rescale fixedDelta or consume the paused
     *          accumulator. Host suspension consumes no steps. Unexpected participant failure preserves RuntimeHost's fatal policy.
     */
    class RuntimeSimulationControl final {
    public:
        ~RuntimeSimulationControl();
        RuntimeSimulationControl(const RuntimeSimulationControl &) = delete;
        RuntimeSimulationControl &operator=(const RuntimeSimulationControl &) = delete;
        RuntimeSimulationControl(RuntimeSimulationControl &&) = delete;
        RuntimeSimulationControl &operator=(RuntimeSimulationControl &&) = delete;
        /** @brief Pins copied desired policy for a subsequent command. @return Read or lifecycle/thread failure. */
        [[nodiscard]] Result<RuntimeSimulationPolicyRead> ReadPolicy() const;
        /** @brief Returns last cutoff's copied policy. @return Facts; no mutable producer authority. */
        [[nodiscard]] RuntimeSimulationPolicy Policy() const noexcept;
        /**
         * @brief Queues a positive bounded rational rate without losing its existing fractional remainder.
         * @param rate Rate normalized at admission. @param expected Actual current control read.
         * @return New command revision, or failure preserving desired/admitted state.
         */
        [[nodiscard]] Result<std::uint64_t> SetRate(RuntimeSimulationRate rate, const RuntimeSimulationPolicyRead &expected);
        /**
         * @brief Acquires one composed bounded pause reason; only this lease can release its request.
         * @param reason Closed diagnostic reason. @param expected Actual current control read.
         * @return Actual request lease or typed stale/capacity/retirement failure.
         */
        [[nodiscard]] Result<RuntimeSimulationPauseLease> AcquirePause(RuntimeSimulationPauseReason reason,
                                                                       const RuntimeSimulationPolicyRead &expected);
        /**
         * @brief Queues one ordinary fixed quantum while desired pause admission remains unchanged.
         * @param expected Current issuer read with at least one active pause reason.
         * @return Retained receipt or typed stale/capacity/policy failure. Suspension retains pending requests.
         */
        [[nodiscard]] Result<RuntimeSingleStepReceipt> RequestStep(const RuntimeSimulationPolicyRead &expected);

    private:
        friend class FrameScheduler;
        friend class RuntimeHost;
        RuntimeSimulationControl() noexcept = default;

        /** @brief Const-propagating ownership; read-only facade access cannot borrow a mutable publisher. */
        struct OwnedState final {
            std::shared_ptr<SimulationTimingDetail::Storage> pin;
            [[nodiscard]] SimulationTimingDetail::Storage *Mutable() noexcept;
            [[nodiscard]] const SimulationTimingDetail::Storage *Borrow() const noexcept;
            [[nodiscard]] std::shared_ptr<const SimulationTimingDetail::Storage> ReadPin() const noexcept;
            [[nodiscard]] std::shared_ptr<SimulationTimingDetail::Storage> &PublisherPin() noexcept;
        };

        [[nodiscard]] Result<void> Initialize(std::uint32_t pauseCapacity, std::uint32_t stepCapacity);
        [[nodiscard]] Result<void> CommitCutoff(bool suspended);
        [[nodiscard]] Result<void> AddAccumulatorTime(Duration delta, Duration &accumulator);
        [[nodiscard]] bool HasAdmittedStep() const noexcept;
        void CommitStep(std::uint64_t tick, std::uint64_t attempt, std::uint64_t frame, Duration duration) noexcept;
        void Close() noexcept;
        OwnedState storage_;
    };
}  // namespace Horo::Runtime
