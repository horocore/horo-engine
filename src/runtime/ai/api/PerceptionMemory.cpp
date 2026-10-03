#include "Horo/AI/PerceptionMemory.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/PerceptionDescriptorRegistry.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Checks that a declared capacity fits the bounded memory storage. */
        [[nodiscard]] bool ValidCapacity(const std::size_t capacity) noexcept {
            return capacity > 0 && capacity <= MaximumPerceptionMemoryEntries;
        }

        /** @brief Checks every policy value before admitting fixed-tick memory state. */
        [[nodiscard]] bool ValidPolicy(const PerceptionMemoryPolicy &policy) noexcept {
            return ValidCapacity(policy.listenerMaximumEntries) && ValidCapacity(policy.profileMaximumEntries) &&
                   policy.fixedStep.count() > 0 && std::isfinite(policy.memoryDurationSeconds) && policy.memoryDurationSeconds > 0 &&
                   std::isfinite(policy.decayPerSecond) && policy.decayPerSecond >= 0 && std::isfinite(policy.forgetThreshold) &&
                   policy.forgetThreshold >= 0 && policy.forgetThreshold < 1;
        }

        /** @brief Checks sense-supplied velocity before it enters decision-visible memory. */
        [[nodiscard]] bool ValidVelocity(const std::array<double, 3> &velocity) noexcept {
            return std::ranges::all_of(velocity, [](const double component) {
                return std::isfinite(component);
            });
        }
    }  // namespace

    AIPerceptionMemory::AIPerceptionMemory(const std::uint64_t sceneIncarnation, const AgentHandle agent,
                                           const PerceptionMemoryPolicy &policy, const std::uint64_t initialSimulationTick) noexcept
        : sceneIncarnation_(sceneIncarnation), agent_(agent), policy_(policy), simulationTick_(initialSimulationTick) {}

    /** @copydoc AIPerceptionMemory::Create */
    Result<AIPerceptionMemory> AIPerceptionMemory::Create(const std::uint64_t sceneIncarnation, const AgentHandle agent,
                                                          const PerceptionMemoryPolicy &policy, const std::uint64_t initialSimulationTick) {
        if (sceneIncarnation == 0 || !agent.IsValid() || !ValidPolicy(policy))
            return Result<AIPerceptionMemory>::Failure(MakeError(AIErrors::PerceptionMemoryInvalid));
        return Result<AIPerceptionMemory>::Success(AIPerceptionMemory{sceneIncarnation, agent, policy, initialSimulationTick});
    }

    /** @brief Checks exact source scene and typed stimulus identity. */
    bool AIPerceptionMemory::ValidKey(const PerceptionMemoryKey &key) const noexcept {
        return key.IsValid() && key.source.sceneIncarnation == sceneIncarnation_;
    }

    /** @brief Converts an exact committed-tick difference to simulation seconds only for reporting and decay. */
    double AIPerceptionMemory::AgeSeconds(const std::uint64_t lastSensedTick) const noexcept {
        const auto elapsedTicks = simulationTick_ - lastSensedTick;
        return static_cast<double>(elapsedTicks) * static_cast<double>(policy_.fixedStep.count()) / 1'000'000'000.0;
    }

    /** @brief Removes one record without changing the order of survivors. */
    void AIPerceptionMemory::Erase(const std::size_t index) noexcept {
        for (std::size_t position = index + 1; position < count_; ++position)
            entries_[position - 1] = std::move(entries_[position]);
        --count_;
    }

    /** @copydoc AIPerceptionMemory::AdvanceTo */
    Result<void> AIPerceptionMemory::AdvanceTo(const std::uint64_t simulationTick) {
        if (simulationTick < simulationTick_)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionMemoryTimeInvalid));
        simulationTick_ = simulationTick;
        for (std::size_t index = 0; index < count_;) {
            auto &entry = entries_[index];
            entry.ageSeconds = AgeSeconds(entry.lastSensedTick);
            if (!entry.isCurrentlySensed)
                entry.confidence = std::max(0.0, 1.0 - policy_.decayPerSecond * entry.ageSeconds);
            if (entry.ageSeconds >= policy_.memoryDurationSeconds || entry.confidence <= policy_.forgetThreshold)
                Erase(index);
            else
                ++index;
        }
        return Result<void>::Success();
    }

    /** @copydoc AIPerceptionMemory::Observe */
    Result<void> AIPerceptionMemory::Observe(const PerceptionObservation &observation, const std::uint64_t simulationTick) {
        if (observation.key.sense == SenseTypeIds::Team || observation.key.stimulus == StimulusTypeIds::Team)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionEventUnauthorized));
        return ObserveAdmitted(observation, simulationTick);
    }

    /** @copydoc AIPerceptionMemory::ValidObservation */
    bool AIPerceptionMemory::ValidObservation(const PerceptionObservation &observation) const noexcept {
        if (!ValidKey(observation.key) || !ValidVelocity(observation.velocity))
            return false;
        const bool suppliedProvenance =
            observation.provenance.sceneIncarnation != 0 || observation.provenance.slot != 0 || observation.provenance.generation != 0;
        return !suppliedProvenance || (observation.provenance.IsValid() && observation.provenance.sceneIncarnation == sceneIncarnation_);
    }

    /** @copydoc AIPerceptionMemory::ObserveAdmitted */
    Result<void> AIPerceptionMemory::ObserveAdmitted(const PerceptionObservation &observation, const std::uint64_t simulationTick) {
        if (!ValidObservation(observation))
            return Result<void>::Failure(MakeError(AIErrors::PerceptionMemoryInvalid));
        const PerceptionSourceRef provenance = observation.provenance.IsValid() ? observation.provenance : observation.key.source;
        if (const auto advanced = AdvanceTo(simulationTick); advanced.HasError())
            return advanced;

        for (std::size_t index = 0; index < count_; ++index) {
            auto &entry = entries_[index];
            if (entry.key != observation.key)
                continue;
            entry.lastKnownPosition = observation.position;
            entry.provenance = provenance;
            entry.lastKnownVelocity = observation.velocity;
            entry.lastSensedTick = simulationTick;
            entry.ageSeconds = 0;
            entry.confidence = 1;
            entry.isCurrentlySensed = true;
            return Result<void>::Success();
        }

        EvictFor(observation.key.listener);
        entries_[count_++] = {.key = observation.key,
                              .provenance = provenance,
                              .lastKnownPosition = observation.position,
                              .lastKnownVelocity = observation.velocity,
                              .firstSensedTick = simulationTick,
                              .lastSensedTick = simulationTick};
        return Result<void>::Success();
    }

    /** @copydoc AIPerceptionMemory::EvictFor */
    void AIPerceptionMemory::EvictFor(const PerceptionListenerTypeId listener) noexcept {
        const auto listenerCount =
            static_cast<std::size_t>(std::ranges::count(std::span{entries_}.first(count_), listener, [](const PerceivedStimulus &entry) {
            return entry.key.listener;
        }));
        if (const bool listenerFull = listenerCount >= ListenerCapacity(); listenerFull || count_ == Capacity()) {
            std::size_t victim = count_;
            for (std::size_t index = 0; index < count_; ++index) {
                if (listenerFull && entries_[index].key.listener != listener)
                    continue;
                if (victim == count_ || std::tie(entries_[index].confidence, entries_[index].lastSensedTick) <
                                            std::tie(entries_[victim].confidence, entries_[victim].lastSensedTick))
                    victim = index;
            }
            Erase(victim);
        }
    }

    /** @copydoc AIPerceptionMemory::MarkLost */
    Result<void> AIPerceptionMemory::MarkLost(const PerceptionMemoryKey &key, const std::uint64_t simulationTick) {
        if (!ValidKey(key))
            return Result<void>::Failure(MakeError(AIErrors::PerceptionMemoryInvalid));
        if (const auto advanced = AdvanceTo(simulationTick); advanced.HasError())
            return advanced;
        for (std::size_t index = 0; index < count_; ++index) {
            if (entries_[index].key == key) {
                entries_[index].isCurrentlySensed = false;
                entries_[index].confidence = std::max(0.0, 1.0 - policy_.decayPerSecond * entries_[index].ageSeconds);
                if (entries_[index].confidence <= policy_.forgetThreshold)
                    Erase(index);
                break;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc AIPerceptionMemory::ForgetSource */
    Result<void> AIPerceptionMemory::ForgetSource(const PerceptionSourceRef source) {
        if (!source.IsValid() || source.sceneIncarnation != sceneIncarnation_)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionMemoryInvalid));
        for (std::size_t index = 0; index < count_;) {
            if (entries_[index].key.source == source)
                Erase(index);
            else
                ++index;
        }
        return Result<void>::Success();
    }

    /** @brief Removes stale weak references before exposing decision-visible memory. */
    void AIPerceptionMemory::PruneDead(const PerceptionSourceLiveness liveness) {
        for (std::size_t index = 0; index < count_;) {
            if (!liveness.isAlive(liveness.context, entries_[index].key.source))
                Erase(index);
            else
                ++index;
        }
    }

    /** @copydoc AIPerceptionMemory::Find */
    Result<std::optional<PerceivedStimulus>> AIPerceptionMemory::Find(const PerceptionMemoryKey &key, const std::uint64_t simulationTick,
                                                                      const PerceptionSourceLiveness liveness) {
        if (!ValidKey(key) || liveness.isAlive == nullptr)
            return Result<std::optional<PerceivedStimulus>>::Failure(MakeError(AIErrors::PerceptionMemoryInvalid));
        if (const auto advanced = AdvanceTo(simulationTick); advanced.HasError())
            return Result<std::optional<PerceivedStimulus>>::Failure(advanced.ErrorValue());
        PruneDead(liveness);
        for (std::size_t index = 0; index < count_; ++index) {
            if (entries_[index].key == key)
                return Result<std::optional<PerceivedStimulus>>::Success(entries_[index]);
        }
        return Result<std::optional<PerceivedStimulus>>::Success(std::nullopt);
    }

    /** @copydoc AIPerceptionMemory::Snapshot */
    Result<PerceptionMemorySnapshot> AIPerceptionMemory::Snapshot(const std::uint64_t simulationTick,
                                                                  const PerceptionSourceLiveness liveness) {
        if (liveness.isAlive == nullptr)
            return Result<PerceptionMemorySnapshot>::Failure(MakeError(AIErrors::PerceptionMemoryInvalid));
        if (const auto advanced = AdvanceTo(simulationTick); advanced.HasError())
            return Result<PerceptionMemorySnapshot>::Failure(advanced.ErrorValue());
        PruneDead(liveness);
        PerceptionMemorySnapshot snapshot;
        snapshot.count = count_;
        std::ranges::copy_n(entries_.begin(), count_, snapshot.entries.begin());
        return Result<PerceptionMemorySnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc AIPerceptionMemory::ResetAt */
    Result<void> AIPerceptionMemory::ResetAt(const std::uint64_t simulationTick) {
        count_ = 0;
        simulationTick_ = simulationTick;
        return Result<void>::Success();
    }

    /** @copydoc AIPerceptionMemory::Capacity */
    std::size_t AIPerceptionMemory::Capacity() const noexcept {
        return policy_.profileMaximumEntries;
    }

    /** @copydoc AIPerceptionMemory::ListenerCapacity */
    std::size_t AIPerceptionMemory::ListenerCapacity() const noexcept {
        return std::min(policy_.listenerMaximumEntries, policy_.profileMaximumEntries);
    }

    /** @copydoc AIPerceptionMemory::Agent */
    AgentHandle AIPerceptionMemory::Agent() const noexcept {
        return agent_;
    }

    /** @copydoc AIPerceptionMemory::SceneIncarnation */
    std::uint64_t AIPerceptionMemory::SceneIncarnation() const noexcept {
        return sceneIncarnation_;
    }

    /** @copydoc AIPerceptionMemory::StoredCount */
    std::size_t AIPerceptionMemory::StoredCount() const noexcept {
        return count_;
    }
}  // namespace Horo::AI
