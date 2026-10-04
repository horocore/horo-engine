#include "Horo/Navigation/NavigationDefinition.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace Horo::Navigation {
    namespace {
        /** @brief Rejects capacity violations before registry/profile allocation. */
        bool WithinCapacity(const NavigationDefinitionInput &input) noexcept {
            return input.profiles.size() <= NavigationDefinition::MaximumProfiles &&
                   input.areas.size() <= NavigationDefinition::MaximumAreas &&
                   input.filters.size() <= NavigationDefinition::MaximumFilters &&
                   std::ranges::all_of(input.filters, [](const auto &filter) {
                return filter.costOverrides.size() <= NavigationDefinition::MaximumAreas;
            });
        }

        /** @brief Validates the qualified grid domain independently of source scope. */
        bool ValidTiles(const NavigationDefinitionTilePolicy &tiles) noexcept {
            return tiles.tileSizeCells > 0 && tiles.tileSizeCells <= 4096 && tiles.maximumTiles > 0 && tiles.maximumTiles <= 65536;
        }

        /** @brief Validates the closed policy without deriving defaults from absent source data. */
        bool ValidPolicy(const NavigationDefinitionInput &input) noexcept {
            return !input.profiles.empty() && !input.areas.empty() && !input.filters.empty() &&
                   input.scope < NavigationDefinitionScope::Count && input.coordinates.system < NavigationSourceCoordinateSystem::Count &&
                   std::isfinite(input.coordinates.metersPerUnit) && input.coordinates.metersPerUnit > 0.0F && ValidTiles(input.tiles);
        }

        /** @brief Validates profiles then rejects repeated stable identities independently of name/order. */
        Result<void> CanonicalizeProfiles(std::vector<NavigationAgentProfileDescriptor> &profiles) {
            for (const auto &profile : profiles) {
                if (profile.displayName.size() > NavigationDefinition::MaximumProfileNameBytes)
                    return Result<void>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
                if (!IsValidUtf8ScalarSequence(profile.displayName) || profile.displayName.find('\0') != std::string::npos)
                    return Result<void>::Failure(MakeError(NavigationErrors::AgentProfileInvalid));
                if (const auto valid = ValidateNavigationAgentProfile(profile); valid.HasError())
                    return valid;
            }
            std::ranges::sort(profiles, {}, &NavigationAgentProfileDescriptor::id);
            if (std::ranges::adjacent_find(profiles, {}, &NavigationAgentProfileDescriptor::id) != profiles.end())
                return Result<void>::Failure(MakeError(NavigationErrors::SourceDuplicateIdentity));
            return Result<void>::Success();
        }
    }  // namespace

    NavigationDefinition::NavigationDefinition(std::vector<NavigationAgentProfileDescriptor> profiles, NavigationAreaRegistry registry,
                                               const NavigationSourceCoordinateConvention coordinates,
                                               const NavigationDefinitionTilePolicy tiles, const NavigationDefinitionScope scope) noexcept
        : profiles_(std::move(profiles)), registry_(std::move(registry)), coordinates_(coordinates), tiles_(tiles), scope_(scope) {}

    /** @copydoc NavigationDefinition::Create */
    Result<NavigationDefinition> NavigationDefinition::Create(NavigationDefinitionInput input) {
        if (!WithinCapacity(input))
            return Result<NavigationDefinition>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
        if (!ValidPolicy(input))
            return Result<NavigationDefinition>::Failure(MakeError(NavigationErrors::SourceEnvelopeInvalid));
        try {
            if (const auto valid = CanonicalizeProfiles(input.profiles); valid.HasError())
                return Result<NavigationDefinition>::Failure(valid.ErrorValue());
            auto registry = NavigationAreaRegistry::Create(input.areas, input.filters);
            if (registry.HasError())
                return Result<NavigationDefinition>::Failure(registry.ErrorValue());
            return Result<NavigationDefinition>::Success(
                NavigationDefinition{std::move(input.profiles), std::move(registry).Value(), input.coordinates, input.tiles, input.scope});
        } catch (const std::bad_alloc &) {
            return Result<NavigationDefinition>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
        }
    }

    /** @copydoc NavigationDefinition::Profiles */
    std::span<const NavigationAgentProfileDescriptor> NavigationDefinition::Profiles() const noexcept {
        return profiles_;
    }

    /** @copydoc NavigationDefinition::Registry */
    const NavigationAreaRegistry &NavigationDefinition::Registry() const noexcept {
        return registry_;
    }

    /** @copydoc NavigationDefinition::Coordinates */
    NavigationSourceCoordinateConvention NavigationDefinition::Coordinates() const noexcept {
        return coordinates_;
    }

    /** @copydoc NavigationDefinition::Tiles */
    NavigationDefinitionTilePolicy NavigationDefinition::Tiles() const noexcept {
        return tiles_;
    }

    /** @copydoc NavigationDefinition::Scope */
    NavigationDefinitionScope NavigationDefinition::Scope() const noexcept {
        return scope_;
    }

    /** @copydoc NavigationDefinitionRecordSupport */
    NavigationAuthoredRecordSupport NavigationDefinitionRecordSupport() {
        return {.type = NavigationAuthoredRecordTypeId::Create(NavigationDefinitionRecordTypeValue).Value(),
                .minimum = NavigationDefinitionPayloadVersion,
                .maximum = NavigationDefinitionPayloadVersion};
    }
}  // namespace Horo::Navigation
