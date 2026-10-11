#include "Horo/AI/PerceptionFiltering.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>

namespace Horo::AI {
    /** @copydoc ValidatePerceptionFiltering */
    Result<void> ValidatePerceptionFiltering(const PerceptionFilteringPolicy &policy) {
        if (policy.relationshipCount > policy.relationships.size())
            return Result<void>::Failure(MakeError(AIErrors::PerceptionDescriptorLimitExceeded));
        for (std::size_t index = 0; index < policy.relationshipCount; ++index) {
            const auto &entry = policy.relationships[index];
            if (entry.affiliation.kind >= PerceptionAffiliation::Count ||
                (entry.affiliation.kind == PerceptionAffiliation::Custom) != (entry.affiliation.custom.value != 0))
                return Result<void>::Failure(MakeError(AIErrors::PerceptionDescriptorInvalid));
            for (std::size_t prior = 0; prior < index; ++prior)
                if (policy.relationships[prior].listener == entry.listener && policy.relationships[prior].source == entry.source)
                    return Result<void>::Failure(MakeError(AIErrors::PerceptionDescriptorConflict));
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidatePerceptionParticipant */
    Result<void> ValidatePerceptionParticipant(const PerceptionParticipantFilter &participant, const PerceptionListenerFilter &listener) {
        if (participant.layers == 0 || participant.visibility >= PerceptionGameplayVisibility::Count || listener.affiliations > 7 ||
            listener.teams >= PerceptionTeamFilter::Count || listener.customCount > listener.custom.size())
            return Result<void>::Failure(MakeError(AIErrors::PerceptionDescriptorInvalid));
        for (std::size_t index = 0; index < listener.customCount; ++index) {
            if (listener.custom[index].value == 0)
                return Result<void>::Failure(MakeError(AIErrors::PerceptionDescriptorInvalid));
            for (std::size_t prior = 0; prior < index; ++prior)
                if (listener.custom[prior] == listener.custom[index])
                    return Result<void>::Failure(MakeError(AIErrors::PerceptionDescriptorConflict));
        }
        return Result<void>::Success();
    }

    /** @copydoc AllowsPerception */
    bool AllowsPerception(const PerceptionFilteringPolicy &policy, const PerceptionParticipantFilter &listener,
                          const PerceptionParticipantFilter &source, const PerceptionListenerFilter &filter) noexcept {
        if (policy.relationshipCount > policy.relationships.size() || filter.customCount > filter.custom.size() ||
            filter.affiliations > 7 || filter.teams >= PerceptionTeamFilter::Count || listener.layers == 0 || source.layers == 0 ||
            source.visibility >= PerceptionGameplayVisibility::Count || listener.visibility >= PerceptionGameplayVisibility::Count ||
            (listener.mask & source.layers) == 0 || (source.mask & listener.layers) == 0 ||
            source.visibility == PerceptionGameplayVisibility::Hidden)
            return false;
        const bool sameTeam = listener.team.value != 0 && listener.team == source.team;
        if ((source.visibility == PerceptionGameplayVisibility::TeamOnly || filter.teams == PerceptionTeamFilter::Same) && !sameTeam)
            return false;
        if (filter.teams == PerceptionTeamFilter::Different && sameTeam)
            return false;
        PerceptionAffiliationDescriptor affiliation{.kind = sameTeam ? PerceptionAffiliation::Friendly
                                                                     : (listener.team.value == 0 || source.team.value == 0
                                                                            ? PerceptionAffiliation::Neutral
                                                                            : PerceptionAffiliation::Hostile)};
        for (std::size_t index = 0; index < policy.relationshipCount; ++index) {
            const auto &entry = policy.relationships[index];
            if (entry.listener == listener.team && entry.source == source.team) {
                affiliation = entry.affiliation;
                break;
            }
        }
        if (affiliation.kind == PerceptionAffiliation::Custom)
            return affiliation.custom.value != 0 && std::find(filter.custom.begin(), filter.custom.begin() + filter.customCount,
                                                              affiliation.custom) != filter.custom.begin() + filter.customCount;
        return affiliation.kind < PerceptionAffiliation::Custom && affiliation.custom.value == 0 &&
               (filter.affiliations & (std::uint8_t{1} << static_cast<std::uint8_t>(affiliation.kind))) != 0;
    }
}  // namespace Horo::AI
