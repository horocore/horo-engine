#pragma once

/** @file RuntimeDispatchEvidence.h
 * @brief Scheduler-issued callback provenance; public frame observations cannot grant dispatch authority.
 */
#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/RuntimeSimulationTiming.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    class FrameScheduler;
    class RuntimeDispatchEvidence;
    enum class RuntimePhase : std::uint8_t;

    /** @brief Allocation-free dispatch admission/read rejection, with no exception or error-string construction. */
    enum class RuntimeDispatchStatus : std::uint8_t {
        Valid,
        Invalid,
        WrongThread,
        ForeignSource,
        Stale,
        WrongPhase,
        Reentrant,
        Exhausted,
        Retired
    };

    /**
     * @brief Copied observations read only from a currently admitted actual scheduler dispatch.
     * @details These values grant no authority when copied or fabricated. Consumers validate the opaque evidence and expected
     *          source for each read. Fixed dispatch admission never claims that all fixed participants have succeeded.
     */
    struct RuntimeDispatchFacts final {
        std::uint64_t frame{};
        std::uint64_t fixedTick{};
        std::uint64_t fixedAttempt{};
        Duration fixedDuration;
        std::uint64_t committedTick{};
        std::uint64_t committedAttempt{};
        std::uint64_t committedFrame{};
        Duration committedDuration;
        Duration presentationAdmittedDuration;
        std::uint64_t presentationGeneration{};
        bool presentationReset{};
        bool presentationClamped{};
        std::uint64_t completedVariableUpdateFrame{};
        RuntimeSimulationPolicy simulationPolicy;
    };

    /**
     * @brief Read-only expected scheduler issuer bound explicitly by application composition.
     * @details A copied capability pins only producer identity/storage, never a callback context or scheduler object. The
     *          scheduler owns all mutation and retires admission on shutdown. It has no numeric or serialized representation.
     *          Destruction after producer retirement is a quiescent lifetime operation; retain the composition pin on hot paths.
     */
    class RuntimeDispatchSource final {
    public:
        RuntimeDispatchSource() noexcept = default;
        /** @brief Reports whether this capability names an issuer. @return False for a default capability. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Checks actual issuer thread, retirement and dispatch-idle admission for explicit application binding.
         * @return Readonly typed admission; this check grants no dispatch or commitment authority.
         */
        [[nodiscard]] RuntimeDispatchStatus BindingStatus() const noexcept;

    private:
        friend class FrameScheduler;
        friend class RuntimeDispatchEvidence;
        struct Storage;

        /** @brief Const-propagating issuer ownership; mutable publication pin exists only on a nonconst producer. */
        class OwnedIssuer final {
        public:
            [[nodiscard]] const Storage *operator->() const noexcept {
                return pin_.get();
            }

            [[nodiscard]] Storage *operator->() noexcept {
                return pin_.get();
            }

            [[nodiscard]] const Storage *Get() const noexcept {
                return pin_.get();
            }

            [[nodiscard]] std::shared_ptr<const Storage> ReadPin() const noexcept {
                return pin_;
            }

            [[nodiscard]] std::shared_ptr<Storage> &PublisherPin() noexcept {
                return pin_;
            }

        private:
            std::shared_ptr<Storage> pin_;
        };

        static_assert(std::is_same_v<decltype(std::declval<const OwnedIssuer &>().operator->()), const Storage *>);
        static_assert(std::is_same_v<decltype(std::declval<OwnedIssuer &>().operator->()), Storage *>);
        static_assert(!std::is_invocable_v<decltype(&OwnedIssuer::PublisherPin), const OwnedIssuer &>);
        OwnedIssuer storage_;

        /** @brief Allocates one lifetime issuer at load time; failure leaves admission invalid. */
        [[nodiscard]] bool Initialize();
        /** @brief Checks immutable owner identity before mutable admission facts. */
        [[nodiscard]] RuntimeDispatchStatus ValidateRun() const noexcept;
        /** @brief Reserves checked actual callback admission with copied producer facts. */
        [[nodiscard]] RuntimeDispatchStatus Begin(RuntimePhase phase, const RuntimeDispatchFacts &facts) noexcept;
        /** @brief Revokes the current owner-thread callback reservation. */
        void End() noexcept;
        /** @brief Closes admission; immutable source pins remain safe through quiescence. */
        void Retire() noexcept;
        /** @brief Copies only readonly producer identity for the currently admitted callback. */
        [[nodiscard]] RuntimeDispatchEvidence Evidence() const noexcept;
    };

    /**
     * @brief Default-invalid scheduler-issued evidence for one actual active callback dispatch.
     * @details Copies cannot extend admission. Each read checks immutable issuer thread identity before mutable facts, exact
     *          expected source, current monotonic dispatch ordinal and phase. Replayed, foreign or fabricated contexts fail.
     *          No context references, callbacks, allocation, blocking or pointer-derived numeric identity are retained.
     */
    class RuntimeDispatchEvidence final {
    public:
        RuntimeDispatchEvidence() noexcept = default;
        /**
         * @brief Reads trusted producer facts only while the exact callback dispatch remains active.
         * @param expected Source explicitly bound by the consuming application participant.
         * @param phase Exact canonical phase required by the consumer.
         * @param facts Output replaced only on a valid read; failures preserve it.
         * @return Exact allocation-free admission status.
         */
        [[nodiscard]] RuntimeDispatchStatus Read(const RuntimeDispatchSource &expected, RuntimePhase phase,
                                                 RuntimeDispatchFacts &facts) const noexcept;

    private:
        friend class RuntimeDispatchSource;

        RuntimeDispatchEvidence(std::shared_ptr<const RuntimeDispatchSource::Storage> storage, std::uint64_t ordinal) noexcept
            : storage_(std::move(storage)), ordinal_(ordinal) {}

        std::shared_ptr<const RuntimeDispatchSource::Storage> storage_;
        std::uint64_t ordinal_{};
    };
}  // namespace Horo::Runtime
