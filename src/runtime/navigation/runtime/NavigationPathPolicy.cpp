#include "Horo/Navigation/NavigationPathPolicy.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Navigation {
    namespace {
        /** @brief Normalize one changed dependency into Horo-owned revision evidence. */
        NavigationPathInvalidation Change(const NavigationPathInvalidationKind kind, const std::uint64_t expected,
                                          const std::uint64_t observed, const std::uint64_t dependency = 0) noexcept {
            return {.kind = kind, .dependency = dependency, .expected = expected, .observed = observed};
        }

        /** @brief Reject a path whose status or canonical corridor identities contradict its source. */
        bool ValidCorridor(const NavigationPath &path, const NavigationOutcomeProvenance &provenance,
                           const NavigationCoverageEvidence &coverage) noexcept {
            if (path.sourceGeneration != provenance.topology ||
                (path.status != NavigationPathStatus::Reachable && path.status != NavigationPathStatus::Partial))
                return false;
            if ((path.status == NavigationPathStatus::Reachable && !coverage.IsComplete()) ||
                (path.status == NavigationPathStatus::Partial && coverage.IsComplete()))
                return false;
            for (const auto &polygon : path.corridor) {
                if (polygon.provenance.world != provenance.world || polygon.provenance.topology != provenance.topology)
                    return false;
            }
            for (const auto &waypoint : path.waypoints) {
                if (waypoint.provenance.polygon.world != provenance.world || waypoint.provenance.polygon.topology != provenance.topology)
                    return false;
            }
            return true;
        }

        /** @brief Check one bounded covered-or-missing dependency set against the current sorted region slice. */
        NavigationPathInvalidation ChangedRegion(const std::span<const NavigationCoverageDependency> dependencies,
                                                 const std::span<const NavigationCoverageDependency> current) noexcept {
            for (const auto &dependency : dependencies) {
                const auto found = std::ranges::lower_bound(current, dependency.region, {}, &NavigationCoverageDependency::region);
                if (found == current.end() || found->region != dependency.region || found->generation != dependency.generation)
                    return Change(NavigationPathInvalidationKind::Tile, dependency.generation.Value(),
                                  found == current.end() || found->region != dependency.region ? 0 : found->generation.Value(),
                                  dependency.region);
            }
            return {};
        }
    }  // namespace

    /** @copydoc NavigationPathPolicy::NavigationPathPolicy */
    NavigationPathPolicy::NavigationPathPolicy(const NavigationRepathPolicy policy) noexcept : policy_(policy) {}

    /** @brief Reject malformed or over-bound atomic observations. */
    bool NavigationPathPolicy::ValidObservation(const NavigationPathObservation &current) noexcept {
        if (!ValidateNavigationOutcomeProvenance(current.provenance) || current.linkRevision == 0 || current.goalRevision == 0 ||
            current.regions.size() > MaximumNavigationOutcomeCoverageDependencies)
            return false;
        std::uint64_t previousRegion = 0;
        for (const auto &region : current.regions) {
            if (region.region <= previousRegion || !region.generation.IsValid())
                return false;
            previousRegion = region.region;
        }
        return true;
    }

    /** @brief Reject time or same-world revision rollback after an earlier owner observation. */
    bool NavigationPathPolicy::ValidAdvance(const NavigationPathObservation &current) const noexcept {
        if (lastObservedTick_ && current.tick < *lastObservedTick_)
            return false;
        if (!lastObservedProvenance_ || lastObservedProvenance_->world != current.provenance.world)
            return true;
        const auto &previous = *lastObservedProvenance_;
        const auto &now = current.provenance;
        return now.topology.Value() >= previous.topology.Value() && now.obstacleRevision >= previous.obstacleRevision &&
               now.filterRevision >= previous.filterRevision && now.profileRevision >= previous.profileRevision &&
               now.originRevision >= previous.originRevision && current.linkRevision >= lastObservedLinkRevision_ &&
               current.goalRevision >= lastObservedGoalRevision_;
    }

    /** @brief Find the first changed source dependency in deterministic precedence. */
    NavigationPathInvalidation NavigationPathPolicy::Compare(const Held &held, const NavigationPathObservation &current) noexcept {
        const auto &source = held.provenance;
        const auto &now = current.provenance;
        if (source.world != now.world)
            return Change(NavigationPathInvalidationKind::World, source.world.Value(), now.world.Value());
        if (source.filterRevision != now.filterRevision)
            return Change(NavigationPathInvalidationKind::Filter, source.filterRevision, now.filterRevision);
        if (source.profileRevision != now.profileRevision)
            return Change(NavigationPathInvalidationKind::Profile, source.profileRevision, now.profileRevision);
        if (source.originRevision != now.originRevision)
            return Change(NavigationPathInvalidationKind::Origin, source.originRevision, now.originRevision);
        if (source.obstacleRevision != now.obstacleRevision)
            return Change(NavigationPathInvalidationKind::Obstacle, source.obstacleRevision, now.obstacleRevision);
        if (held.linkRevision != current.linkRevision)
            return Change(NavigationPathInvalidationKind::Link, held.linkRevision, current.linkRevision);

        if (held.coverage.Scope() == NavigationCoverageScope::ExactRegions) {
            if (const auto changed = ChangedRegion(held.coverage.Covered(), current.regions); changed.IsStale())
                return changed;
            if (const auto changed = ChangedRegion(held.coverage.Missing(), current.regions); changed.IsStale())
                return changed;
        }
        // Corridor polygon indices are scoped to a whole topology generation even if exact regions survived.
        if (source.topology != now.topology)
            return Change(NavigationPathInvalidationKind::Topology, source.topology.Value(), now.topology.Value());
        if (held.goalRevision != current.goalRevision)
            return Change(NavigationPathInvalidationKind::Target, held.goalRevision, current.goalRevision);
        return {};
    }

    /** @brief Replace the single pending intent with the newest observed goal and source. */
    void NavigationPathPolicy::Queue(const NavigationPathInvalidation cause, const NavigationPathObservation &current,
                                     const bool forced) noexcept {
        const bool wasForced = pending_.has_value() && pending_->forced;
        pending_ = Pending{.cause = cause,
                           .goalRevision = current.goalRevision,
                           .sourceTopology = current.provenance.topology.Value(),
                           .changedTick = current.tick,
                           .forced = forced || wasForced};
    }

    /** @copydoc NavigationPathPolicy::Install */
    Result<void> NavigationPathPolicy::Install(const PathId id, NavigationPath path, NavigationOutcomeProvenance provenance,
                                               NavigationCoverageEvidence coverage, const std::uint64_t linkRevision,
                                               const std::uint64_t goalRevision, const NavigationPathObservation &current) {
        if (!id.IsValid() || id.world != provenance.world || lastInvalidation_.kind == NavigationPathInvalidationKind::World ||
            (lastObservedProvenance_ && lastObservedProvenance_->world != current.provenance.world) ||
            (held_ && (held_->id == id || held_->id.world != id.world)) || !ValidObservation(current) || !ValidAdvance(current) ||
            !ValidateNavigationOutcomeProvenance(provenance) || linkRevision == 0 || goalRevision == 0 ||
            !ValidCorridor(path, provenance, coverage))
            return Result<void>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        Held candidate{.id = id,
                       .path = std::move(path),
                       .provenance = provenance,
                       .coverage = std::move(coverage),
                       .linkRevision = linkRevision,
                       .goalRevision = goalRevision};
        if (Compare(candidate, current).IsStale())
            return Result<void>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        held_ = std::move(candidate);
        pending_.reset();
        lastInvalidation_ = {};
        lastObservedTick_ = current.tick;
        lastObservedProvenance_ = current.provenance;
        lastObservedLinkRevision_ = current.linkRevision;
        lastObservedGoalRevision_ = current.goalRevision;
        lastObservedTopology_ = current.provenance.topology.Value();
        return Result<void>::Success();
    }

    /** @copydoc NavigationPathPolicy::Observe */
    Result<NavigationPathInvalidation> NavigationPathPolicy::Observe(const NavigationPathObservation &current) {
        if (!ValidObservation(current) || !ValidAdvance(current))
            return Result<NavigationPathInvalidation>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        NavigationPathInvalidation compared;
        if (held_)
            compared = Compare(*held_, current);
        else if (lastObservedProvenance_ && lastObservedProvenance_->world != current.provenance.world)
            compared =
                Change(NavigationPathInvalidationKind::World, lastObservedProvenance_->world.Value(), current.provenance.world.Value());
        // Once observed stale, the old corridor cannot regain currency through revision rollback or a reused snapshot.
        const auto changed = compared.IsStale() ? compared : lastInvalidation_;
        const bool newSource = lastObservedTopology_ != 0 && lastObservedTopology_ != current.provenance.topology.Value();
        const bool newGoal = lastObservedGoalRevision_ != 0 && lastObservedGoalRevision_ != current.goalRevision;
        if (changed.kind == NavigationPathInvalidationKind::World) {
            pending_.reset();
        } else if (changed.IsStale()) {
            if (changed != lastInvalidation_ || newSource || newGoal ||
                (pending_ &&
                 (pending_->goalRevision != current.goalRevision || pending_->sourceTopology != current.provenance.topology.Value())))
                Queue(changed, current, false);
        } else if (!held_ && newGoal) {
            Queue(Change(NavigationPathInvalidationKind::Target, lastObservedGoalRevision_, current.goalRevision), current, false);
        }
        lastInvalidation_ = changed;
        lastObservedTick_ = current.tick;
        lastObservedProvenance_ = current.provenance;
        lastObservedLinkRevision_ = current.linkRevision;
        lastObservedGoalRevision_ = current.goalRevision;
        lastObservedTopology_ = current.provenance.topology.Value();
        return Result<NavigationPathInvalidation>::Success(changed);
    }

    /** @copydoc NavigationPathPolicy::CurrentPath */
    const NavigationPath *NavigationPathPolicy::CurrentPath(const NavigationPathObservation &current) {
        const auto observed = Observe(current);
        if (observed.HasError() || observed.Value().IsStale() || !held_)
            return nullptr;
        return &held_->path;
    }

    /** @copydoc NavigationPathPolicy::ForceRepath */
    Result<void> NavigationPathPolicy::ForceRepath(const NavigationPathObservation &current) {
        if (!ValidObservation(current) || !ValidAdvance(current))
            return Result<void>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        const auto observed = Observe(current);
        if (observed.HasError())
            return Result<void>::Failure(observed.ErrorValue());
        if (observed.Value().kind == NavigationPathInvalidationKind::World)
            return Result<void>::Failure(MakeError(NavigationErrors::InvalidWorld));
        Queue({}, current, true);
        return Result<void>::Success();
    }

    /** @copydoc NavigationPathPolicy::Decide */
    NavigationRepathDecision NavigationPathPolicy::Decide(const std::uint64_t tick) const noexcept {
        if (!pending_ || !lastObservedTick_ || tick != *lastObservedTick_)
            return {};
        NavigationRepathDecision decision{.action = NavigationRepathAction::Submit,
                                          .cause = pending_->cause,
                                          .goalRevision = pending_->goalRevision,
                                          .sourceTopology = pending_->sourceTopology,
                                          .forced = pending_->forced};
        if (!pending_->forced) {
            if (tick < pending_->changedTick || tick - pending_->changedTick < policy_.debounceTicks)
                decision.action = NavigationRepathAction::Debouncing;
            else if (lastSubmittedTick_ && (tick < *lastSubmittedTick_ || tick - *lastSubmittedTick_ < policy_.cooldownTicks))
                decision.action = NavigationRepathAction::CoolingDown;
        }
        return decision;
    }

    /** @copydoc NavigationPathPolicy::MarkSubmitted */
    bool NavigationPathPolicy::MarkSubmitted(const std::uint64_t tick) noexcept {
        if (Decide(tick).action != NavigationRepathAction::Submit)
            return false;
        lastSubmittedTick_ = tick;
        pending_.reset();
        return true;
    }

    /** @copydoc NavigationPathPolicy::Clear */
    void NavigationPathPolicy::Clear() noexcept {
        held_.reset();
        pending_.reset();
        lastInvalidation_ = {};
        lastSubmittedTick_.reset();
        lastObservedTick_.reset();
        lastObservedProvenance_.reset();
        lastObservedLinkRevision_ = 0;
        lastObservedGoalRevision_ = 0;
        lastObservedTopology_ = 0;
    }

    /** @copydoc NavigationPathPolicy::Id */
    PathId NavigationPathPolicy::Id() const noexcept {
        return held_ ? held_->id : PathId{};
    }

    /** @copydoc NavigationPathPolicy::LastInvalidation */
    NavigationPathInvalidation NavigationPathPolicy::LastInvalidation() const noexcept {
        return lastInvalidation_;
    }
}  // namespace Horo::Navigation
