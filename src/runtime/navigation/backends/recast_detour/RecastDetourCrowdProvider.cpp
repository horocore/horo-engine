#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"

#include "DetourObstacleAvoidance.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "RecastDetourCrowdGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <new>
#include <optional>

namespace Horo::Navigation {
    namespace {
        using enum NavigationAvoidanceStopReason;

        [[nodiscard]] bool Finite(const Math::Vec3 &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        [[nodiscard]] bool Valid(const NavigationAvoidanceRequest &request) noexcept {
            return Finite(request.preferredVelocity) && request.preferredVelocity.y == 0.0F &&
                   std::isfinite(request.maximumSpeedMetersPerSecond) && request.maximumSpeedMetersPerSecond > 0.0F &&
                   request.maximumSpeedMetersPerSecond <= 100.0F && std::isfinite(request.maximumAccelerationMetersPerSecondSquared) &&
                   request.maximumAccelerationMetersPerSecondSquared > 0.0F &&
                   request.maximumAccelerationMetersPerSecondSquared <= 1'000.0F && std::isfinite(request.stepSeconds) &&
                   request.stepSeconds > 0.0F && request.stepSeconds <= 1.0F && std::isfinite(request.horizonSeconds) &&
                   request.horizonSeconds >= request.stepSeconds && request.horizonSeconds <= 30.0F;
        }

        [[nodiscard]] NavigationAvoidanceOutcome Stop(const NavigationCrowdSnapshot &snapshot,
                                                      const NavigationAvoidanceStopReason reason) noexcept {
            return {.desiredVelocity = {},
                    .disposition = NavigationAvoidanceDisposition::CollisionSafeStop,
                    .stopReason = reason,
                    .binding = snapshot.Binding(),
                    .dynamicRevision = snapshot.DynamicRevision(),
                    .captureTick = snapshot.CaptureTick()};
        }

        [[nodiscard]] float SpeedSquared(const Math::Vec3 &velocity) noexcept {
            return velocity.x * velocity.x + velocity.z * velocity.z;
        }

        [[nodiscard]] bool NativeRange(const double value) noexcept {
            return std::isfinite(value) && std::abs(value) <= 10'000.0;
        }

        class RecastDetourCrowdBackend final : public INavigationCrowdBackend {
        public:
            [[nodiscard]] bool Initialize(const RecastDetourCrowdLimits &limits) noexcept {
                if (!query_.init(static_cast<int>(limits.maximumNeighbors), static_cast<int>(limits.maximumBoundarySegments)))
                    return false;
                limits_ = limits;
                return true;
            }

            /** @copydoc INavigationCrowdBackend::Solve */
            [[nodiscard]] Result<NavigationAvoidanceOutcome> Solve(const NavigationCrowdSnapshot &snapshot,
                                                                   const NavigationAvoidanceRequest &request) override {
                if (!snapshot.IsValid() || request.agentIndex >= snapshot.Agents().size() || !Valid(request))
                    return Result<NavigationAvoidanceOutcome>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
                const auto &agent = snapshot.Agents()[request.agentIndex];
                if (!Finite(agent.position) || !Finite(agent.velocity) || agent.radiusMeters <= 0.0F || !std::isfinite(agent.radiusMeters))
                    return Result<NavigationAvoidanceOutcome>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
                if (agent.radiusMeters > 1'000.0F || SpeedSquared(agent.velocity) > 10'000.0F)
                    return Result<NavigationAvoidanceOutcome>::Success(Stop(snapshot, NumericalFailure));
                if (snapshot.Mode() != AvoidanceExecutionMode::BestEffortBounded || agent.truncatedNeighbors != 0 ||
                    agent.truncatedBoundaries != 0 || agent.neighborCount > limits_.maximumNeighbors ||
                    agent.boundaryCount > limits_.maximumBoundarySegments)
                    return Result<NavigationAvoidanceOutcome>::Success(Stop(snapshot, SnapshotIncomplete));
                // The if-init lock must enclose the entire native query, not only the admission decision.
                if (std::unique_lock lock(mutex_, std::try_to_lock); lock.owns_lock())
                    return SolveAdmitted(snapshot, agent, request);
                return Result<NavigationAvoidanceOutcome>::Success(Stop(snapshot, CapacityBusy));
            }

        private:
            [[nodiscard]] Result<NavigationAvoidanceOutcome> SolveAdmitted(const NavigationCrowdSnapshot &snapshot,
                                                                           const NavigationCrowdAgentFact &agent,
                                                                           const NavigationAvoidanceRequest &request) {
                query_.reset();
                if (!LoadNeighbors(snapshot, agent) || !LoadBoundaries(snapshot, agent))
                    return Result<NavigationAvoidanceOutcome>::Success(Stop(snapshot, NumericalFailure));
                const auto sampled = Sample(agent, request);
                if (!sampled)
                    return Result<NavigationAvoidanceOutcome>::Success(Stop(snapshot, NumericalFailure));
                const auto candidate = Constrain(*sampled, agent.velocity, request);
                if (!candidate)
                    return Result<NavigationAvoidanceOutcome>::Success(Stop(snapshot, NumericalFailure));
                if (const float speedSquared = SpeedSquared(*candidate);
                    !std::isfinite(speedSquared) ||
                    speedSquared > request.maximumSpeedMetersPerSecond * request.maximumSpeedMetersPerSecond + 1.0e-4F ||
                    !Detail::IsAvoidanceCandidateClear(snapshot, agent, *candidate, request.horizonSeconds))
                    return Result<NavigationAvoidanceOutcome>::Success(Stop(snapshot, NoFeasibleSample));
                return Result<NavigationAvoidanceOutcome>::Success({.desiredVelocity = *candidate,
                                                                    .disposition = NavigationAvoidanceDisposition::Sampled,
                                                                    .stopReason = None,
                                                                    .binding = snapshot.Binding(),
                                                                    .dynamicRevision = snapshot.DynamicRevision(),
                                                                    .captureTick = snapshot.CaptureTick()});
            }

            [[nodiscard]] bool LoadNeighbors(const NavigationCrowdSnapshot &snapshot, const NavigationCrowdAgentFact &agent) noexcept {
                for (std::uint32_t offset = 0; offset < agent.neighborCount; ++offset) {
                    const auto &neighbor = snapshot.Agents()[snapshot.NeighborIndices()[agent.firstNeighbor + offset]];
                    if (!Finite(neighbor.position) || !Finite(neighbor.velocity) || !std::isfinite(neighbor.radiusMeters) ||
                        neighbor.radiusMeters <= 0.0F || neighbor.radiusMeters > 1'000.0F || SpeedSquared(neighbor.velocity) > 10'000.0F)
                        return false;
                    const double x = static_cast<double>(neighbor.position.x) - agent.position.x;
                    const double z = static_cast<double>(neighbor.position.z) - agent.position.z;
                    if (!NativeRange(x) || !NativeRange(z))
                        return false;
                    const std::array position{static_cast<float>(x), 0.0F, static_cast<float>(z)};
                    const std::array velocity{neighbor.velocity.x, 0.0F, neighbor.velocity.z};
                    query_.addCircle(position.data(), neighbor.radiusMeters, velocity.data(), velocity.data());
                }
                return true;
            }

            [[nodiscard]] bool LoadBoundaries(const NavigationCrowdSnapshot &snapshot, const NavigationCrowdAgentFact &agent) noexcept {
                for (std::uint32_t offset = 0; offset < agent.boundaryCount; ++offset) {
                    const auto &boundary = snapshot.BoundarySegments()[snapshot.BoundaryIndices()[agent.firstBoundary + offset]];
                    if (!Finite(boundary.first) || !Finite(boundary.second))
                        return false;
                    const double firstX = static_cast<double>(boundary.first.x) - agent.position.x;
                    const double firstZ = static_cast<double>(boundary.first.z) - agent.position.z;
                    const double secondX = static_cast<double>(boundary.second.x) - agent.position.x;
                    const double secondZ = static_cast<double>(boundary.second.z) - agent.position.z;
                    if (!NativeRange(firstX) || !NativeRange(firstZ) || !NativeRange(secondX) || !NativeRange(secondZ))
                        return false;
                    const std::array first{static_cast<float>(firstX), 0.0F, static_cast<float>(firstZ)};
                    const std::array second{static_cast<float>(secondX), 0.0F, static_cast<float>(secondZ)};
                    query_.addSegment(first.data(), second.data());
                }
                return true;
            }

            [[nodiscard]] std::optional<Math::Vec3> Sample(const NavigationCrowdAgentFact &agent,
                                                           const NavigationAvoidanceRequest &request) noexcept {
                const std::array position{0.0F, 0.0F, 0.0F};
                const std::array current{agent.velocity.x, 0.0F, agent.velocity.z};
                const double preferredLength =
                    std::hypot(static_cast<double>(request.preferredVelocity.x), static_cast<double>(request.preferredVelocity.z));
                const double preferredFactor =
                    preferredLength > request.maximumSpeedMetersPerSecond ? request.maximumSpeedMetersPerSecond / preferredLength : 1.0;
                const std::array preferred{static_cast<float>(request.preferredVelocity.x * preferredFactor), 0.0F,
                                           static_cast<float>(request.preferredVelocity.z * preferredFactor)};
                std::array<float, 3> sampled{};
                dtObstacleAvoidanceParams params{};
                params.velBias = 0.4F;
                // Right-of-way biases steering effort, never admission, collision validation, or movement authority.
                params.weightDesVel = 1.0F + 2.0F * agent.avoidance.priority;
                params.weightCurVel = 0.75F;
                params.weightSide = 0.75F;
                params.weightToi = 2.5F;
                params.horizTime = request.horizonSeconds;
                params.gridSize = 11;
                if (query_.sampleVelocityGrid(position.data(), agent.radiusMeters, request.maximumSpeedMetersPerSecond, current.data(),
                                              preferred.data(), sampled.data(), &params) <= 0 ||
                    !std::isfinite(sampled[0]) || !std::isfinite(sampled[2]))
                    return std::nullopt;
                return Math::Vec3{sampled[0], 0.0F, sampled[2]};
            }

            [[nodiscard]] static std::optional<Math::Vec3> Constrain(Math::Vec3 candidate, const Math::Vec3 current,
                                                                     const NavigationAvoidanceRequest &request) noexcept {
                const float maxDelta = request.maximumAccelerationMetersPerSecondSquared * request.stepSeconds;
                const Math::Vec3 delta{candidate.x - current.x, 0.0F, candidate.z - current.z};
                const float deltaSquared = SpeedSquared(delta);
                if (!std::isfinite(deltaSquared))
                    return std::nullopt;
                if (deltaSquared > maxDelta * maxDelta) {
                    const float factor = maxDelta / std::sqrt(deltaSquared);
                    candidate.x = current.x + delta.x * factor;
                    candidate.z = current.z + delta.z * factor;
                }
                if (!Finite(candidate))
                    return std::nullopt;
                return candidate;
            }

            // One preallocated native sampler. Concurrent callers never block and receive an explicit stop.
            std::mutex mutex_;
            dtObstacleAvoidanceQuery query_;
            RecastDetourCrowdLimits limits_;
        };
    }  // namespace

    /** @copydoc CreateRecastDetourCrowdBackend */
    Result<std::unique_ptr<INavigationCrowdBackend>> CreateRecastDetourCrowdBackend(const RecastDetourCrowdLimits &limits) {
        if (limits.maximumNeighbors == 0 || limits.maximumNeighbors > 256 || limits.maximumBoundarySegments == 0 ||
            limits.maximumBoundarySegments > 256)
            return Result<std::unique_ptr<INavigationCrowdBackend>>::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
        auto backend = std::unique_ptr<RecastDetourCrowdBackend>(new (std::nothrow) RecastDetourCrowdBackend());
        if (!backend)
            return Result<std::unique_ptr<INavigationCrowdBackend>>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        if (!backend->Initialize(limits))
            return Result<std::unique_ptr<INavigationCrowdBackend>>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        return Result<std::unique_ptr<INavigationCrowdBackend>>::Success(std::move(backend));
    }
}  // namespace Horo::Navigation
