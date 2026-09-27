#pragma once

/**
 * @file DestructionEventDispatcher.h
 * @brief Application-owned safe-point translation of committed destruction facts to typed destination requests.
 */

#include "Horo/Destruction/DestructionEventStream.h"

#include <array>
#include <cstdint>
#include <span>
#include <thread>

namespace Horo::Destruction {
    /** @brief Closed application destination families; concrete backend handles remain private to their owners. */
    enum class DestructionDestinationKind : std::uint8_t {
        Gameplay,
        Vfx,
        Decal,
        Audio,
        Accessibility
    };

    /** @brief One cooked, ordered mapping layer. No runtime asset lookup or consumer discovery occurs. */
    struct DestructionEventBinding final {
        DestructionFactKind factKind{};                     /**< Exact source semantic kind. */
        std::uint16_t payloadSchema{DestructionFactSchema}; /**< Exact accepted source fact schema. */
        DestructionDestinationKind destination{};           /**< Destination owner family. */
        std::uint16_t requestSchema{1};                     /**< Exact destination capability/request schema. */
        std::uint32_t destinationId{};                      /**< Non-zero cooked destination semantic ID. */
        std::uint16_t layerOrdinal{};                       /**< Stable ordinal for layered effects. */
        bool required{};                                    /**< Required semantic delivery rather than fallible cosmetic admission. */
        bool headlessEligible{};                            /**< Explicit headless policy; required bindings must be eligible. */
    };

    /** @brief Fully qualified idempotency identity, independent of timestamp, position and asset. */
    struct DestructionDestinationRequestId final {
        DestructionEventOccurrenceId occurrence{}; /**< Source committed occurrence. */
        std::uint64_t bindingGeneration{};         /**< Exact immutable cooked binding publication. */
        std::uint32_t destinationId{};             /**< Cooked destination ID. */
        std::uint16_t layerOrdinal{};              /**< Ordered layer identity. */
        [[nodiscard]] auto operator<=>(const DestructionDestinationRequestId &) const noexcept = default;
    };

    /** @brief Copied owner-boundary input; it never borrows a journal record or native consumer state. */
    struct DestructionDestinationRequest final {
        DestructionDestinationRequestId id{};     /**< Stable request identity for consumer deduplication. */
        DestructionDestinationKind destination{}; /**< Typed destination family. */
        std::uint16_t requestSchema{};            /**< Exact cooked destination schema, without fallback. */
        std::uint64_t transitionTicket{};         /**< Source aggregate transition for required reservation correlation. */
        std::uint64_t committedTick{};            /**< Source fixed tick; never a consumer playback clock. */
        DestructionFactPayload payload{};         /**< Finite semantic values, not selected effect assets. */
    };

    /**
     * @brief Borrowed application adapter at its own safe point.
     * @details The composition root owns and revokes adapters. Submit must deduplicate by request id and retain copied
     * values if it queues work; it may not synchronously mutate the source destruction transition.
     */
    class IDestructionDestinationAdapter {
    public:
        virtual ~IDestructionDestinationAdapter() = default;
        /** @brief Checks exact destination request support. @param schema Non-zero cooked request schema.
         * @return True only if this schema is supported without fallback.
         */
        [[nodiscard]] virtual bool Supports(std::uint16_t schema) const noexcept = 0;
        /**
         * @brief Guarantees finite required capacity before aggregate commit.
         * @param transitionTicket Non-zero source transition identity to reserve.
         * @param count Exact maximum requests for this adapter in the planned batch.
         * @return Ok or explicit denial; no source fact has committed yet.
         */
        [[nodiscard]] virtual DestructionEventStatus ReserveRequired(std::uint64_t transitionTicket, std::uint32_t count) noexcept = 0;
        /** @brief Releases any pre-commit capacity for a failed or cancelled transition.
         * @param transitionTicket Exact owner-issued ticket; duplicate cancellation is harmless.
         */
        virtual void CancelRequired(std::uint64_t transitionTicket) noexcept = 0;
        /**
         * @brief Admits a copied request at the destination owner boundary.
         * @param request Generation-fenced idempotent request.
         * @return Ok, AlreadyDispatched, or typed destination failure.
         */
        [[nodiscard]] virtual DestructionEventStatus Submit(const DestructionDestinationRequest &request) noexcept = 0;
    };

    /** @brief Call-scoped adapter registration; no raw pointer is retained by the dispatcher. */
    struct DestructionAdapterSlot final {
        DestructionDestinationKind destination{};  /**< Exact family. */
        std::uint32_t destinationId{};             /**< Exact cooked destination ID. */
        IDestructionDestinationAdapter *adapter{}; /**< Borrowed only during one owner-safe call. */
    };

    /** @brief Result of one bounded safe-point operation. */
    struct DestructionDispatchResult final {
        DestructionEventStatus status{DestructionEventStatus::Invalid}; /**< Typed admission or delivery outcome. */
        DestructionEventCursor next{}; /**< Current cursor; advances only after all required layers complete. */
        std::uint32_t submitted{};     /**< Successfully submitted layers in this call. */
        std::uint32_t suppressed{};    /**< Explicitly omitted or failed optional layers in this call. */
    };

    /**
     * @brief Per-session application dispatcher with fixed cooked bindings and no second source-event store.
     * @details Pump reads one copied fact per call. A required failure leaves its layer pending for bounded recovery;
     * optional failures never block the source cursor. Source lifetime evidence comes from the canonical scene owner.
     */
    class DestructionEventDispatcher final {
    public:
        static constexpr std::size_t MaximumBindings = 64; /**< Fixed finite fan-out envelope per session. */

        /**
         * @brief Validates and copies cooked bindings at session activation.
         * @param world Exact source world incarnation.
         * @param generation Non-zero binding publication generation.
         * @param bindings Finite ordered table, with unique destination/layer identities per fact kind.
         * @param headless Explicit session mode; cosmetic ineligible layers are omitted.
         * @return Valid inert dispatcher or typed rejection.
         */
        [[nodiscard]] static std::pair<DestructionEventStatus, DestructionEventDispatcher> Create(
            DestructionWorldId world, std::uint64_t generation, std::span<const DestructionEventBinding> bindings, bool headless) noexcept;

        /** @brief Returns the installed source cursor. @return Exact world-fenced next fact. */
        [[nodiscard]] DestructionEventCursor Cursor() const noexcept;

        /**
         * @brief Checks required adapters and reserves exact destination capacity before aggregate commit.
         * @param facts Planned immutable source batch.
         * @param adapters Borrowed live adapters for this preflight only.
         * @return Ok or typed invalid/unavailable/capacity failure; no source event is published.
         */
        [[nodiscard]] DestructionEventStatus Preflight(std::span<const DestructionFact> facts,
                                                       std::span<const DestructionAdapterSlot> adapters) const noexcept;

        /**
         * @brief Releases required capacity if the aggregate transition fails before commit.
         * @param transitionTicket Exact ticket previously passed to Preflight.
         * @param adapters Borrowed same-generation adapters; cancellation is idempotent.
         * @return Ok or owner-thread/lifecycle rejection.
         */
        [[nodiscard]] DestructionEventStatus CancelRequired(std::uint64_t transitionTicket,
                                                            std::span<const DestructionAdapterSlot> adapters) const noexcept;

        /**
         * @brief Dispatches at most one fact at an application safe point, after source aggregate commit.
         * @param stream Committed source journal, never an event bus or mutable state cache.
         * @param currentSource Canonical current handle for the fact about to be consumed.
         * @param adapters Borrowed current destination owners; no pointer survives the call.
         * @return Typed outcome, including Gap requiring canonical snapshot reconciliation.
         */
        [[nodiscard]] DestructionDispatchResult PumpOne(const DestructionEventStream &stream, DestructionHandle currentSource,
                                                        std::span<const DestructionAdapterSlot> adapters) noexcept;

        /**
         * @brief Installs the cursor captured with a full canonical snapshot after Gap or scene activation.
         * @param stream Current source journal used to bound the cursor.
         * @param cursor Snapshot-consistent world-fenced cursor, not an arbitrary replay position.
         * @return Ok or stale/invalid lifecycle status.
         */
        [[nodiscard]] DestructionEventStatus Reconcile(const DestructionEventStream &stream, DestructionEventCursor cursor) noexcept;

        /**
         * @brief Atomically changes cooked mapping at a safe point, never during a partly admitted fact.
         * @param generation New non-zero generation greater than the current one.
         * @param bindings New finite cooked table.
         * @return Ok or typed invalid/stale status.
         */
        [[nodiscard]] DestructionEventStatus ReplaceBindings(std::uint64_t generation,
                                                             std::span<const DestructionEventBinding> bindings) noexcept;

        /** @brief Fences routing and revokes all pending adapter work without deleting committed source facts. */
        void BeginShutdown() noexcept;

    private:
        [[nodiscard]] static bool ValidBindings(std::span<const DestructionEventBinding> bindings, bool headless) noexcept;
        [[nodiscard]] static IDestructionDestinationAdapter *FindAdapter(const DestructionEventBinding &binding,
                                                                         std::span<const DestructionAdapterSlot> adapters) noexcept;
        [[nodiscard]] bool Active(const DestructionEventBinding &binding) const noexcept;
        [[nodiscard]] DestructionEventStatus ValidatePreflightInputs(std::span<const DestructionFact> facts,
                                                                     std::span<const DestructionAdapterSlot> adapters) const noexcept;
        [[nodiscard]] std::uint32_t RequiredRequestCount(const DestructionEventBinding &binding,
                                                         std::span<const DestructionFact> facts) const noexcept;
        [[nodiscard]] DestructionEventStatus ValidateRequiredAdapters(std::span<const DestructionAdapterSlot> adapters) const noexcept;
        [[nodiscard]] DestructionEventStatus ReserveRequiredDestinations(std::span<const DestructionFact> facts,
                                                                         std::span<const DestructionAdapterSlot> adapters) const noexcept;
        [[nodiscard]] DestructionEventStatus CheckPumpSource(const DestructionEventStream &stream,
                                                             DestructionHandle currentSource) const noexcept;
        [[nodiscard]] DestructionEventStatus DeliverLayer(const DestructionEventBinding &binding, const DestructionFact &fact,
                                                          std::span<const DestructionAdapterSlot> adapters,
                                                          DestructionDispatchResult &result) const noexcept;

        DestructionWorldId world_{};
        std::uint64_t generation_{};
        DestructionEventCursor cursor_{};
        std::array<DestructionEventBinding, MaximumBindings> bindings_{};
        std::size_t bindingCount_{};
        std::size_t pendingBinding_{};
        std::thread::id owner_{};
        bool pending_{};
        bool headless_{};
        bool closing_{};
    };
}  // namespace Horo::Destruction
