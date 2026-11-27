#include "Horo/Navigation/NavigationCrowdDiagnostics.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Navigation {
    /** @copydoc SummarizeNavigationCrowd */
    NavigationCrowdCaptureMetrics SummarizeNavigationCrowd(const NavigationCrowdSnapshot &capture) noexcept {
        NavigationCrowdCaptureMetrics metrics{.activeAgents = capture.Agents().size(),
                                              .groups = capture.Cells().size(),
                                              .neighborFacts = capture.NeighborIndices().size()};
        for (const auto &profile : capture.ProfileTruncation()) {
            metrics.neighborsOmitted += profile.neighborsOmitted;
            metrics.boundariesOmitted += profile.boundariesOmitted;
        }
        return metrics;
    }

    /** @copydoc NavigationCrowdDebugSource::Retain */
    void NavigationCrowdDebugSource::Retain(const NavigationCrowdSnapshot &capture, const NavigationGeneration topology) noexcept {
        if (!capture.IsValid() || !topology.IsValid()) {
            Clear();
            return;
        }
        capture_ = capture;
        topology_ = topology;
    }

    /** @copydoc NavigationCrowdDebugSource::Clear */
    void NavigationCrowdDebugSource::Clear() noexcept {
        capture_ = {};
        topology_ = {};
    }

    /** @copydoc NavigationCrowdDebugSource::Stamp */
    std::optional<NavigationCrowdDebugStamp> NavigationCrowdDebugSource::Stamp() const noexcept {
        if (!capture_.IsValid())
            return std::nullopt;
        return NavigationCrowdDebugStamp{capture_.Binding(), topology_, capture_.DynamicRevision(), capture_.CaptureTick()};
    }

    /** @copydoc NavigationCrowdDebugSource::Read */
    Result<NavigationCrowdDebugSnapshot> NavigationCrowdDebugSource::Read(const NavigationCrowdDebugStamp &current,
                                                                          const NavigationCrowdDebugLimits limits) const {
        using Read = Result<NavigationCrowdDebugSnapshot>;
        if (!limits.IsValid())
            return Read::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
        if (Stamp() != current)
            return Read::Failure(MakeError(NavigationErrors::StaleSnapshot));
        try {
            NavigationCrowdDebugSnapshot result{.stamp = current, .metrics = SummarizeNavigationCrowd(capture_)};
            const auto cellCount = std::min(limits.groups, capture_.Cells().size());
            result.groups.reserve(cellCount);
            result.agents.reserve(std::min(limits.agents, capture_.Agents().size()));
            for (const auto &cell : capture_.Cells().first(cellCount)) {
                const auto shown = std::min<std::size_t>(cell.agentCount, limits.agents - result.agents.size());
                result.groups.emplace_back(cell.x, cell.z, result.agents.size(), shown, cell.agentCount);
                for (const auto index : capture_.CellAgentIndices().subspan(cell.firstAgent, shown)) {
                    const auto &agent = capture_.Agents()[index];
                    result.agents.emplace_back(agent.handle, agent.position, agent.velocity, agent.neighborCount, agent.truncatedNeighbors,
                                               agent.truncatedBoundaries);
                }
            }
            result.truncated = result.groups.size() != result.metrics.groups || result.agents.size() != result.metrics.activeAgents;
            return Read::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Read::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Navigation
