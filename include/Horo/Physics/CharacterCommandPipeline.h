#pragma once

/** @file CharacterCommandPipeline.h
 * @brief Bounded Character movement-command admission and fixed-tick scheduling contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Time.h"
#include "Horo/Physics/CharacterControllerContracts.h"

#include <compare>
#include <cstdint>

namespace Horo::Character {
    /** @brief Non-blocking ownership outcome for one copied movement command. */
    enum class CharacterCommandAdmissionStatus : std::uint8_t {
        Deferred,
        RejectedFull,
        RejectedBusy,
    };

    /** @brief Admission outcome and bounded queue depth after the attempt. */
    struct CharacterCommandAdmission final {
        CharacterCommandAdmissionStatus status{CharacterCommandAdmissionStatus::RejectedFull};
        std::uint32_t pendingCommands{};
    };

    /** @brief Stable observation points within one Character fixed-tick attempt. */
    enum class CharacterTickPhase : std::uint8_t {
        FreezeCommands,
        ResolveMovement,
        PublishCompletedTick,
    };

    /**
     * @brief Optional allocation-free observer for tests and explicit host composition.
     *
     * Callbacks run synchronously on the Character owner thread. They must not throw or retain
     * borrowed requests or publications. Movement callbacks receive only the final replacement
     * selected for each controller and tick. The optional result callback supplies backend-neutral
     * post-tick evidence; Character validates it before publishing any state.
     */
    struct CharacterTickObserver final {
        void *context{}; /**< Caller-owned context valid until AdvanceFixedTick returns. */
        void (*phase)(void *context, CharacterTickPhase phase, std::uint64_t tick) noexcept {};
        void (*movement)(void *context, const CharacterMovementRequest &request) noexcept {};
        Result<CharacterMovementResult> (*movementResult)(void *context, const CharacterMovementRequest &request,
                                                          const CharacterTransformPublication &previous) noexcept {};
    };

    /** @brief Exact host-owned Character tick; no render time or live input state is accepted. */
    struct CharacterFixedTickInput final {
        std::uint64_t tick{};            /**< One-based next attempted simulation tick. */
        std::uint64_t sceneGeneration{}; /**< Exact owning scene generation. */
        Duration fixedDelta{};           /**< Positive immutable host quantum. */
        CharacterTickObserver observer;  /**< Optional synchronous observation. */
    };

    /** @brief Coherent publication marker for the last successfully scheduled Character tick. */
    struct CharacterPublishedTick final {
        std::uint64_t completedTick{};
        std::uint64_t publicationRevision{};
        std::uint32_t appliedCommands{};

        [[nodiscard]] constexpr auto operator<=>(const CharacterPublishedTick &) const noexcept = default;
    };

    /** @brief Allocation-free cumulative command and bounded fast-path counters. */
    struct CharacterTickStatistics final {
        std::uint64_t completedTicks{};
        std::uint64_t admittedCommands{};
        std::uint64_t rejectedCommands{};
        std::uint32_t pendingCommands{};
        std::uint32_t maximumCommandDepth{};
        std::uint64_t commandOverflowCount{}; /**< Full command admissions rejected at the configured bound. */
        std::uint64_t contactOverflowCount{}; /**< Contacts reduced or rejected at retained capacity. */
        std::uint64_t hitOverflowCount{};     /**< Query hits reduced or rejected at retained capacity. */
        std::uint64_t eventOverflowCount{};   /**< Events reduced or rejected at queued capacity. */
        std::uint64_t impulseOverflowCount{}; /**< Staged impulses reduced or rejected at queued capacity. */
        std::uint64_t scratchOverflowCount{}; /**< Byte-scratch reservations rejected at the configured bound. */
        std::uint64_t invalidInputCount{};    /**< Malformed fast-path evidence rejected before retention. */
        std::uint32_t retainedContacts{};     /**< Current owner-thread contact collection occupancy. */
        std::uint32_t queuedHits{};           /**< Current owner-thread query-hit collection occupancy. */
        std::uint32_t queuedEvents{};         /**< Current retained event collection occupancy. */
        std::uint64_t scratchBytesUsed{};     /**< Current per-tick byte-scratch occupancy. */
    };
}  // namespace Horo::Character
