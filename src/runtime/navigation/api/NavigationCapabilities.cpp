#include "Horo/Navigation/NavigationCapabilities.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <cmath>

namespace Horo::Navigation {
    namespace {
        /** @brief Checks one closed enum without accepting its Count sentinel. */
        template <typename Enum> bool IsKnown(const Enum value, const Enum count) noexcept {
            return value < count;
        }

        /** @brief Checks support evidence against the composition-level availability fact. */
        bool IsCoherentSupport(const NavigationSupport support, const NavigationProviderAvailability availability) noexcept {
            if (!IsKnown(support, NavigationSupport::Count))
                return false;
            if (availability == NavigationProviderAvailability::Omitted)
                return support == NavigationSupport::Unsupported;
            return availability == NavigationProviderAvailability::Available || support != NavigationSupport::Available;
        }

        /** @brief Reports whether a query limit declaration is exactly the unused zero representation. */
        bool IsZero(const NavigationQueryLimits &limits) noexcept {
            return limits.maximumNodeExpansions == 0 && limits.maximumResultPoints == 0 && limits.maximumSearchDistanceMeters == 0.0F;
        }

        /** @brief Validates positive integral bounds and a positive finite distance without inventing product policy. */
        bool ValidateLimits(const NavigationQueryLimits &limits) noexcept {
            return limits.maximumNodeExpansions > 0 && limits.maximumResultPoints > 0 &&
                   std::isfinite(limits.maximumSearchDistanceMeters) && limits.maximumSearchDistanceMeters > 0.0F;
        }

        /** @brief Combines exact query evidence without hiding available or temporarily unavailable support. */
        NavigationSupport CombineQuerySupport(const NavigationSupport aggregate, const NavigationSupport support) noexcept {
            using enum NavigationSupport;
            if (aggregate == Available || support == Available)
                return Available;
            if (aggregate == Unavailable || support == Unavailable)
                return Unavailable;
            if (aggregate == Unknown || support == Unknown)
                return Unknown;
            return Unsupported;
        }

        /** @brief Validates all query-quality support and limit pairs and derives their single aggregate support state. */
        bool ValidateQueries(const NavigationProviderCapabilities &capabilities, NavigationSupport &aggregate) noexcept {
            aggregate = NavigationSupport::Unsupported;
            for (std::size_t query = 0; query < capabilities.querySupport.size(); ++query) {
                for (std::size_t quality = 0; quality < capabilities.querySupport[query].size(); ++quality) {
                    const auto support = capabilities.querySupport[query][quality];
                    if (!IsCoherentSupport(support, capabilities.availability))
                        return false;
                    if (const bool available = support == NavigationSupport::Available;
                        available ? !ValidateLimits(capabilities.queryLimits[query][quality])
                                  : !IsZero(capabilities.queryLimits[query][quality]))
                        return false;
                    aggregate = CombineQuerySupport(aggregate, support);
                }
            }
            return true;
        }

        /** @brief Checks whether the requested limits fit the exact supported query-quality declaration. */
        bool Fits(const NavigationQueryLimits &requested, const NavigationQueryLimits &available) noexcept {
            return requested.maximumNodeExpansions <= available.maximumNodeExpansions &&
                   requested.maximumResultPoints <= available.maximumResultPoints &&
                   requested.maximumSearchDistanceMeters <= available.maximumSearchDistanceMeters;
        }

        /** @brief Validates only descriptor-shaped admission inputs before revision or support checks. */
        Result<void> ValidateAdmissionDescriptors(const NavigationProviderCapabilities &capabilities, const std::uint64_t expectedRevision,
                                                  const NavigationQueryRequirement &requirement) {
            if (!ValidateNavigationProviderCapabilities(capabilities) || expectedRevision == 0 ||
                !IsKnown(requirement.query, NavigationQueryKind::Count) || !IsKnown(requirement.quality, NavigationQualityLevel::Count) ||
                !ValidateLimits(requirement.limits))
                return Result<void>::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
            return Result<void>::Success();
        }

        /** @brief Maps exact support evidence to pre-dispatch admission without queue mutation. */
        Result<void> AdmitSupport(const NavigationSupport support) {
            if (support == NavigationSupport::Unsupported)
                return Result<void>::Failure(MakeError(NavigationErrors::OperationUnsupported));
            if (support != NavigationSupport::Available)
                return Result<void>::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MakeAvailablePathQueryCapabilities */
    NavigationProviderCapabilities MakeAvailablePathQueryCapabilities(const std::uint64_t revision, const NavigationQueryLimits &limits,
                                                                      const std::uint32_t maximumConcurrentQueries) noexcept {
        NavigationProviderCapabilities capabilities;
        capabilities.revision = revision;
        capabilities.availability = NavigationProviderAvailability::Available;
        capabilities.capabilities.fill(NavigationSupport::Unsupported);
        capabilities.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)] = NavigationSupport::Available;
        for (auto &query : capabilities.querySupport)
            query.fill(NavigationSupport::Unsupported);
        capabilities.querySupport[static_cast<std::size_t>(NavigationQueryKind::Path)].fill(NavigationSupport::Available);
        capabilities.queryLimits[static_cast<std::size_t>(NavigationQueryKind::Path)].fill(limits);
        capabilities.maximumConcurrentQueries = maximumConcurrentQueries;
        return capabilities;
    }

    /** @copydoc MakeAvailableGroundedQueryCapabilities */
    NavigationProviderCapabilities MakeAvailableGroundedQueryCapabilities(const std::uint64_t revision, const NavigationQueryLimits &limits,
                                                                          const std::uint32_t maximumConcurrentQueries) noexcept {
        auto capabilities = MakeAvailablePathQueryCapabilities(revision, limits, maximumConcurrentQueries);
        for (std::size_t query = 0; query < capabilities.querySupport.size(); ++query) {
            capabilities.querySupport[query].fill(NavigationSupport::Available);
            capabilities.queryLimits[query].fill(limits);
        }
        return capabilities;
    }

    /** @copydoc ValidateNavigationProviderCapabilities */
    bool ValidateNavigationProviderCapabilities(const NavigationProviderCapabilities &capabilities) noexcept {
        if (capabilities.contractVersion != 1 || capabilities.revision == 0 ||
            !IsKnown(capabilities.availability, NavigationProviderAvailability::Count))
            return false;
        if (!std::ranges::all_of(capabilities.capabilities, [&capabilities](const auto support) {
            return IsCoherentSupport(support, capabilities.availability);
        }))
            return false;

        NavigationSupport aggregateQuerySupport{};
        if (!ValidateQueries(capabilities, aggregateQuerySupport))
            return false;
        if (const auto grounded = capabilities.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)];
            aggregateQuerySupport != grounded)
            return false;
        if (aggregateQuerySupport == NavigationSupport::Available)
            return capabilities.maximumConcurrentQueries > 0;
        return capabilities.maximumConcurrentQueries == 0;
    }

    /** @copydoc QueryNavigationCapability */
    NavigationSupport QueryNavigationCapability(const NavigationProviderCapabilities &capabilities,
                                                const NavigationCapability capability) noexcept {
        if (!ValidateNavigationProviderCapabilities(capabilities) || !IsKnown(capability, NavigationCapability::Count))
            return NavigationSupport::Unknown;
        return capabilities.capabilities[static_cast<std::size_t>(capability)];
    }

    /** @copydoc QueryNavigationSupport */
    NavigationSupport QueryNavigationSupport(const NavigationProviderCapabilities &capabilities, const NavigationQueryKind query,
                                             const NavigationQualityLevel quality) noexcept {
        if (!ValidateNavigationProviderCapabilities(capabilities) || !IsKnown(query, NavigationQueryKind::Count) ||
            !IsKnown(quality, NavigationQualityLevel::Count))
            return NavigationSupport::Unknown;
        return capabilities.querySupport[static_cast<std::size_t>(query)][static_cast<std::size_t>(quality)];
    }

    /** @copydoc AdmitNavigationQuery */
    Result<void> AdmitNavigationQuery(const NavigationProviderCapabilities &capabilities, const std::uint64_t expectedRevision,
                                      const NavigationQueryRequirement &requirement) {
        if (const auto descriptors = ValidateAdmissionDescriptors(capabilities, expectedRevision, requirement); descriptors.HasError())
            return descriptors;
        if (capabilities.revision != expectedRevision)
            return Result<void>::Failure(MakeError(NavigationErrors::CapabilityStale));

        const auto queryIndex = static_cast<std::size_t>(requirement.query);
        const auto qualityIndex = static_cast<std::size_t>(requirement.quality);
        if (const auto support = AdmitSupport(capabilities.querySupport[queryIndex][qualityIndex]); support.HasError())
            return support;
        if (!Fits(requirement.limits, capabilities.queryLimits[queryIndex][qualityIndex]))
            return Result<void>::Failure(MakeError(NavigationErrors::QueryLimitExceeded));
        return Result<void>::Success();
    }
}  // namespace Horo::Navigation
