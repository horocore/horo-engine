#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationCrowdSnapshotInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

namespace Horo::Navigation::Detail {
    namespace {
        struct Candidate final {
            double distanceSquared{};
            std::uint32_t index{};
        };

        [[nodiscard]] Result<void> CapacityExceeded() {
            return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }

        [[nodiscard]] const NavigationCrowdProfileFacts &ProfileFor(const NavigationCrowdSnapshotStorage &storage,
                                                                    const NavigationAgentProfileId profile) {
            return *std::ranges::lower_bound(storage.profiles, profile, {}, &NavigationCrowdProfileFacts::profile);
        }

        [[nodiscard]] NavigationCrowdProfileTruncation &TruncationFor(NavigationCrowdSnapshotStorage &storage,
                                                                      const NavigationAgentProfileId profile) {
            return *std::ranges::lower_bound(storage.truncation, profile, {}, &NavigationCrowdProfileTruncation::profile);
        }

        [[nodiscard]] std::pair<std::vector<NavigationCrowdCell>::const_iterator, std::vector<NavigationCrowdCell>::const_iterator>
        AgentCellRange(const NavigationCrowdSnapshotStorage &storage, const std::int32_t firstX, const std::int32_t lastX) {
            return {std::ranges::lower_bound(storage.cells, firstX, {}, &NavigationCrowdCell::x),
                    std::ranges::upper_bound(storage.cells, lastX, {}, &NavigationCrowdCell::x)};
        }

        [[nodiscard]] std::pair<std::vector<NavigationCrowdCellEntry>::const_iterator,
                                std::vector<NavigationCrowdCellEntry>::const_iterator>
        BoundaryCellRange(const NavigationCrowdSnapshotStorage &storage, const std::int32_t firstX, const std::int32_t lastX) {
            const auto &entries = storage.boundaryCellEntries;
            return {std::ranges::lower_bound(entries, firstX, {}, &NavigationCrowdCellEntry::x),
                    std::ranges::upper_bound(entries, lastX, {}, &NavigationCrowdCellEntry::x)};
        }

        [[nodiscard]] double PointSegmentDistanceSquared(const Math::Vec3 point, const NavigationCrowdBoundarySegment &segment) noexcept {
            const double edgeX = static_cast<double>(segment.second.x) - segment.first.x;
            const double edgeZ = static_cast<double>(segment.second.z) - segment.first.z;
            const double pointX = static_cast<double>(point.x) - segment.first.x;
            const double pointZ = static_cast<double>(point.z) - segment.first.z;
            const double lengthSquared = edgeX * edgeX + edgeZ * edgeZ;
            const double along = std::clamp((pointX * edgeX + pointZ * edgeZ) / lengthSquared, 0.0, 1.0);
            const double distanceX = pointX - along * edgeX;
            const double distanceZ = pointZ - along * edgeZ;
            return distanceX * distanceX + distanceZ * distanceZ;
        }

        [[nodiscard]] Result<void> CellWindow(const NavigationCrowdAgentFact &agent, const float radius,
                                              const NavigationCrowdSnapshotLimits &limits, std::int32_t &firstX, std::int32_t &lastX,
                                              std::int32_t &firstZ, std::int32_t &lastZ) {
            if (!CrowdCellCoordinate(static_cast<double>(agent.position.x) - radius, limits.cellSizeMeters, firstX) ||
                !CrowdCellCoordinate(static_cast<double>(agent.position.x) + radius, limits.cellSizeMeters, lastX) ||
                !CrowdCellCoordinate(static_cast<double>(agent.position.z) - radius, limits.cellSizeMeters, firstZ) ||
                !CrowdCellCoordinate(static_cast<double>(agent.position.z) + radius, limits.cellSizeMeters, lastZ))
                return Result<void>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> GatherNeighbors(const std::uint32_t agentIndex, const NavigationCrowdProfileFacts &profile,
                                                   const NavigationCrowdSnapshotLimits &limits, NavigationCrowdSnapshotStorage &storage,
                                                   std::size_t &pairChecks, std::vector<Candidate> &candidates) {
            auto &agent = storage.agents[agentIndex];
            candidates.clear();
            std::int32_t firstX{};
            std::int32_t lastX{};
            std::int32_t firstZ{};
            std::int32_t lastZ{};
            if (const auto window = CellWindow(agent, profile.neighborRadiusMeters, limits, firstX, lastX, firstZ, lastZ);
                window.HasError())
                return window;
            const double radiusSquared = static_cast<double>(profile.neighborRadiusMeters) * profile.neighborRadiusMeters;
            const auto [firstCell, lastCell] = AgentCellRange(storage, firstX, lastX);
            for (auto cell = firstCell; cell != lastCell; ++cell) {
                if (cell->z < firstZ || cell->z > lastZ)
                    continue;
                for (std::uint32_t offset = 0; offset < cell->agentCount; ++offset) {
                    const std::uint32_t otherIndex = storage.cellAgentIndices[cell->firstAgent + offset];
                    if (otherIndex == agentIndex)
                        continue;
                    if (++pairChecks > limits.maximumPairChecks)
                        return CapacityExceeded();
                    const auto &other = storage.agents[otherIndex];
                    if ((agent.avoidance.avoidsLayers & (std::uint64_t{1} << other.avoidance.layerBit)) == 0)
                        continue;
                    const double dx = static_cast<double>(other.position.x) - agent.position.x;
                    const double dz = static_cast<double>(other.position.z) - agent.position.z;
                    const double distanceSquared = dx * dx + dz * dz;
                    if (distanceSquared <= radiusSquared)
                        candidates.push_back({distanceSquared, otherIndex});
                }
            }
            std::ranges::sort(candidates, [&storage](const Candidate &left, const Candidate &right) {
                if (left.distanceSquared != right.distanceSquared)
                    return left.distanceSquared < right.distanceSquared;
                return storage.agents[left.index].handle < storage.agents[right.index].handle;
            });
            const std::size_t admitted = std::min(candidates.size(), static_cast<std::size_t>(profile.maximumNeighbors));
            if (admitted > limits.maximumNeighborFacts - storage.neighborIndices.size())
                return CapacityExceeded();
            agent.firstNeighbor = static_cast<std::uint32_t>(storage.neighborIndices.size());
            agent.neighborCount = static_cast<std::uint32_t>(admitted);
            agent.truncatedNeighbors = static_cast<std::uint32_t>(candidates.size() - admitted);
            for (std::size_t index = 0; index < admitted; ++index)
                storage.neighborIndices.push_back(candidates[index].index);
            return Result<void>::Success();
        }

        /** @brief Finds each relevant segment once, then orders equal distances by stable source identity. */
        [[nodiscard]] Result<void> CollectBoundaryCandidates(const NavigationCrowdAgentFact &agent,
                                                             const NavigationCrowdProfileFacts &profile,
                                                             const NavigationCrowdSnapshotLimits &limits,
                                                             const NavigationCrowdSnapshotStorage &storage, std::size_t &boundaryChecks,
                                                             std::vector<std::uint32_t> &scratch, std::vector<Candidate> &candidates) {
            scratch.clear();
            candidates.clear();
            std::int32_t firstX{};
            std::int32_t lastX{};
            std::int32_t firstZ{};
            std::int32_t lastZ{};
            if (const auto window = CellWindow(agent, profile.neighborRadiusMeters, limits, firstX, lastX, firstZ, lastZ);
                window.HasError())
                return window;
            const auto [firstEntry, lastEntry] = BoundaryCellRange(storage, firstX, lastX);
            for (auto entry = firstEntry; entry != lastEntry; ++entry) {
                if (++boundaryChecks > limits.maximumBoundaryChecks)
                    return CapacityExceeded();
                if (entry->z >= firstZ && entry->z <= lastZ)
                    scratch.push_back(entry->index);
            }
            std::ranges::sort(scratch);
            scratch.erase(std::ranges::unique(scratch).begin(), scratch.end());
            const double radiusSquared = static_cast<double>(profile.neighborRadiusMeters) * profile.neighborRadiusMeters;
            for (const std::uint32_t index : scratch) {
                const auto &segment = storage.segments[index];
                if (!segment.layers.Intersects(profile.obstacleLayers))
                    continue;
                if (static_cast<double>(agent.position.y) + agent.radiusMeters < segment.minimumY ||
                    static_cast<double>(agent.position.y) - agent.radiusMeters > segment.maximumY)
                    continue;
                const double distanceSquared = PointSegmentDistanceSquared(agent.position, segment);
                if (distanceSquared <= radiusSquared)
                    candidates.push_back({distanceSquared, index});
            }
            std::ranges::sort(candidates, [&storage](const Candidate &left, const Candidate &right) {
                if (left.distanceSquared != right.distanceSquared)
                    return left.distanceSquared < right.distanceSquared;
                const auto &a = storage.segments[left.index];
                const auto &b = storage.segments[right.index];
                return std::tuple{a.source.index(), a.stableId, a.edge} < std::tuple{b.source.index(), b.stableId, b.edge};
            });
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> GatherBoundaries(const std::uint32_t agentIndex, const NavigationCrowdProfileFacts &profile,
                                                    const NavigationCrowdSnapshotLimits &limits, NavigationCrowdSnapshotStorage &storage,
                                                    std::size_t &boundaryChecks, std::vector<std::uint32_t> &scratch,
                                                    std::vector<Candidate> &candidates) {
            auto &agent = storage.agents[agentIndex];
            if (const auto collected = CollectBoundaryCandidates(agent, profile, limits, storage, boundaryChecks, scratch, candidates);
                collected.HasError())
                return collected;
            const std::size_t admitted = std::min(candidates.size(), static_cast<std::size_t>(profile.maximumBoundarySegments));
            if (admitted > limits.maximumBoundaryFacts - storage.boundaryIndices.size())
                return CapacityExceeded();
            agent.firstBoundary = static_cast<std::uint32_t>(storage.boundaryIndices.size());
            agent.boundaryCount = static_cast<std::uint32_t>(admitted);
            agent.truncatedBoundaries = static_cast<std::uint32_t>(candidates.size() - admitted);
            for (std::size_t index = 0; index < admitted; ++index)
                storage.boundaryIndices.push_back(candidates[index].index);
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Selects exact bounded facts using stable distance and source/handle tie-breaks. */
    Result<void> GatherCrowdFacts(const NavigationCrowdSnapshotLimits &limits, NavigationCrowdSnapshotStorage &storage) {
        std::size_t pairChecks{};
        std::size_t boundaryChecks{};
        std::vector<Candidate> candidates;
        std::vector<std::uint32_t> boundaryScratch;
        for (std::uint32_t index = 0; index < storage.agents.size(); ++index) {
            const auto &profile = ProfileFor(storage, storage.agents[index].profile);
            if (const auto neighbors = GatherNeighbors(index, profile, limits, storage, pairChecks, candidates); neighbors.HasError())
                return neighbors;
            if (const auto boundaries = GatherBoundaries(index, profile, limits, storage, boundaryChecks, boundaryScratch, candidates);
                boundaries.HasError())
                return boundaries;
            const auto &agent = storage.agents[index];
            auto &truncation = TruncationFor(storage, agent.profile);
            if (agent.truncatedNeighbors != 0 || agent.truncatedBoundaries != 0)
                ++truncation.affectedAgents;
            truncation.neighborsOmitted += agent.truncatedNeighbors;
            truncation.boundariesOmitted += agent.truncatedBoundaries;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Navigation::Detail
