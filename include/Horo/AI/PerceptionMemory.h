#pragma once

/**
 * @file PerceptionMemory.h
 * @brief Fixed-capacity scene-owned perception memory with simulation-time decay and weak source checks.
 */

#include "Horo/AI/AIIdentity.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Math/WorldCoordinate64.h"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Horo::AI {
    /** @brief Hard storage ceiling for one agent's remembered stimuli. */
    inline constexpr std::size_t MaximumPerceptionMemoryEntries = 32;

    /**
     * @brief Dependency-neutral weak projection of a RuntimeScene entity reference.
     * @details The scene adapter owns conversion and must check exact incarnation, generation, and residency on every read.
     * This projection is not a second canonical entity identity and must not be used as a raw pointer or durable save identity.
     */
    struct PerceptionSourceRef final {
        std::uint64_t sceneIncarnation{}; /**< Exact active RuntimeScene identity. */
        std::uint32_t slot{};             /**< Entity slot within that scene. */
        std::uint32_t generation{};       /**< Non-zero entity generation. */

        /** @brief Checks the weak reference representation. @return True for a non-zero scene and generation. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return sceneIncarnation != 0 && generation != 0;
        }

        constexpr auto operator<=>(const PerceptionSourceRef &) const noexcept = default;
    };

    /** @brief Typed identity of one listener/source stimulus remembered by an agent. */
    struct PerceptionMemoryKey final {
        PerceptionListenerTypeId listener; /**< Listener descriptor that admitted this observation. */
        SenseTypeId sense;                 /**< Sense that observed the source. */
        StimulusTypeId stimulus;           /**< Typed stimulus payload identity. */
        PerceptionSourceRef source;        /**< Weak scene-generation source reference. */

        /** @brief Checks all identity fields. @return True for a valid key representation. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return listener.IsValid() && sense.IsValid() && stimulus.IsValid() && source.IsValid();
        }

        constexpr auto operator<=>(const PerceptionMemoryKey &) const noexcept = default;
    };

    /** @brief Product and listener memory policy admitted before the fixed-tick path. */
    struct PerceptionMemoryPolicy final {
        std::size_t listenerMaximumEntries{16};         /**< Maximum remembered stimuli from one listener, at most 32. */
        std::size_t profileMaximumEntries{16};          /**< Total per-agent host profile cap, at most 32. */
        std::chrono::nanoseconds fixedStep{16'666'667}; /**< Exact host simulation quantum; never frame or wall time. */
        double memoryDurationSeconds{10.0};             /**< Maximum age since last sensing in simulation seconds. */
        double decayPerSecond{0.1};                     /**< Linear confidence loss after explicit sensing loss. */
        double forgetThreshold{0.0};                    /**< Inclusive confidence threshold for forgetting. */
    };

    /** @brief Last known spatial facts supplied by a simulation sense, never refreshed from a live target during reads. */
    struct PerceptionObservation final {
        PerceptionMemoryKey key;
        Math::WorldCoordinate64 position;
        std::array<double, 3> velocity{}; /**< World velocity in meters per simulation second. */
    };

    /** @brief Immutable value record safe to hand to decisions without exposing mutable memory storage. */
    struct PerceivedStimulus final {
        PerceptionMemoryKey key;
        Math::WorldCoordinate64 lastKnownPosition;
        std::array<double, 3> lastKnownVelocity{};
        std::uint64_t firstSensedTick{}; /**< First committed simulation tick in this memory lifetime. */
        std::uint64_t lastSensedTick{};  /**< Most recent committed simulation tick. */
        double ageSeconds{};             /**< Exact tick age converted with the admitted fixed quantum. */
        double confidence{1.0};          /**< Normalized [0,1] linear-decay confidence. */
        bool isCurrentlySensed{true};    /**< Cleared on explicit loss; expiry removes the record. */
    };

    /** @brief Fixed-capacity copy of currently valid memory records. */
    struct PerceptionMemorySnapshot final {
        std::array<PerceivedStimulus, MaximumPerceptionMemoryEntries> entries{};
        std::size_t count{};
    };

    /** @brief Caller-supplied scene residency check; never retained by the memory. */
    struct PerceptionSourceLiveness final {
        void *context{}; /**< Borrowed scene adapter context valid for this synchronous query. */
        bool (*isAlive)(void *context, const PerceptionSourceRef &source){}; /**< Exact scene/slot/generation residency check. */
    };

    /**
     * @brief One agent's scene-owned, allocation-free memory of typed listener/source stimuli.
     * @details Owner-thread mutation occurs at PerceptionSensePoll. Queries revalidate weak sources and return value copies;
     * blackboard publication is a separate safe-point concern. No wall clock is read here.
     */
    class AIPerceptionMemory final {
    public:
        /**
         * @brief Admits bounded policy for one agent in one exact scene incarnation.
         * @param sceneIncarnation Non-zero RuntimeScene identity.
         * @param agent Generation-fenced scene-owned agent handle.
         * @param policy Product and listener limits and decay settings.
         * @param initialSimulationTick Initial committed simulation tick.
         * @return Memory or a typed invalid scene, agent, or policy failure; every unsigned initial tick is valid.
         */
        [[nodiscard]] static Result<AIPerceptionMemory> Create(std::uint64_t sceneIncarnation, AgentHandle agent,
                                                               PerceptionMemoryPolicy policy = {}, std::uint64_t initialSimulationTick = 0);

        /**
         * @brief Refreshes or admits a stimulus at a monotonic simulation time.
         * @param observation Typed weak source and last-known spatial facts.
         * @param simulationTick Current committed fixed simulation tick, not a frame or wall clock.
         * @return Success or a typed invalid observation/time failure. At a listener or profile cap the weakest, then oldest
         * record within the exhausted scope is evicted.
         */
        [[nodiscard]] Result<void> Observe(const PerceptionObservation &observation, std::uint64_t simulationTick);

        /**
         * @brief Ends current sensing without losing last-known facts; confidence then decays from last sensing time.
         * @param key Exact sensed stimulus key.
         * @param simulationTick Current monotonic committed simulation tick.
         * @return Success even when already absent, or a typed invalid key/time failure.
         */
        [[nodiscard]] Result<void> MarkLost(PerceptionMemoryKey key, std::uint64_t simulationTick);

        /**
         * @brief Advances age, decay, and expiry only when simulation time advances.
         * @details A record still marked currently sensed expires if its sense stops refreshing it; only an explicit loss
         * clears the current-sensing flag before expiry.
         * @param simulationTick Current monotonic committed simulation tick; repeating it models pause.
         * @return Success or a typed invalid/backward time failure.
         */
        [[nodiscard]] Result<void> AdvanceTo(std::uint64_t simulationTick);

        /**
         * @brief Eagerly removes every record for an entity destroyed or recycled by its scene owner.
         * @param source Exact weak source identity.
         * @return Success or a typed foreign/invalid source failure.
         */
        [[nodiscard]] Result<void> ForgetSource(PerceptionSourceRef source);

        /**
         * @brief Returns an exact stimulus only after aging and checking source residency.
         * @param key Stimulus to find.
         * @param simulationTick Current monotonic committed simulation tick.
         * @param liveness Synchronous exact-generation scene residency adapter.
         * @return Value copy or no entry; invalid key, time, or adapter produces a typed failure.
         */
        [[nodiscard]] Result<std::optional<PerceivedStimulus>> Find(PerceptionMemoryKey key, std::uint64_t simulationTick,
                                                                    PerceptionSourceLiveness liveness);

        /**
         * @brief Returns bounded value copies after aging and removing destroyed sources.
         * @param simulationTick Current monotonic committed simulation tick.
         * @param liveness Synchronous exact-generation scene residency adapter.
         * @return Valid records in deterministic storage order or a typed failure.
         */
        [[nodiscard]] Result<PerceptionMemorySnapshot> Snapshot(std::uint64_t simulationTick, PerceptionSourceLiveness liveness);

        /**
         * @brief Clears scene memory and sets a new clock origin for replay rewind or scene reset.
         * @param simulationTick New committed simulation tick, including a rewind to an earlier tick.
         * @return Success; every unsigned committed tick is a valid new clock origin.
         */
        [[nodiscard]] Result<void> ResetAt(std::uint64_t simulationTick);

        /** @brief Returns the admitted total per-agent profile capacity. @return At most 32. */
        [[nodiscard]] std::size_t Capacity() const noexcept;
        /** @brief Returns the admitted per-listener capacity. @return At most 32. */
        [[nodiscard]] std::size_t ListenerCapacity() const noexcept;
        /** @brief Returns the scene-owned agent identity. @return Exact admitted agent handle. */
        [[nodiscard]] AgentHandle Agent() const noexcept;
        /** @brief Returns currently stored record count, before a new time/liveness query. @return At most Capacity(). */
        [[nodiscard]] std::size_t StoredCount() const noexcept;

    private:
        AIPerceptionMemory(std::uint64_t sceneIncarnation, AgentHandle agent, PerceptionMemoryPolicy policy,
                           std::uint64_t initialSimulationTick) noexcept;
        [[nodiscard]] bool ValidKey(const PerceptionMemoryKey &key) const noexcept;
        [[nodiscard]] double AgeSeconds(std::uint64_t lastSensedTick) const noexcept;
        void Erase(std::size_t index) noexcept;
        void PruneDead(PerceptionSourceLiveness liveness);

        std::array<PerceivedStimulus, MaximumPerceptionMemoryEntries> entries_{};
        std::size_t count_{};
        std::uint64_t sceneIncarnation_{};
        AgentHandle agent_;
        PerceptionMemoryPolicy policy_;
        std::uint64_t simulationTick_{};
    };
}  // namespace Horo::AI
