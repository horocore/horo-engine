#include "NavigationTileArtifactInternal.h"

namespace Horo::Navigation::TileArtifactInternal {
    namespace {
        /** @brief Writes finite capacity fields in their explicit unit-labelled portable widths. */
        void WriteCapacities(Writer &writer, const NavigationCapacityLimits &limits) {
            writer.Integer(limits.maximumAgents);
            writer.Integer(limits.maximumSurfaces);
            writer.Integer(limits.maximumResidentTiles);
            writer.Integer(limits.maximumConcurrentQueries);
            writer.Integer(limits.maximumBytesPerResidentTile, 8);
            writer.Integer(limits.maximumResidentMemoryBytes, 8);
            writer.Integer(limits.maximumWorkUnitsPerTick, 8);
        }

        /** @brief Reads fixed-width capacities before validating the complete profile. */
        [[nodiscard]] NavigationCapacityLimits ReadCapacities(Reader &reader) {
            NavigationCapacityLimits limits;
            limits.maximumAgents = static_cast<std::uint32_t>(reader.Integer());
            limits.maximumSurfaces = static_cast<std::uint32_t>(reader.Integer());
            limits.maximumResidentTiles = static_cast<std::uint32_t>(reader.Integer());
            limits.maximumConcurrentQueries = static_cast<std::uint32_t>(reader.Integer());
            limits.maximumBytesPerResidentTile = reader.Integer(8);
            limits.maximumResidentMemoryBytes = reader.Integer(8);
            limits.maximumWorkUnitsPerTick = reader.Integer(8);
            return limits;
        }

        /** @brief Rejects reserved authored profile identities at the portable format boundary. */
        template <typename Identity> [[nodiscard]] Identity ProfileIdentity(Reader &reader) {
            auto identity = Identity::Create(reader.Integer(8));
            if (identity.HasError())
                throw std::invalid_argument("Invalid captured navigation profile identity");
            return std::move(identity).Value();
        }
    }  // namespace

    /** @copydoc WriteContentProfile */
    void WriteContentProfile(Writer &writer, const NavigationProjectProfile &profile) {
        writer.Integer(profile.Id().Value(), 8);
        writer.Integer(profile.Revision().Value(), 8);
        writer.Integer(profile.Fingerprint().Value(), 8);
        WriteCapacities(writer, profile.Capacities());
        const auto &query = profile.MaximumQuery();
        writer.Integer(static_cast<std::uint8_t>(query.query), 1);
        writer.Integer(static_cast<std::uint8_t>(query.quality), 1);
        writer.Integer(query.limits.maximumNodeExpansions);
        writer.Integer(query.limits.maximumResultPoints);
        writer.Float(query.limits.maximumSearchDistanceMeters);
        for (std::size_t index = 0; index < static_cast<std::size_t>(NavigationCapability::Count); ++index)
            writer.Integer(static_cast<std::uint8_t>(profile.Requirement(static_cast<NavigationCapability>(index))), 1);
    }

    /** @copydoc ReadContentProfile */
    NavigationProjectProfile ReadContentProfile(Reader &reader) {
        NavigationProjectProfileInput input;
        input.id = ProfileIdentity<NavigationProjectProfileId>(reader);
        input.revision = ProfileIdentity<NavigationProjectProfileRevision>(reader);
        const auto fingerprint = reader.Integer(8);
        input.capacities = ReadCapacities(reader);
        input.maximumQuery.query = static_cast<NavigationQueryKind>(reader.Integer(1));
        input.maximumQuery.quality = static_cast<NavigationQualityLevel>(reader.Integer(1));
        input.maximumQuery.limits.maximumNodeExpansions = static_cast<std::uint32_t>(reader.Integer());
        input.maximumQuery.limits.maximumResultPoints = static_cast<std::uint32_t>(reader.Integer());
        input.maximumQuery.limits.maximumSearchDistanceMeters = reader.Float();
        for (auto &requirement : input.capabilities)
            requirement = static_cast<NavigationCapabilityRequirement>(reader.Integer(1));
        auto profile = NavigationProjectProfile::Create(input);
        if (profile.HasError() || profile.Value().Fingerprint().Value() != fingerprint)
            throw std::invalid_argument("Invalid captured navigation project profile");
        return std::move(profile).Value();
    }
}  // namespace Horo::Navigation::TileArtifactInternal
