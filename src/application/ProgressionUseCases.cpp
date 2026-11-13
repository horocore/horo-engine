#include "Horo/Application/ProgressionUseCases.h"

#include <array>
#include <new>
#include <utility>

namespace Horo::Application {
    using namespace PlatformServices;

    /** @copydoc FrontendProgressionProjectionGate::FrontendProgressionProjectionGate */
    FrontendProgressionProjectionGate::FrontendProgressionProjectionGate(const PlatformServicesFrontend &frontend,
                                                                         const IProgressionProjectionGate &semantics) noexcept
        : frontend_(frontend), semantics_(semantics) {}

    /** @copydoc FrontendProgressionProjectionGate::Validate */
    Result<void> FrontendProgressionProjectionGate::Validate(const PlatformProgressionMutationEnvelope &envelope) const {
        if (!envelope.IsValid())
            return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        const auto service = envelope.definitionKind == PlatformServiceIdKind::Achievement ? PlatformServiceKind::Achievements
                                                                                           : PlatformServiceKind::LeaderboardsAndStats;
        const auto available = frontend_.ServiceLimits(service);
        if (available.HasError())
            return Result<void>::Failure(available.ErrorValue());
        return semantics_.Validate(envelope);
    }

    namespace {
        /** @brief Enforces the registered representation and inclusive portable range without signed conversion overflow. */
        bool InRange(const PlatformProgressionValue &value, ProgressionValueKind kind, const ProgressionNumericRange &range) {
            if (kind == ProgressionValueKind::SignedInteger64) {
                const auto *number = std::get_if<std::int64_t>(&value);
                return number && *number >= range.minimum && *number <= range.maximum;
            }
            const auto *number = std::get_if<std::uint64_t>(&value);
            return number && range.minimum >= 0 && *number >= static_cast<std::uint64_t>(range.minimum) &&
                   *number <= static_cast<std::uint64_t>(range.maximum);
        }
    }  // namespace

    ProgressionUseCases::ProgressionUseCases(ProgressionDefinitions definitions, PlatformProgressionSessionScope scope,
                                             PlatformProgressionPolicyRevision policy, const IProgressionFactAuthority &authority,
                                             const IProgressionProjectionGate &gate, PlatformProgressionCoalescer &coalescer,
                                             const ProgressionUseCasesConfig config)
        : definitions_(std::move(definitions)), scope_(scope), policy_(policy), authority_(authority), gate_(gate), coalescer_(coalescer),
          config_(config), facts_(std::make_unique<std::vector<AcceptedFact>>()) {
        facts_->reserve(config_.maximumAcceptedFacts);
    }

    /** @copydoc ProgressionUseCases::ProgressionUseCases */
    ProgressionUseCases::ProgressionUseCases(ProgressionUseCases &&other) noexcept
        : definitions_(std::move(other.definitions_)), scope_(other.scope_), policy_(other.policy_), authority_(other.authority_),
          gate_(other.gate_), coalescer_(other.coalescer_), config_(other.config_), facts_(std::move(other.facts_)),
          closed_(std::exchange(other.closed_, true)) {}

    /** @copydoc ProgressionUseCases::Create */
    Result<ProgressionUseCases> ProgressionUseCases::Create(ProgressionDefinitions definitions, PlatformProgressionSessionScope scope,
                                                            PlatformProgressionPolicyRevision policy,
                                                            const IProgressionFactAuthority &authority,
                                                            const IProgressionProjectionGate &gate, PlatformProgressionCoalescer &coalescer,
                                                            const ProgressionUseCasesConfig config) {
        if (!definitions.identities || !definitions.achievements || !definitions.stats || !definitions.leaderboards || !scope.IsValid() ||
            !policy.IsValid() || config.maximumAcceptedFacts == 0 ||
            config.maximumAcceptedFacts > PlatformProgressionMaximumInFlightMutations)
            return Result<ProgressionUseCases>::Failure(MakeError(PlatformProgressionErrors::InvalidConfiguration));
        const auto &ids = *definitions.identities;
        if (definitions.achievements->StableIdProjectId() != ids.ProjectId() || definitions.stats->StableIdProjectId() != ids.ProjectId() ||
            definitions.leaderboards->StableIdProjectId() != ids.ProjectId() ||
            definitions.achievements->StableIdRegistryFingerprint() != ids.Fingerprint() ||
            definitions.stats->StableIdRegistryFingerprint() != ids.Fingerprint() ||
            definitions.leaderboards->StableIdRegistryFingerprint() != ids.Fingerprint())
            return Result<ProgressionUseCases>::Failure(MakeError(PlatformProgressionErrors::InvalidConfiguration));
        try {
            return Result<ProgressionUseCases>::Success(
                ProgressionUseCases(std::move(definitions), scope, policy, authority, gate, coalescer, config));
        } catch (const std::bad_alloc &) {
            return Result<ProgressionUseCases>::Failure(MakeError(PlatformProgressionErrors::CapacityExceeded));
        }
    }

    /** @copydoc ProgressionUseCases::ApplyDefinition */
    Result<LeaderboardOrdering> ProgressionUseCases::ApplyDefinition(const ProgressionFact &fact,
                                                                     PlatformProgressionMutationCandidate &candidate) const {
        using Return = Result<LeaderboardOrdering>;
        if (const auto *id = std::get_if<AchievementId>(&fact.definition)) {
            const auto found = definitions_.achievements->Find(*id);
            if (found.HasError())
                return Return::Failure(found.ErrorValue());
            const auto &definition = *found.Value();
            candidate.definitionKind = PlatformServiceIdKind::Achievement;
            candidate.definition = {id->value};
            candidate.authority = definition.authority;
            candidate.kind = definition.progress.kind == AchievementProgressKind::UnlockOnce
                                 ? PlatformProgressionMutationKind::UnlockOnce
                                 : PlatformProgressionMutationKind::SetProgressMaximum;
            if (candidate.kind == PlatformProgressionMutationKind::SetProgressMaximum) {
                const auto *progress = std::get_if<std::uint64_t>(&fact.value);
                if (!progress || *progress > definition.progress.total)
                    return Return::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
            }
            return Return::Success(LeaderboardOrdering::HighestFirst);
        }
        if (const auto *id = std::get_if<StatId>(&fact.definition)) {
            const auto found = definitions_.stats->Find(*id);
            if (found.HasError())
                return Return::Failure(found.ErrorValue());
            const auto &definition = *found.Value();
            if (!InRange(fact.value, definition.valueKind, definition.range))
                return Return::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
            constexpr std::array kinds{PlatformProgressionMutationKind::SetStatMaximum, PlatformProgressionMutationKind::SetStatMinimum,
                                       PlatformProgressionMutationKind::SetStatSnapshot, PlatformProgressionMutationKind::AddStatOnce};
            candidate.definitionKind = PlatformServiceIdKind::Stat;
            candidate.definition = {id->value};
            candidate.authority = definition.authority;
            candidate.kind = kinds.at(static_cast<std::size_t>(definition.mutation));
            return Return::Success(LeaderboardOrdering::HighestFirst);
        }
        const auto id = std::get<LeaderboardId>(fact.definition);
        const auto found = definitions_.leaderboards->Find(id);
        if (found.HasError())
            return Return::Failure(found.ErrorValue());
        const auto &definition = *found.Value();
        if (!InRange(fact.value, definition.valueKind, definition.range))
            return Return::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        candidate.definitionKind = PlatformServiceIdKind::Leaderboard;
        candidate.definition = {id.value};
        candidate.authority = definition.authority;
        candidate.kind = fact.expectedRevision ? PlatformProgressionMutationKind::ReplaceScoreAtRevision
                                               : PlatformProgressionMutationKind::SubmitBestScore;
        return Return::Success(definition.ordering);
    }

    /** @copydoc ProgressionUseCases::PrepareFact */
    Result<ProgressionUseCases::PreparedIntent> ProgressionUseCases::PrepareFact(const ProgressionFact &fact) const {
        if (closed_)
            return Result<PreparedIntent>::Failure(MakeError(PlatformProgressionErrors::Closed));
        PlatformProgressionMutationCandidate candidate{.occurrence = fact.occurrence,
                                                       .scope = scope_,
                                                       .value = fact.value,
                                                       .expectedRevision = fact.expectedRevision,
                                                       .policy = policy_};
        const auto definition = ApplyDefinition(fact, candidate);
        if (definition.HasError())
            return Result<PreparedIntent>::Failure(definition.ErrorValue());
        const auto envelope = BuildPlatformProgressionMutationEnvelope(candidate);
        if (envelope.HasError())
            return Result<PreparedIntent>::Failure(envelope.ErrorValue());
        const auto authorized = authority_.Authorize(candidate);
        if (authorized.HasError())
            return Result<PreparedIntent>::Failure(authorized.ErrorValue());
        const auto supported = gate_.Validate(envelope.Value());
        if (supported.HasError())
            return Result<PreparedIntent>::Failure(supported.ErrorValue());
        return Result<PreparedIntent>::Success({envelope.Value(), definition.Value()});
    }

    /** @copydoc ProgressionUseCases::CheckFact */
    Result<ProgressionUseCases::FactDisposition> ProgressionUseCases::CheckFact(
        const ProgressionFact &fact, const PreparedIntent &intent, const DeliveryOwner delivery,
        const std::optional<PlatformProgressionReplayRequirements> &requirements) const {
        for (const auto &accepted : *facts_) {
            if (accepted.occurrence != fact.occurrence || accepted.intent.envelope.definitionKind != intent.envelope.definitionKind ||
                accepted.intent.envelope.definition != intent.envelope.definition)
                continue;
            if (accepted.intent.envelope != intent.envelope || accepted.intent.ordering != intent.ordering ||
                accepted.delivery != delivery || accepted.requirements != requirements)
                return Result<FactDisposition>::Failure(MakeError(PlatformProgressionErrors::IdempotencyConflict));
            return Result<FactDisposition>::Success(FactDisposition::Existing);
        }
        if (facts_->size() == config_.maximumAcceptedFacts)
            return Result<FactDisposition>::Failure(MakeError(PlatformProgressionErrors::CapacityExceeded));
        return Result<FactDisposition>::Success(FactDisposition::New);
    }

    /** @copydoc ProgressionUseCases::Submit */
    Result<PlatformProgressionAdmission> ProgressionUseCases::Submit(const ProgressionFact &fact, const std::uint64_t tick) {
        const auto prepared = PrepareFact(fact);
        if (prepared.HasError())
            return Result<PlatformProgressionAdmission>::Failure(prepared.ErrorValue());
        const auto factBinding = CheckFact(fact, prepared.Value(), DeliveryOwner::Volatile, std::nullopt);
        if (factBinding.HasError())
            return Result<PlatformProgressionAdmission>::Failure(factBinding.ErrorValue());
        if (factBinding.Value() == FactDisposition::New)
            facts_->push_back({fact.occurrence, prepared.Value(), DeliveryOwner::Volatile, std::nullopt});
        return coalescer_.Admit(prepared.Value().envelope, prepared.Value().ordering, tick);
    }

    /** @copydoc ProgressionUseCases::PrepareDurableIntent */
    Result<PlatformProgressionQualifiedReplayIntent> ProgressionUseCases::PrepareDurableIntent(
        const ProgressionFact &fact, const PlatformProgressionReplayRequirements &requirements,
        const IPlatformProgressionReplayQualifier &qualifier) {
        const auto prepared = PrepareFact(fact);
        if (prepared.HasError())
            return Result<PlatformProgressionQualifiedReplayIntent>::Failure(prepared.ErrorValue());
        Sha256Digest definitionFingerprint{};
        switch (prepared.Value().envelope.definitionKind) {
            case PlatformServiceIdKind::Achievement:
                definitionFingerprint = definitions_.achievements->Fingerprint();
                break;
            case PlatformServiceIdKind::Stat:
                definitionFingerprint = definitions_.stats->Fingerprint();
                break;
            case PlatformServiceIdKind::Leaderboard:
                definitionFingerprint = definitions_.leaderboards->Fingerprint();
                break;
            case PlatformServiceIdKind::PresenceStatus:
                return Result<PlatformProgressionQualifiedReplayIntent>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        }
        if (!requirements.IsValid() || requirements.stableIdRegistryFingerprint != definitions_.identities->Fingerprint() ||
            requirements.definitionRegistryFingerprint != definitionFingerprint || requirements.policy != policy_ ||
            requirements.providerGeneration != scope_.provider)
            return Result<PlatformProgressionQualifiedReplayIntent>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        const auto factBinding = CheckFact(fact, prepared.Value(), DeliveryOwner::Durable, requirements);
        if (factBinding.HasError())
            return Result<PlatformProgressionQualifiedReplayIntent>::Failure(factBinding.ErrorValue());
        if (factBinding.Value() == FactDisposition::New)
            facts_->push_back({fact.occurrence, prepared.Value(), DeliveryOwner::Durable, requirements});
        auto qualified = qualifier.Qualify(prepared.Value().envelope, requirements, prepared.Value().ordering);
        if (qualified.HasError())
            return qualified;
        if (qualified.Value().Envelope() != prepared.Value().envelope || qualified.Value().Requirements() != requirements ||
            qualified.Value().Ordering() != prepared.Value().ordering)
            return Result<PlatformProgressionQualifiedReplayIntent>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        return qualified;
    }

    /** @copydoc ProgressionUseCases::SubmitByKey */
    Result<PlatformProgressionAdmission> ProgressionUseCases::SubmitByKey(const PlatformServiceIdKind kind, const std::string_view key,
                                                                          ProgressionFact fact, const std::uint64_t tick) {
        if (closed_)
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::Closed));
        if (kind != PlatformServiceIdKind::Achievement && kind != PlatformServiceIdKind::Stat && kind != PlatformServiceIdKind::Leaderboard)
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        const auto resolved = definitions_.identities->Resolve(kind, key);
        if (resolved.HasError())
            return Result<PlatformProgressionAdmission>::Failure(resolved.ErrorValue());
        if (kind == PlatformServiceIdKind::Achievement)
            fact.definition = AchievementId{resolved.Value().value};
        else if (kind == PlatformServiceIdKind::Stat)
            fact.definition = StatId{resolved.Value().value};
        else
            fact.definition = LeaderboardId{resolved.Value().value};
        return Submit(fact, tick);
    }

    /** @copydoc ProgressionUseCases::Cancel */
    Result<void> ProgressionUseCases::Cancel(const PlatformProgressionMutationId mutation) {
        if (closed_)
            return Result<void>::Failure(MakeError(PlatformProgressionErrors::Closed));
        // Retained identities never change scope or disappear, so the check remains valid across a concurrent completion.
        if (!coalescer_.Query(mutation, scope_).has_value())
            return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        return coalescer_.Cancel(mutation);
    }

    /** @copydoc ProgressionUseCases::Query */
    std::optional<PlatformProgressionReceiptSnapshot> ProgressionUseCases::Query(const PlatformProgressionMutationId mutation) const {
        if (closed_)
            return std::nullopt;
        return coalescer_.Query(mutation, scope_);
    }
}  // namespace Horo::Application
