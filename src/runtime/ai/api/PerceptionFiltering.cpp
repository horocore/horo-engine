#include "Horo/AI/PerceptionFiltering.h"

#include "Horo/AI/AIErrors.h"

#include <limits>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Validates a canonical bounded policy before staging or activation. */
        [[nodiscard]] bool ValidPolicy(const PerceptionFilterPolicy &policy) noexcept {
            if (!policy.affiliations.IsValid() || policy.teamRule >= PerceptionTeamRule::Count ||
                policy.customAffiliationCount > PerceptionFilterPolicy::MaximumCustomAffiliations)
                return false;
            for (std::size_t index = 0; index < policy.customAffiliations.size(); ++index) {
                if (index < policy.customAffiliationCount) {
                    if (!policy.customAffiliations[index].IsValid() ||
                        (index > 0 && policy.customAffiliations[index] <= policy.customAffiliations[index - 1]))
                        return false;
                } else if (policy.customAffiliations[index].IsValid())
                    return false;
            }
            return true;
        }

        /** @brief Rejects malformed candidate facts before any sensing query or publication. */
        [[nodiscard]] bool ValidFacts(const PerceptionFilterFacts &facts) noexcept {
            return facts.key.IsValid() && facts.affiliation < PerceptionAffiliation::Count &&
                   facts.visibility < GameplayPerceptionVisibility::Count &&
                   (facts.affiliation == PerceptionAffiliation::Custom) == facts.customAffiliation.IsValid() && facts.listenerLayers != 0 &&
                   facts.sourceLayers != 0;
        }

        /** @brief Compares only explicit non-zero team identities. */
        [[nodiscard]] bool SameTeam(const PerceptionFilterFacts &facts) noexcept {
            return facts.listenerTeam.IsValid() && facts.sourceTeam.IsValid() && facts.listenerTeam == facts.sourceTeam;
        }

        /** @brief Evaluates only canonical typed metadata, without touching observation payloads. */
        [[nodiscard]] PerceptionFilterOutcome PolicyOutcome(const PerceptionFilterPolicy &policy,
                                                            const PerceptionFilterFacts &facts) noexcept {
            if (!policy.affiliations.Contains(facts.affiliation))
                return PerceptionFilterOutcome::AffiliationDenied;
            if (facts.affiliation == PerceptionAffiliation::Custom && policy.customAffiliationCount != 0) {
                bool found = false;
                for (std::size_t index = 0; index < policy.customAffiliationCount; ++index)
                    found |= policy.customAffiliations[index] == facts.customAffiliation;
                if (!found)
                    return PerceptionFilterOutcome::AffiliationDenied;
            }
            const bool sameTeam = SameTeam(facts);
            if (policy.teamRule == PerceptionTeamRule::SameTeam && !sameTeam)
                return PerceptionFilterOutcome::TeamDenied;
            if (policy.teamRule == PerceptionTeamRule::DifferentTeam &&
                (!facts.listenerTeam.IsValid() || !facts.sourceTeam.IsValid() || sameTeam))
                return PerceptionFilterOutcome::TeamDenied;
            if ((facts.sourceLayers & facts.listenerMask) == 0 || (facts.listenerLayers & facts.sourceMask) == 0)
                return PerceptionFilterOutcome::LayerDenied;
            if (facts.visibility == GameplayPerceptionVisibility::Hidden ||
                (facts.visibility == GameplayPerceptionVisibility::TeamOnly && !sameTeam))
                return PerceptionFilterOutcome::VisibilityDenied;
            return PerceptionFilterOutcome::Allowed;
        }

        /** @brief Keeps memory snapshot creation independent of a concrete RuntimeScene. */
        [[nodiscard]] bool AssumeAliveForAdmission(void *, const PerceptionSourceRef &) noexcept {
            return true;
        }
    }  // namespace

    PerceptionFilteredMemory::PerceptionFilteredMemory(AIPerceptionMemory memory, PerceptionFilterPolicy policy,
                                                       const std::uint64_t sceneIncarnation) noexcept
        : memory_(std::move(memory)), policy_(std::move(policy)), sceneIncarnation_(sceneIncarnation) {}

    /** @copydoc PerceptionFilteredMemory::Create */
    Result<PerceptionFilteredMemory> PerceptionFilteredMemory::Create(const std::uint64_t sceneIncarnation, const AgentHandle agent,
                                                                      const PerceptionMemoryPolicy memoryPolicy,
                                                                      const PerceptionFilterPolicy filterPolicy,
                                                                      const std::uint64_t initialSimulationTick) {
        if (!ValidPolicy(filterPolicy))
            return Result<PerceptionFilteredMemory>::Failure(MakeError(AIErrors::PerceptionFilterInvalid));
        auto memory = AIPerceptionMemory::Create(sceneIncarnation, agent, memoryPolicy, initialSimulationTick);
        if (memory.HasError())
            return Result<PerceptionFilteredMemory>::Failure(memory.ErrorValue());
        return Result<PerceptionFilteredMemory>::Success(
            PerceptionFilteredMemory{std::move(memory).Value(), filterPolicy, sceneIncarnation});
    }

    /** @copydoc PerceptionFilteredMemory::Evaluate */
    Result<PerceptionFilterDecision> PerceptionFilteredMemory::Evaluate(const PerceptionFilterFacts &facts,
                                                                        const std::uint64_t expectedRevision) const {
        if (!ValidFacts(facts) || facts.key.source.sceneIncarnation != sceneIncarnation_)
            return Result<PerceptionFilterDecision>::Failure(MakeError(AIErrors::PerceptionFilterInvalid));
        const auto decision = expectedRevision == policyRevision_ ? PolicyOutcome(policy_, facts) : PerceptionFilterOutcome::StalePolicy;
        return Result<PerceptionFilterDecision>::Success({.outcome = decision, .policyRevision = policyRevision_});
    }

    /** @brief Drops one retained admission fact after its exact memory fact is denied or evicted. */
    void PerceptionFilteredMemory::EraseFacts(const PerceptionMemoryKey key) noexcept {
        for (std::size_t index = 0; index < admittedFactCount_; ++index) {
            if (admittedFacts_[index].key != key)
                continue;
            for (std::size_t following = index + 1; following < admittedFactCount_; ++following)
                admittedFacts_[following - 1] = admittedFacts_[following];
            --admittedFactCount_;
            break;
        }
    }

    /** @brief Tracks only still-resident facts in bounded storage, including after memory eviction. */
    Result<void> PerceptionFilteredMemory::RememberFacts(const PerceptionFilterFacts &facts, const std::uint64_t simulationTick) {
        for (std::size_t index = 0; index < admittedFactCount_; ++index) {
            if (admittedFacts_[index].key == facts.key) {
                admittedFacts_[index] = facts;
                return Result<void>::Success();
            }
        }
        if (admittedFactCount_ == admittedFacts_.size()) {
            auto snapshot = memory_.Snapshot(simulationTick, {.isAlive = AssumeAliveForAdmission});
            if (snapshot.HasError())
                return Result<void>::Failure(snapshot.ErrorValue());
            std::size_t retained = 0;
            for (std::size_t index = 0; index < admittedFactCount_; ++index) {
                bool exists = false;
                for (std::size_t resident = 0; resident < snapshot.Value().count; ++resident)
                    exists |= admittedFacts_[index].key == snapshot.Value().entries[resident].key;
                if (exists)
                    admittedFacts_[retained++] = admittedFacts_[index];
            }
            admittedFactCount_ = retained;
        }
        if (admittedFactCount_ == admittedFacts_.size())
            return Result<void>::Failure(MakeError(AIErrors::PerceptionFilterInvalid));
        admittedFacts_[admittedFactCount_++] = facts;
        return Result<void>::Success();
    }

    /** @copydoc PerceptionFilteredMemory::Observe */
    Result<PerceptionFilterDecision> PerceptionFilteredMemory::Observe(const PerceptionObservation &observation,
                                                                       const PerceptionFilterFacts &facts,
                                                                       const std::uint64_t expectedRevision,
                                                                       const std::uint64_t simulationTick) {
        if (observation.key != facts.key)
            return Result<PerceptionFilterDecision>::Failure(MakeError(AIErrors::PerceptionFilterInvalid));
        auto decision = Evaluate(facts, expectedRevision);
        if (decision.HasError() || decision.Value().outcome == PerceptionFilterOutcome::StalePolicy)
            return decision;
        if (!decision.Value().Allowed()) {
            if (const auto advanced = memory_.AdvanceTo(simulationTick); advanced.HasError())
                return Result<PerceptionFilterDecision>::Failure(advanced.ErrorValue());
            if (const auto forgotten = memory_.Forget(facts.key); forgotten.HasError())
                return Result<PerceptionFilterDecision>::Failure(forgotten.ErrorValue());
            EraseFacts(facts.key);
            return decision;
        }
        if (const auto observed = memory_.Observe(observation, simulationTick); observed.HasError())
            return Result<PerceptionFilterDecision>::Failure(observed.ErrorValue());
        if (const auto remembered = RememberFacts(facts, simulationTick); remembered.HasError()) {
            (void)memory_.Forget(facts.key);
            return Result<PerceptionFilterDecision>::Failure(remembered.ErrorValue());
        }
        return decision;
    }

    /** @copydoc PerceptionFilteredMemory::RecheckFactsAtSafePoint */
    Result<PerceptionFilterDecision> PerceptionFilteredMemory::RecheckFactsAtSafePoint(const PerceptionFilterFacts &facts,
                                                                                       const std::uint64_t expectedRevision,
                                                                                       const std::uint64_t simulationTick) {
        auto decision = Evaluate(facts, expectedRevision);
        if (decision.HasError() || decision.Value().outcome == PerceptionFilterOutcome::StalePolicy)
            return decision;
        if (const auto advanced = memory_.AdvanceTo(simulationTick); advanced.HasError())
            return Result<PerceptionFilterDecision>::Failure(advanced.ErrorValue());
        if (!decision.Value().Allowed()) {
            if (const auto forgotten = memory_.Forget(facts.key); forgotten.HasError())
                return Result<PerceptionFilterDecision>::Failure(forgotten.ErrorValue());
            EraseFacts(facts.key);
            return decision;
        }
        for (std::size_t index = 0; index < admittedFactCount_; ++index) {
            if (admittedFacts_[index].key == facts.key) {
                admittedFacts_[index] = facts;
                break;
            }
        }
        return decision;
    }

    /** @copydoc PerceptionFilteredMemory::StagePolicy */
    Result<void> PerceptionFilteredMemory::StagePolicy(PerceptionFilterPolicy policy, const std::uint64_t expectedRevision) {
        if (!ValidPolicy(policy))
            return Result<void>::Failure(MakeError(AIErrors::PerceptionFilterInvalid));
        if (hasPendingPolicy_ || expectedRevision != policyRevision_ || policyRevision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(AIErrors::PerceptionFilterRevisionInvalid));
        pendingPolicy_ = std::move(policy);
        hasPendingPolicy_ = true;
        return Result<void>::Success();
    }

    /** @copydoc PerceptionFilteredMemory::CommitAtSafePoint */
    Result<PerceptionPolicyCommit> PerceptionFilteredMemory::CommitAtSafePoint(const std::uint64_t simulationTick) {
        if (!hasPendingPolicy_)
            return Result<PerceptionPolicyCommit>::Failure(MakeError(AIErrors::PerceptionFilterRevisionInvalid));
        if (const auto advanced = memory_.AdvanceTo(simulationTick); advanced.HasError())
            return Result<PerceptionPolicyCommit>::Failure(advanced.ErrorValue());
        if (pendingPolicy_ == policy_) {
            pendingPolicy_ = {};
            hasPendingPolicy_ = false;
            return Result<PerceptionPolicyCommit>::Success(
                {.policyRevision = policyRevision_, .simulationTick = simulationTick, .invalidatedMemoryEntries = 0, .changed = false});
        }
        auto snapshot = memory_.Snapshot(simulationTick, {.isAlive = AssumeAliveForAdmission});
        if (snapshot.HasError())
            return Result<PerceptionPolicyCommit>::Failure(snapshot.ErrorValue());
        std::array<PerceptionFilterFacts, MaximumPerceptionMemoryEntries> retainedFacts{};
        std::size_t retainedCount = 0;
        std::size_t invalidated = 0;
        for (std::size_t index = 0; index < snapshot.Value().count; ++index) {
            const auto key = snapshot.Value().entries[index].key;
            const PerceptionFilterFacts *facts = nullptr;
            for (std::size_t admitted = 0; admitted < admittedFactCount_; ++admitted) {
                if (admittedFacts_[admitted].key == key) {
                    facts = &admittedFacts_[admitted];
                    break;
                }
            }
            if (facts != nullptr && PolicyOutcome(pendingPolicy_, *facts) == PerceptionFilterOutcome::Allowed) {
                retainedFacts[retainedCount++] = *facts;
                continue;
            }
            if (const auto forgotten = memory_.Forget(key); forgotten.HasError())
                return Result<PerceptionPolicyCommit>::Failure(forgotten.ErrorValue());
            ++invalidated;
        }
        admittedFacts_ = retainedFacts;
        admittedFactCount_ = retainedCount;
        policy_ = pendingPolicy_;
        pendingPolicy_ = {};
        hasPendingPolicy_ = false;
        ++policyRevision_;
        return Result<PerceptionPolicyCommit>::Success({.policyRevision = policyRevision_,
                                                        .simulationTick = simulationTick,
                                                        .invalidatedMemoryEntries = invalidated,
                                                        .changed = true});
    }

    /** @copydoc PerceptionFilteredMemory::SnapshotForPublication */
    Result<PerceptionMemorySnapshot> PerceptionFilteredMemory::SnapshotForPublication(const std::uint64_t simulationTick,
                                                                                      const PerceptionSourceLiveness liveness) {
        return memory_.Snapshot(simulationTick, liveness);
    }

    /** @copydoc PerceptionFilteredMemory::PolicyRevision */
    std::uint64_t PerceptionFilteredMemory::PolicyRevision() const noexcept {
        return policyRevision_;
    }
}  // namespace Horo::AI
