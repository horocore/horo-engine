#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    /** @copydoc CanonicalProfileQueryResponse */
    std::optional<PhysicsQueryResponse> CanonicalProfileQueryResponse(const CanonicalQueryAccess &access, const CollisionProfileId profile,
                                                                      const PhysicsQueryChannelId channel, const bool sensor) noexcept {
        const auto response = access.simulation.QueryResponse(profile, channel);
        if (!response || *response == CollisionQueryResponse::Ignore)
            return std::nullopt;
        return *response == CollisionQueryResponse::Block && !sensor ? PhysicsQueryResponse::Block : PhysicsQueryResponse::Overlap;
    }

    /** @copydoc CanonicalQueryFilterGeneration */
    std::uint64_t CanonicalQueryFilterGeneration(const CanonicalQueryAccess &access) noexcept {
        return access.simulation.binding.schema ? access.simulation.SchemaGeneration() : access.querySchemaGeneration;
    }

    /** @copydoc ValidateCanonicalQueryAuthority */
    Result<void> ValidateCanonicalQueryAuthority(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor) {
        if (!access.simulation.binding.schema)
            return Result<void>::Success();
        if (!access.simulation.Channel(descriptor.filter.channel) ||
            (descriptor.filter.requiredProfile && !access.simulation.Profile(*descriptor.filter.requiredProfile)))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (descriptor.filter.requiredLayer) {
            const auto layers = access.simulation.binding.schema->Layers();
            const auto layer = std::ranges::find(layers, *descriptor.filter.requiredLayer, &CollisionLayerDefinition::id);
            if (layer == layers.end() || !layer->enabled)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateCanonicalFixtureAuthority */
    Result<void> ValidateCanonicalFixtureAuthority(const CanonicalQueryAccess &access, const PhysicsQueryFixtureDescriptor &fixture) {
        if (!access.simulation.binding.schema)
            return Result<void>::Success();
        const auto valid = [&access](const auto &contributor) {
            const auto *profile = access.simulation.Profile(contributor.profile);
            return profile && profile->layer == contributor.layer && access.simulation.Channel(contributor.channel);
        };
        if (const auto *compound = std::get_if<PhysicsCompoundShapeDescriptor>(&fixture.shape)) {
            if (!std::ranges::all_of(compound->children, valid))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        } else if (!valid(fixture))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
