#include "Horo/Navigation/Backends/NullProvider.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Navigation {
    namespace {
        [[nodiscard]] NavigationProviderCapabilities NullCapabilities() noexcept {
            constexpr auto maximumCount = std::numeric_limits<std::uint32_t>::max();
            constexpr NavigationQueryLimits limits{maximumCount, maximumCount, std::numeric_limits<float>::max()};
            return MakeAvailableGroundedQueryCapabilities(1, limits, 1);
        }

        template <typename Request, typename Validator>
        [[nodiscard]] Result<void> ValidateNullRequest(const Request &request, const NavigationQueryKind expected, Validator &&validator) {
            if (!request.world.IsValid())
                return Result<void>::Failure(MakeError(NavigationErrors::InvalidWorld));
            if (!request.topology.IsValid())
                return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
            if (!validator(request))
                return Result<void>::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
            if (request.requirement.query != expected)
                return Result<void>::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
            return AdmitNavigationQuery(NullCapabilities(), 1, request.requirement);
        }

        [[nodiscard]] bool IsWithinDistance(const Math::Vec3 first, const Math::Vec3 second, const float maximum) noexcept {
            const double distance = std::hypot(static_cast<double>(second.x) - first.x, static_cast<double>(second.y) - first.y,
                                               static_cast<double>(second.z) - first.z);
            return std::isfinite(distance) && distance <= maximum;
        }

        class NullNavigationQueryBackend final : public INavigationQueryBackend {
        public:
            /** @copydoc INavigationQueryBackend::Capabilities */
            [[nodiscard]] NavigationProviderCapabilities Capabilities() const noexcept override {
                static const NavigationProviderCapabilities capabilities = NullCapabilities();
                return capabilities;
            }

            /** @copydoc INavigationQueryBackend::FindPath */
            [[nodiscard]] Result<NavigationPath> FindPath(const NavigationPathRequest &request,
                                                          const CancellationToken &cancellation) const override {
                if (const auto validated = ValidateNullRequest(request, NavigationQueryKind::Path,
                                                               [](const auto &value) {
                    return Math::IsFinite(value.start) && Math::IsFinite(value.destination) && value.filter.IsValid() &&
                           value.coveragePolicy < NavigationPathCoveragePolicy::Count && value.requirement.limits.maximumResultPoints >= 2U;
                });
                    validated.HasError())
                    return Result<NavigationPath>::Failure(validated.ErrorValue());
                if (!IsWithinDistance(request.start, request.destination, request.requirement.limits.maximumSearchDistanceMeters))
                    return Result<NavigationPath>::Failure(MakeError(NavigationErrors::QueryLimitExceeded));
                if (cancellation.IsCancellationRequested())
                    return Result<NavigationPath>::Failure(MakeError(NavigationErrors::QueryCancelled));
                return Result<NavigationPath>::Failure(MakeError(NavigationErrors::NoNavigationData));
            }

            /** @copydoc INavigationQueryBackend::ProjectPoint */
            [[nodiscard]] Result<NavigationProjectionResult> ProjectPoint(const NavigationPointProjectionRequest &request,
                                                                          const CancellationToken &cancellation) const override {
                if (const auto validated = ValidateNullRequest(request, NavigationQueryKind::NearestPoint,
                                                               [](const auto &value) {
                    const bool zeroExtents = value.halfExtents.x == 0.0F && value.halfExtents.y == 0.0F && value.halfExtents.z == 0.0F;
                    return Math::IsFinite(value.point) && Math::IsFinite(value.halfExtents) &&
                           (zeroExtents || (value.halfExtents.x > 0.0F && value.halfExtents.y > 0.0F && value.halfExtents.z > 0.0F));
                });
                    validated.HasError())
                    return Result<NavigationProjectionResult>::Failure(validated.ErrorValue());
                if (std::max({request.halfExtents.x, request.halfExtents.y, request.halfExtents.z}) >
                    request.requirement.limits.maximumSearchDistanceMeters)
                    return Result<NavigationProjectionResult>::Failure(MakeError(NavigationErrors::QueryLimitExceeded));
                if (cancellation.IsCancellationRequested())
                    return Result<NavigationProjectionResult>::Failure(MakeError(NavigationErrors::QueryCancelled));
                return Result<NavigationProjectionResult>::Failure(MakeError(NavigationErrors::NoNavigationData));
            }

            /** @copydoc INavigationQueryBackend::SamplePosition */
            [[nodiscard]] Result<NavigationSamplePositionResult> SamplePosition(const NavigationSamplePositionRequest &request,
                                                                                const CancellationToken &cancellation) const override {
                if (const auto validated = ValidateNullRequest(request, NavigationQueryKind::SamplePosition,
                                                               [](const auto &value) {
                    return Math::IsFinite(value.center) && std::isfinite(value.radiusMeters) && value.radiusMeters > 0.0F;
                });
                    validated.HasError())
                    return Result<NavigationSamplePositionResult>::Failure(validated.ErrorValue());
                if (request.radiusMeters > request.requirement.limits.maximumSearchDistanceMeters)
                    return Result<NavigationSamplePositionResult>::Failure(MakeError(NavigationErrors::QueryLimitExceeded));
                if (cancellation.IsCancellationRequested())
                    return Result<NavigationSamplePositionResult>::Failure(MakeError(NavigationErrors::QueryCancelled));
                return Result<NavigationSamplePositionResult>::Failure(MakeError(NavigationErrors::NoNavigationData));
            }

            /** @copydoc INavigationQueryBackend::Raycast */
            [[nodiscard]] Result<NavigationRaycastResult> Raycast(const NavigationRaycastRequest &request,
                                                                  const CancellationToken &cancellation) const override {
                if (const auto validated = ValidateNullRequest(request, NavigationQueryKind::Raycast,
                                                               [](const auto &value) {
                    return Math::IsFinite(value.start) && Math::IsFinite(value.destination);
                });
                    validated.HasError())
                    return Result<NavigationRaycastResult>::Failure(validated.ErrorValue());
                if (!IsWithinDistance(request.start, request.destination, request.requirement.limits.maximumSearchDistanceMeters))
                    return Result<NavigationRaycastResult>::Failure(MakeError(NavigationErrors::QueryLimitExceeded));
                if (cancellation.IsCancellationRequested())
                    return Result<NavigationRaycastResult>::Failure(MakeError(NavigationErrors::QueryCancelled));
                return Result<NavigationRaycastResult>::Failure(MakeError(NavigationErrors::NoNavigationData));
            }

            /** @copydoc INavigationQueryBackend::QueryPolygons */
            [[nodiscard]] Result<NavigationPolygonQueryResult> QueryPolygons(const NavigationPolygonQueryRequest &request,
                                                                             const CancellationToken &cancellation) const override {
                if (const auto validated = ValidateNullRequest(request, NavigationQueryKind::PolygonQuery,
                                                               [](const auto &value) {
                    return Math::IsFinite(value.center) && Math::IsFinite(value.halfExtents) && value.halfExtents.x > 0.0F &&
                           value.halfExtents.y > 0.0F && value.halfExtents.z > 0.0F;
                });
                    validated.HasError())
                    return Result<NavigationPolygonQueryResult>::Failure(validated.ErrorValue());
                if (std::max({request.halfExtents.x, request.halfExtents.y, request.halfExtents.z}) >
                    request.requirement.limits.maximumSearchDistanceMeters)
                    return Result<NavigationPolygonQueryResult>::Failure(MakeError(NavigationErrors::QueryLimitExceeded));
                if (cancellation.IsCancellationRequested())
                    return Result<NavigationPolygonQueryResult>::Failure(MakeError(NavigationErrors::QueryCancelled));
                return Result<NavigationPolygonQueryResult>::Failure(MakeError(NavigationErrors::NoNavigationData));
            }
        };
    }  // namespace

    /** @copydoc CreateNullNavigationQueryBackend */
    Result<std::unique_ptr<INavigationQueryBackend>> CreateNullNavigationQueryBackend() {
        auto provider = std::unique_ptr<INavigationQueryBackend>{new (std::nothrow) NullNavigationQueryBackend{}};
        if (!provider)
            return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        return Result<std::unique_ptr<INavigationQueryBackend>>::Success(std::move(provider));
    }
}  // namespace Horo::Navigation
