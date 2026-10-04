#include "NavigationDefinitionCodecInternal.h"

#include <new>

namespace Horo::Navigation {
    namespace {
        /** @brief Reads the closed enum domain before narrowing its portable uint32 representation. */
        template <typename Enum> Enum ReadEnum(DefinitionCodec::Reader &reader) {
            const auto value = reader.U32();
            if (value >= static_cast<std::uint32_t>(Enum::Count))
                throw DefinitionCodec::InvalidPayload{};
            return static_cast<Enum>(value);
        }

        /** @brief Reads all grounded geometry fields without supplying absent defaults. */
        NavigationAgentProfileDescriptor ReadProfile(DefinitionCodec::Reader &reader) {
            NavigationAgentProfileDescriptor profile;
            profile.id = reader.Id<NavigationAgentProfileId>();
            profile.displayName = reader.Text();
            profile.buildGeometry = {.radiusMeters = reader.Float(),
                                     .heightMeters = reader.Float(),
                                     .maxSlopeDegrees = reader.Float(),
                                     .stepHeightMeters = reader.Float(),
                                     .cellSizeMeters = reader.Float(),
                                     .cellHeightMeters = reader.Float(),
                                     .minimumRegionSizeMeters = reader.Float()};
            return profile;
        }

        /** @brief Preserves explicit descriptor ownership without inferring package/project identities. */
        NavigationDescriptorSource ReadSource(DefinitionCodec::Reader &reader) {
            return {.kind = ReadEnum<NavigationDescriptorSourceKind>(reader), .id = reader.Id<NavigationDescriptorSourceId>()};
        }

        /** @brief Parses bounded area rows before the registry performs semantic admission. */
        void ReadAreas(DefinitionCodec::Reader &reader, NavigationDefinitionInput &input) {
            const auto count = reader.Count(NavigationDefinition::MaximumAreas, 32);
            input.areas.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index)
                input.areas.push_back({.id = reader.Id<NavigationAreaId>(),
                                       .source = ReadSource(reader),
                                       .traversalCost = reader.Float(),
                                       .flags = {reader.U64()}});
        }

        /** @brief Binds exact referenced costs; missing areas are rejected by the shared registry. */
        NavigationQueryFilterDescriptor ReadFilter(DefinitionCodec::Reader &reader) {
            NavigationQueryFilterDescriptor filter;
            filter.id = reader.Id<NavigationFilterId>();
            filter.source = ReadSource(reader);
            filter.includedFlags = {reader.U64()};
            filter.excludedFlags = {reader.U64()};
            const auto count = reader.Count(NavigationDefinition::MaximumAreas, 12);
            filter.costOverrides.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index)
                filter.costOverrides.push_back({.area = reader.Id<NavigationAreaId>(), .traversalCost = reader.Float()});
            return filter;
        }

        /** @brief Rejects wrong identity, opaque or optional records before semantic payload interpretation. */
        Result<void> ValidateRecord(const NavigationAuthoredRecord &record) {
            if (!record.id.IsValid() || record.type != NavigationDefinitionRecordSupport().type || !record.required || record.opaque)
                return Result<void>::Failure(MakeError(NavigationErrors::SourceUnknownAuthoredRecord));
            if (record.version != NavigationDefinitionPayloadVersion)
                return Result<void>::Failure(MakeError(NavigationErrors::SourceUnsupportedVersion));
            if (record.payload.size() > NavigationSerializationLimits::MaximumRecordPayloadBytes)
                return Result<void>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc DefinitionCodec::DecodePayload */
    NavigationDefinitionInput DefinitionCodec::DecodePayload(const std::span<const std::byte> bytes) {
        Reader reader{.bytes = bytes};
        NavigationDefinitionInput input;
        input.coordinates = {.system = ReadEnum<NavigationSourceCoordinateSystem>(reader), .metersPerUnit = reader.Float()};
        input.tiles = {.tileSizeCells = reader.U32(), .maximumTiles = reader.U32()};
        input.scope = ReadEnum<NavigationDefinitionScope>(reader);
        const auto profileCount = reader.Count(NavigationDefinition::MaximumProfiles, 40);
        input.profiles.reserve(profileCount);
        for (std::uint32_t index = 0; index < profileCount; ++index)
            input.profiles.push_back(ReadProfile(reader));
        ReadAreas(reader, input);
        const auto filterCount = reader.Count(NavigationDefinition::MaximumFilters, 40);
        input.filters.reserve(filterCount);
        for (std::uint32_t index = 0; index < filterCount; ++index)
            input.filters.push_back(ReadFilter(reader));
        if (reader.offset != bytes.size())
            throw InvalidPayload{};
        return input;
    }

    /** @copydoc DecodeNavigationDefinitionRecord */
    Result<NavigationDefinition> DecodeNavigationDefinitionRecord(const NavigationAuthoredRecord &record) {
        if (const auto valid = ValidateRecord(record); valid.HasError())
            return Result<NavigationDefinition>::Failure(valid.ErrorValue());
        try {
            auto definition = NavigationDefinition::Create(DefinitionCodec::DecodePayload(record.payload));
            if (definition.HasError())
                return definition;
            if (DefinitionCodec::EncodePayload(definition.Value()) != record.payload)
                return Result<NavigationDefinition>::Failure(MakeError(NavigationErrors::SourceEnvelopeInvalid));
            return definition;
        } catch (const DefinitionCodec::InvalidPayload &) {
            return Result<NavigationDefinition>::Failure(MakeError(NavigationErrors::SourceEnvelopeInvalid));
        } catch (const std::bad_alloc &) {
            return Result<NavigationDefinition>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
        }
    }
}  // namespace Horo::Navigation
