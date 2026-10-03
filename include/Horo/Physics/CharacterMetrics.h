#pragma once

/**
 * @file CharacterMetrics.h
 * @brief Bounded Character fixed-tick measurements and host-owned telemetry binding.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"
#include "Horo/Physics/CharacterControllerContracts.h"

#include <array>
#include <cstdint>
#include <thread>

namespace Horo::Character {
    /** @brief Closed Character timing stages; values are never used as metric dimensions dynamically. */
    enum class CharacterMetricPhase : std::uint8_t {
        FreezeCommands,
        ResolveMovement,
        PublishCompletedTick,
        Count
    };

    /** @brief One owner-produced, allocation-free fixed-tick attempt measurement. */
    struct CharacterMetricSnapshot final {
        CharacterWorldId world;
        std::uint64_t sceneGeneration{};
        std::uint64_t tick{};
        std::uint64_t publicationRevision{}; /**< Zero when the attempted tick failed before publication. */
        std::uint32_t activeControllers{};
        std::uint32_t queries{};              /**< Actual sweep calls made during this attempt. */
        std::uint32_t movementIterations{};   /**< Sweep iterations that issued a query. */
        std::uint32_t contacts{};             /**< Valid contacts in resolved movement candidates. */
        std::uint64_t overflows{};            /**< Cumulative bounded-storage reductions for this world. */
        bool failed{};                        /**< Attempt ended before a completed-tick publication. */
        std::array<double, 3> phaseSeconds{}; /**< Owner-clock elapsed seconds for each completed stage. */
        std::array<bool, 3> phaseCompleted{}; /**< True only for stages reached before return. */
    };

    /** @brief Caller-owned optional capture; valid until the synchronous tick returns. */
    struct CharacterMetricCapture final {
        CharacterMetricSnapshot snapshot;
    };

    /** @brief Prebound low-cardinality telemetry series, created outside the fixed-tick path. */
    struct CharacterMetricHandles final {
        std::array<Telemetry::Gauge, 4> counts;
        std::array<Telemetry::Counter, 2> events;
        std::array<Telemetry::Histogram, 3> phaseDurations;
    };

    /** @brief Prepared world bounds used to validate one capture before telemetry emission. */
    struct CharacterMetricLimits final {
        std::uint32_t maximumControllers{};
        std::uint32_t maximumQueriesPerTick{};
        std::uint32_t maximumMovementIterations{};
        std::uint32_t maximumContactsPerMovement{};
    };

    /**
     * @brief Registers and prebinds Character series at host composition time.
     * @param level Host-selected metric collection level.
     * @return Empty handles when collection is off, otherwise fixed-series handles.
     */
    [[nodiscard]] CharacterMetricHandles RegisterCharacterMetricHandles(Telemetry::MetricCollectionLevel level);

    /** @brief Owner-thread bridge from one exact Character world to process telemetry. */
    class CharacterMetricBinding final {
    public:
        CharacterMetricBinding(const CharacterMetricBinding &) = delete;
        CharacterMetricBinding &operator=(const CharacterMetricBinding &) = delete;
        CharacterMetricBinding(CharacterMetricBinding &&) noexcept = default;
        CharacterMetricBinding &operator=(CharacterMetricBinding &&) noexcept = default;

        /**
         * @brief Validates a host-owned binding before the first measured tick.
         * @param world Exact Character world identity.
         * @param sceneGeneration Exact owning scene generation.
         * @param revision Nonzero host-issued binding revision.
         * @param limits Prepared controller and per-tick work bounds.
         * @param level Selected collection level.
         * @param handles Handles prebound outside the tick path.
         * @return Binding or a typed descriptor/capability error.
         */
        [[nodiscard]] static Result<CharacterMetricBinding> Create(CharacterWorldId world, std::uint64_t sceneGeneration,
                                                                   std::uint64_t revision, CharacterMetricLimits limits,
                                                                   Telemetry::MetricCollectionLevel level, CharacterMetricHandles handles);

        /**
         * @brief Publishes one completed synchronous capture after the Character tick returns.
         * @param snapshot Owner-produced attempt snapshot.
         * @param expectedRevision Exact host binding revision retained by the caller.
         * @return Success or typed stale/invalid/affinity error; failures emit no partial metric set.
         */
        [[nodiscard]] Result<void> Publish(const CharacterMetricSnapshot &snapshot, std::uint64_t expectedRevision);

        /** @brief Closes owner-thread admission before world replacement or shutdown. */
        [[nodiscard]] Result<void> Close();

    private:
        CharacterMetricBinding(CharacterWorldId world, std::uint64_t sceneGeneration, std::uint64_t revision, CharacterMetricLimits limits,
                               Telemetry::MetricCollectionLevel level, CharacterMetricHandles handles) noexcept;

        CharacterWorldId world_;
        std::uint64_t sceneGeneration_{};
        std::uint64_t revision_{};
        std::uint64_t lastTick_{};
        std::uint64_t lastPublicationRevision_{};
        std::uint64_t lastOverflows_{};
        CharacterMetricLimits limits_;
        Telemetry::MetricCollectionLevel level_{Telemetry::MetricCollectionLevel::Off};
        CharacterMetricHandles handles_;
        std::thread::id ownerThread_;
        bool closed_{};
    };
}  // namespace Horo::Character
