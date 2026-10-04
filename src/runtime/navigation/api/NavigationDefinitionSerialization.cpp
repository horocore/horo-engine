#include "NavigationDefinitionCodecInternal.h"

#include <array>
#include <new>

namespace Horo::Navigation {
    namespace {
        /** @brief Writes one grounded profile's stable identity, UTF-8 name and seven geometry measures. */
        void WriteProfile(DefinitionCodec::Writer &writer, const NavigationAgentProfileDescriptor &profile) {
            writer.U64(profile.id.Value());
            writer.Text(profile.displayName);
            const auto &geometry = profile.buildGeometry;
            const std::array values{geometry.radiusMeters,           geometry.heightMeters,   geometry.maxSlopeDegrees,
                                    geometry.stepHeightMeters,       geometry.cellSizeMeters, geometry.cellHeightMeters,
                                    geometry.minimumRegionSizeMeters};
            for (const float value : values)
                writer.Float(value);
        }

        /** @brief Keeps project/package provenance explicit in durable descriptor rows. */
        void WriteSource(DefinitionCodec::Writer &writer, const NavigationDescriptorSource &source) {
            writer.U32(static_cast<std::uint32_t>(source.kind));
            writer.U64(source.id.Value());
        }

        /** @brief Writes identity-sorted area and filter tables, with no localized/generated fields. */
        void WriteRegistry(DefinitionCodec::Writer &writer, const NavigationAreaRegistry &registry) {
            writer.U32(static_cast<std::uint32_t>(registry.Areas().size()));
            for (const auto &area : registry.Areas()) {
                writer.U64(area.id.Value());
                WriteSource(writer, area.source);
                writer.Float(area.traversalCost);
                writer.U64(area.flags.bits);
            }
            writer.U32(static_cast<std::uint32_t>(registry.Filters().size()));
            for (const auto &filter : registry.Filters()) {
                writer.U64(filter.id.Value());
                WriteSource(writer, filter.source);
                writer.U64(filter.includedFlags.bits);
                writer.U64(filter.excludedFlags.bits);
                writer.U32(static_cast<std::uint32_t>(filter.costOverrides.size()));
                for (const auto &cost : filter.costOverrides) {
                    writer.U64(cost.area.Value());
                    writer.Float(cost.traversalCost);
                }
            }
        }
    }  // namespace

    /** @copydoc DefinitionCodec::EncodePayload */
    std::vector<std::byte> DefinitionCodec::EncodePayload(const NavigationDefinition &definition) {
        Writer writer;
        writer.U32(static_cast<std::uint32_t>(definition.Coordinates().system));
        writer.Float(definition.Coordinates().metersPerUnit);
        writer.U32(definition.Tiles().tileSizeCells);
        writer.U32(definition.Tiles().maximumTiles);
        writer.U32(static_cast<std::uint32_t>(definition.Scope()));
        writer.U32(static_cast<std::uint32_t>(definition.Profiles().size()));
        for (const auto &profile : definition.Profiles())
            WriteProfile(writer, profile);
        WriteRegistry(writer, definition.Registry());
        return std::move(writer.bytes);
    }

    /** @copydoc EncodeNavigationDefinitionRecord */
    Result<NavigationAuthoredRecord> EncodeNavigationDefinitionRecord(const NavigationDefinition &definition,
                                                                      const NavigationAuthoredRecordId recordId) {
        if (!recordId.IsValid())
            return Result<NavigationAuthoredRecord>::Failure(MakeError(NavigationErrors::IdentityInvalid));
        try {
            auto payload = DefinitionCodec::EncodePayload(definition);
            if (payload.size() > NavigationSerializationLimits::MaximumRecordPayloadBytes)
                return Result<NavigationAuthoredRecord>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
            return Result<NavigationAuthoredRecord>::Success({.id = recordId,
                                                              .type = NavigationDefinitionRecordSupport().type,
                                                              .version = NavigationDefinitionPayloadVersion,
                                                              .required = true,
                                                              .payload = std::move(payload),
                                                              .opaque = false});
        } catch (const std::bad_alloc &) {
            return Result<NavigationAuthoredRecord>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
        }
    }
}  // namespace Horo::Navigation
