#include "Horo/Navigation/Backends/DeterministicCrowdProvider.h"

#include "NavigationCrowdGeometry.h"

#include <array>
#include <cfenv>
#include <cmath>
#include <new>
#include <string_view>

namespace Horo::Navigation {
    namespace {
        using Outcome = Result<NavigationAvoidanceOutcome>;

        /** @brief A finite positive magnitude used by the local movement envelope. */
        bool Positive(const float value) noexcept {
            return std::isfinite(value) && value > 0;
        }

        /** @brief An exact fixed-step, finite planar request. */
        bool ValidRequest(const NavigationAvoidanceRequest &request, const NavigationCrowdQualification &qualification) noexcept {
            return Math::IsFinite(request.preferredVelocity) && request.preferredVelocity.y == 0.0F &&
                   request.stepSeconds == qualification.fixedStepSeconds && Positive(request.maximumSpeedMetersPerSecond) &&
                   Positive(request.maximumAccelerationMetersPerSecondSquared) && Positive(request.horizonSeconds);
        }

        /** @brief Clamps the route preference before constructing the fixed candidate sequence. */
        Math::Vec3 ClampPreference(const Math::Vec3 value, const float limit) noexcept {
            const auto speed = std::hypot(static_cast<double>(value.x), static_cast<double>(value.z));
            if (speed <= limit)
                return value;
            const auto scale = static_cast<double>(limit) / speed;
            return {static_cast<float>(value.x * scale), 0, static_cast<float>(value.z * scale)};
        }

        /** @brief Reaches as much of an ordered target as this fixed step's acceleration can authorize. */
        Math::Vec3 ReachableVelocity(const Math::Vec3 current, const Math::Vec3 target, const double maximumChange) noexcept {
            const auto x = static_cast<double>(target.x) - current.x;
            const auto z = static_cast<double>(target.z) - current.z;
            const auto magnitude = std::hypot(x, z);
            if (magnitude <= maximumChange)
                return target;
            const auto scale = maximumChange / magnitude;
            return {static_cast<float>(current.x + x * scale), 0, static_cast<float>(current.z + z * scale)};
        }

        /** @brief Returns the first in-budget collision-free velocity from a fixed sequence. */
        std::optional<Math::Vec3> ChooseCandidate(const NavigationCrowdSnapshot &snapshot, const NavigationCrowdAgentFact &agent,
                                                  const NavigationAvoidanceRequest &request) noexcept {
            const auto preferred = ClampPreference(request.preferredVelocity, request.maximumSpeedMetersPerSecond);
            const Math::Vec3 left{-preferred.z, 0, preferred.x};
            const Math::Vec3 right{preferred.z, 0, -preferred.x};
            const std::array targets{preferred, preferred * 0.5F, left * 0.5F, right * 0.5F, Math::Vec3{}};
            const auto maximumChange = static_cast<double>(request.maximumAccelerationMetersPerSecondSquared) * request.stepSeconds;
            for (const auto target : targets) {
                const auto candidate = ReachableVelocity(agent.velocity, target, maximumChange);
                const auto speed = std::hypot(static_cast<double>(candidate.x), static_cast<double>(candidate.z));
                const auto change =
                    std::hypot(static_cast<double>(candidate.x) - agent.velocity.x, static_cast<double>(candidate.z) - agent.velocity.z);
                if (speed <= request.maximumSpeedMetersPerSecond && change <= maximumChange + 0.000001 &&
                    Detail::IsAvoidanceCandidateClear(snapshot, agent, candidate, request.horizonSeconds))
                    return candidate;
            }
            return std::nullopt;
        }

        /** @brief Stateless owner-executed solver. The immutable evidence stays with the provider. */
        class DeterministicCrowdProvider final : public INavigationQualifiedCrowdBackend {
        public:
            explicit DeterministicCrowdProvider(const NavigationCrowdQualification &qualification) noexcept
                : qualification_(qualification) {}

            /** @copydoc INavigationQualifiedCrowdBackend::Qualification */
            const NavigationCrowdQualification &Qualification() const noexcept override {
                return qualification_;
            }

            /** @copydoc INavigationCrowdBackend::Solve */
            Outcome Solve(const NavigationCrowdSnapshot &snapshot, const NavigationAvoidanceRequest &request) override {
                if (!snapshot.IsValid() || snapshot.Mode() != AvoidanceExecutionMode::DeterministicQualified)
                    return Outcome::Failure(MakeError(NavigationErrors::OperationUnsupported));
                if (std::fegetround() != FE_TONEAREST)
                    return Outcome::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
                if (request.agentIndex >= snapshot.Agents().size() || !ValidRequest(request, qualification_))
                    return Outcome::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
                const auto &agent = snapshot.Agents()[request.agentIndex];
                NavigationAvoidanceOutcome outcome{.binding = snapshot.Binding(),
                                                   .dynamicRevision = snapshot.DynamicRevision(),
                                                   .captureTick = snapshot.CaptureTick()};
                if (agent.truncatedNeighbors != 0 || agent.truncatedBoundaries != 0) {
                    outcome.stopReason = NavigationAvoidanceStopReason::SnapshotIncomplete;
                } else if (const auto chosen = ChooseCandidate(snapshot, agent, request)) {
                    outcome.desiredVelocity = *chosen;
                    outcome.disposition = NavigationAvoidanceDisposition::Sampled;
                    outcome.stopReason = NavigationAvoidanceStopReason::None;
                }
                return Outcome::Success(outcome);
            }

        private:
            const NavigationCrowdQualification qualification_;
        };
    }  // namespace

    /** @copydoc DeterministicCrowdProviderFingerprint */
    Sha256Digest DeterministicCrowdProviderFingerprint() noexcept {
        constexpr std::string_view contract = "horo.navigation.deterministic-crowd.fixed-candidates.v1";
        return ComputeSha256(std::as_bytes(std::span{contract.data(), contract.size()}));
    }

    /** @copydoc CreateDeterministicCrowdBackend */
    Result<std::unique_ptr<INavigationQualifiedCrowdBackend>> CreateDeterministicCrowdBackend(
        const NavigationCrowdQualification &qualification) {
        using Provider = Result<std::unique_ptr<INavigationQualifiedCrowdBackend>>;
        if (!qualification.IsValid() || qualification.provider != DeterministicCrowdProviderFingerprint())
            return Provider::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
        try {
            return Provider::Success(std::make_unique<DeterministicCrowdProvider>(qualification));
        } catch (const std::bad_alloc &) {
            return Provider::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Navigation
