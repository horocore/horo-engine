#include "Horo/Navigation/NavigationProjectProfiles.h"

#include "Horo/Foundation/StableHash.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <ranges>

namespace Horo::Navigation {
    namespace {
        template <typename T> bool CheckedMultiply(const T left, const T right, T &product) noexcept {
            if (left != 0 && right > std::numeric_limits<T>::max() / left)
                return false;
            product = left * right;
            return true;
        }

        bool IsPositive(const NavigationCapacityLimits &limits) noexcept {
            return limits.maximumAgents > 0 && limits.maximumSurfaces > 0 && limits.maximumResidentTiles > 0 &&
                   limits.maximumConcurrentQueries > 0 && limits.maximumBytesPerResidentTile > 0 && limits.maximumResidentMemoryBytes > 0 &&
                   limits.maximumWorkUnitsPerTick > 0;
        }

        bool IsKnownRequirement(const NavigationCapabilityRequirement requirement) noexcept {
            return requirement >= NavigationCapabilityRequirement::Optional && requirement < NavigationCapabilityRequirement::Count;
        }

        bool IsKnownQueryRequirement(const NavigationQueryRequirement &requirement) noexcept {
            return requirement.query < NavigationQueryKind::Count && requirement.quality < NavigationQualityLevel::Count &&
                   requirement.limits.maximumNodeExpansions > 0 && requirement.limits.maximumResultPoints > 0 &&
                   requirement.limits.maximumSearchDistanceMeters > 0.0F &&
                   requirement.limits.maximumSearchDistanceMeters <= std::numeric_limits<float>::max();
        }

        bool AggregateEnvelopeFits(const NavigationCapacityLimits &capacities, const NavigationQueryRequirement &maximumQuery) noexcept {
            if (std::uint64_t residentBytes{}; !CheckedMultiply(static_cast<std::uint64_t>(capacities.maximumResidentTiles),
                                                                capacities.maximumBytesPerResidentTile, residentBytes) ||
                                               residentBytes > capacities.maximumResidentMemoryBytes)
                return false;

            std::uint64_t queryWork{};
            return CheckedMultiply(static_cast<std::uint64_t>(capacities.maximumConcurrentQueries),
                                   static_cast<std::uint64_t>(maximumQuery.limits.maximumNodeExpansions), queryWork) &&
                   queryWork <= capacities.maximumWorkUnitsPerTick;
        }

        NavigationProjectProfileFingerprint ComputeFingerprint(const NavigationProjectProfileInput &input) {
            Foundation::StableHash64 hash;
            hash.AddInteger(input.id.Value());
            hash.AddInteger(input.revision.Value());
            hash.AddInteger(input.capacities.maximumAgents);
            hash.AddInteger(input.capacities.maximumSurfaces);
            hash.AddInteger(input.capacities.maximumResidentTiles);
            hash.AddInteger(input.capacities.maximumConcurrentQueries);
            hash.AddInteger(input.capacities.maximumBytesPerResidentTile);
            hash.AddInteger(input.capacities.maximumResidentMemoryBytes);
            hash.AddInteger(input.capacities.maximumWorkUnitsPerTick);
            hash.AddInteger(static_cast<std::uint8_t>(input.maximumQuery.query));
            hash.AddInteger(static_cast<std::uint8_t>(input.maximumQuery.quality));
            hash.AddInteger(input.maximumQuery.limits.maximumNodeExpansions);
            hash.AddInteger(input.maximumQuery.limits.maximumResultPoints);
            hash.AddInteger(std::bit_cast<std::uint32_t>(input.maximumQuery.limits.maximumSearchDistanceMeters));
            for (const auto requirement : input.capabilities)
                hash.AddInteger(static_cast<std::uint8_t>(requirement));
            const auto value = hash.Value() == 0 ? 1 : hash.Value();
            return NavigationProjectProfileFingerprint::Create(value).Value();
        }

        NavigationCapacityLimits Clamp(const NavigationCapacityLimits &requested, const NavigationCapacityLimits &authority) noexcept {
            return {
                .maximumAgents = std::min(requested.maximumAgents, authority.maximumAgents),
                .maximumSurfaces = std::min(requested.maximumSurfaces, authority.maximumSurfaces),
                .maximumResidentTiles = std::min(requested.maximumResidentTiles, authority.maximumResidentTiles),
                .maximumConcurrentQueries = std::min(requested.maximumConcurrentQueries, authority.maximumConcurrentQueries),
                .maximumBytesPerResidentTile = std::min(requested.maximumBytesPerResidentTile, authority.maximumBytesPerResidentTile),
                .maximumResidentMemoryBytes = std::min(requested.maximumResidentMemoryBytes, authority.maximumResidentMemoryBytes),
                .maximumWorkUnitsPerTick = std::min(requested.maximumWorkUnitsPerTick, authority.maximumWorkUnitsPerTick),
            };
        }
    }  // namespace

    /** @copydoc NavigationProjectProfile::Create */
    Result<NavigationProjectProfile> NavigationProjectProfile::Create(const NavigationProjectProfileInput &input) {
        if (!input.id.IsValid() || !input.revision.IsValid() || !IsPositive(input.capacities) ||
            !IsKnownQueryRequirement(input.maximumQuery) || !std::ranges::all_of(input.capabilities, IsKnownRequirement))
            return Result<NavigationProjectProfile>::Failure(MakeError(NavigationErrors::ProjectProfileInvalid));
        if (!AggregateEnvelopeFits(input.capacities, input.maximumQuery))
            return Result<NavigationProjectProfile>::Failure(MakeError(NavigationErrors::ProjectProfileCapacityExceeded));
        return Result<NavigationProjectProfile>::Success(NavigationProjectProfile{input, ComputeFingerprint(input)});
    }

    /** @copydoc NavigationProjectProfile::Replace */
    Result<NavigationProjectProfile> NavigationProjectProfile::Replace(const NavigationProjectProfile &previous,
                                                                       const NavigationProjectProfileInput &input) {
        if (input.id != previous.Id() || !input.revision.IsValid() || input.revision.Value() <= previous.Revision().Value())
            return Result<NavigationProjectProfile>::Failure(MakeError(NavigationErrors::ProjectProfileStale));
        return Create(input);
    }

    /** @copydoc NavigationProjectProfile::NavigationProjectProfile */
    NavigationProjectProfile::NavigationProjectProfile(const NavigationProjectProfileInput &input,
                                                       const NavigationProjectProfileFingerprint fingerprint) noexcept
        : input_(input), fingerprint_(fingerprint) {}

    /** @copydoc NavigationProjectProfile::Id */
    NavigationProjectProfileId NavigationProjectProfile::Id() const noexcept {
        return input_.id;
    }

    /** @copydoc NavigationProjectProfile::Revision */
    NavigationProjectProfileRevision NavigationProjectProfile::Revision() const noexcept {
        return input_.revision;
    }

    /** @copydoc NavigationProjectProfile::Fingerprint */
    NavigationProjectProfileFingerprint NavigationProjectProfile::Fingerprint() const noexcept {
        return fingerprint_;
    }

    /** @copydoc NavigationProjectProfile::Capacities */
    const NavigationCapacityLimits &NavigationProjectProfile::Capacities() const noexcept {
        return input_.capacities;
    }

    /** @copydoc NavigationProjectProfile::MaximumQuery */
    const NavigationQueryRequirement &NavigationProjectProfile::MaximumQuery() const noexcept {
        return input_.maximumQuery;
    }

    /** @copydoc NavigationProjectProfile::Requirement */
    NavigationCapabilityRequirement NavigationProjectProfile::Requirement(const NavigationCapability capability) const noexcept {
        const auto index = static_cast<std::size_t>(capability);
        if (index >= input_.capabilities.size())
            return NavigationCapabilityRequirement::Count;
        return input_.capabilities[index];
    }

    /** @copydoc ResolvedNavigationProjectProfile::ResolvedNavigationProjectProfile */
    ResolvedNavigationProjectProfile::ResolvedNavigationProjectProfile(const NavigationProjectProfile &project,
                                                                       std::optional<NavigationPreviewPreferenceRevision> previewRevision,
                                                                       const NavigationCapacityLimits &capacities) noexcept
        : id_(project.Id()), projectRevision_(project.Revision()), projectFingerprint_(project.Fingerprint()),
          previewRevision_(previewRevision), capacities_(capacities), maximumQuery_(project.MaximumQuery()) {}

    /** @copydoc ResolvedNavigationProjectProfile::Id */
    NavigationProjectProfileId ResolvedNavigationProjectProfile::Id() const noexcept {
        return id_;
    }

    /** @copydoc ResolvedNavigationProjectProfile::ProjectRevision */
    NavigationProjectProfileRevision ResolvedNavigationProjectProfile::ProjectRevision() const noexcept {
        return projectRevision_;
    }

    /** @copydoc ResolvedNavigationProjectProfile::ProjectFingerprint */
    NavigationProjectProfileFingerprint ResolvedNavigationProjectProfile::ProjectFingerprint() const noexcept {
        return projectFingerprint_;
    }

    /** @copydoc ResolvedNavigationProjectProfile::PreviewRevision */
    std::optional<NavigationPreviewPreferenceRevision> ResolvedNavigationProjectProfile::PreviewRevision() const noexcept {
        return previewRevision_;
    }

    /** @copydoc ResolvedNavigationProjectProfile::Capacities */
    const NavigationCapacityLimits &ResolvedNavigationProjectProfile::Capacities() const noexcept {
        return capacities_;
    }

    /** @copydoc ResolvedNavigationProjectProfile::MaximumQuery */
    const NavigationQueryRequirement &ResolvedNavigationProjectProfile::MaximumQuery() const noexcept {
        return maximumQuery_;
    }

    /** @copydoc ResolveNavigationProjectProfile */
    Result<ResolvedNavigationProjectProfile> ResolveNavigationProjectProfile(
        const NavigationProjectProfile &project, const std::optional<NavigationDeveloperPreviewPreference> &preview) {
        if (!preview.has_value())
            return Result<ResolvedNavigationProjectProfile>::Success(
                ResolvedNavigationProjectProfile{project, std::nullopt, project.Capacities()});
        if (!preview->revision.IsValid() || !IsPositive(preview->requestedMaximums))
            return Result<ResolvedNavigationProjectProfile>::Failure(MakeError(NavigationErrors::ProjectProfileInvalid));
        if (preview->projectRevision != project.Revision())
            return Result<ResolvedNavigationProjectProfile>::Failure(MakeError(NavigationErrors::ProjectProfileStale));
        const auto capacities = Clamp(preview->requestedMaximums, project.Capacities());
        if (!AggregateEnvelopeFits(capacities, project.MaximumQuery()))
            return Result<ResolvedNavigationProjectProfile>::Failure(MakeError(NavigationErrors::ProjectProfileCapacityExceeded));
        return Result<ResolvedNavigationProjectProfile>::Success(ResolvedNavigationProjectProfile{project, preview->revision, capacities});
    }

    /** @copydoc AdmitNavigationCapacity */
    Result<void> AdmitNavigationCapacity(const ResolvedNavigationProjectProfile &profile, const NavigationCapacityUsage &usage) {
        if (const auto &limits = profile.Capacities();
            usage.agents > limits.maximumAgents || usage.surfaces > limits.maximumSurfaces ||
            usage.residentTiles > limits.maximumResidentTiles || usage.concurrentQueries > limits.maximumConcurrentQueries ||
            usage.bytesPerResidentTile > limits.maximumBytesPerResidentTile ||
            usage.residentMemoryBytes > limits.maximumResidentMemoryBytes || usage.workUnitsThisTick > limits.maximumWorkUnitsPerTick)
            return Result<void>::Failure(MakeError(NavigationErrors::ProjectProfileCapacityExceeded));
        return Result<void>::Success();
    }

    /** @copydoc AdmitNavigationProjectProfile */
    Result<void> AdmitNavigationProjectProfile(const NavigationProjectProfile &profile, const NavigationProviderCapabilities &provider,
                                               const std::uint64_t expectedProviderRevision) {
        if (!ValidateNavigationProviderCapabilities(provider))
            return Result<void>::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
        if (expectedProviderRevision == 0 || provider.revision != expectedProviderRevision)
            return Result<void>::Failure(MakeError(NavigationErrors::CapabilityStale));

        for (std::size_t index = 0; index < static_cast<std::size_t>(NavigationCapability::Count); ++index) {
            if (profile.Requirement(static_cast<NavigationCapability>(index)) != NavigationCapabilityRequirement::Required)
                continue;
            const auto support = QueryNavigationCapability(provider, static_cast<NavigationCapability>(index));
            if (support == NavigationSupport::Unsupported)
                return Result<void>::Failure(MakeError(NavigationErrors::OperationUnsupported));
            if (support != NavigationSupport::Available)
                return Result<void>::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
        }

        if (profile.Capacities().maximumConcurrentQueries > provider.maximumConcurrentQueries)
            return Result<void>::Failure(MakeError(NavigationErrors::ProjectProfileCapacityExceeded));
        return AdmitNavigationQuery(provider, expectedProviderRevision, profile.MaximumQuery());
    }
}  // namespace Horo::Navigation
