#include "Horo/Terrain/FoliageAuthoringSource.h"

#include "FoliageSourceWire.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

namespace Horo::Terrain {
    namespace {
        constexpr std::uint32_t Magic = 0x31414648;  // HFA1, disjoint from raster and type sources.
        constexpr std::size_t HeaderBytes = 12;
        constexpr std::size_t PlacementBytes = 52;
        constexpr std::size_t ReferenceBytes = 72;

        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
        }

        /** @brief Admit all allocation and conservative sort/lookup work before copying or decoding collections. */
        Result<void> Admit(std::size_t placements, std::size_t definitions, const FoliageAuthoringSourceLimits &limits) {
            if (auto measured = MeasureFoliageAuthoringSource(placements, definitions, limits); measured.HasError())
                return Result<void>::Failure(measured.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Match the existing document's placement transform admission exactly. */
        bool ValidPlacement(const TerrainAuthoredPlacement &value, std::uint32_t width, std::uint32_t height) {
            if (!value.id.IsValid() || !value.type.IsValid() || value.sampleX >= width || value.sampleZ >= height)
                return false;
            return std::isfinite(value.offsetMeters) && std::isfinite(value.scale) && value.scale > 0 && std::isfinite(value.yawRadians);
        }

        /** @brief Validate actual unique definition references and retain only the type lookup needed by placements. */
        Result<std::set<FoliageTypeId>> DefinitionTypes(const FoliageAuthoringSource &source, const CancellationToken &cancel) {
            std::set<FoliageTypeId> types;
            std::set<Assets::AssetId> assets;
            for (const auto &reference : source.definitions) {
                if (cancel.IsCancellationRequested())
                    return Result<std::set<FoliageTypeId>>::Failure(MakeError(TerrainSourceErrors::Cancelled));
                if (!reference.asset.IsValid() || !reference.type.IsValid() || !reference.revision.IsValid())
                    return Invalid<std::set<FoliageTypeId>>();
                if (!types.insert(reference.type).second || !assets.insert(reference.asset).second)
                    return Invalid<std::set<FoliageTypeId>>();
            }
            return Result<std::set<FoliageTypeId>>::Success(std::move(types));
        }

        /** @brief Write exact persisted identity and transform bits, without cooked state. */
        void WritePlacement(FoliageWire::Writer &writer, const TerrainAuthoredPlacement &value) {
            writer.Bytes(value.id.Bytes());
            writer.Bytes(value.type.Bytes());
            writer.Integer(value.sampleX);
            writer.Integer(value.sampleZ);
            writer.Float(value.offsetMeters);
            writer.Float(value.scale);
            writer.Float(value.yawRadians);
        }

        /** @brief Read a bounded placement record; typed zero identities are rejected before publication. */
        bool ReadPlacement(FoliageWire::Reader &reader, TerrainAuthoredPlacement &value) {
            SerializedTerrainIdentity id{};
            SerializedTerrainIdentity type{};
            if (!reader.Bytes(id) || !reader.Bytes(type))
                return false;
            auto instance = FoliageInstanceId::Create(id);
            auto definition = FoliageTypeId::Create(type);
            if (instance.HasError() || definition.HasError())
                return false;
            value.id = instance.Value();
            value.type = definition.Value();
            return reader.Integer(value.sampleX) && reader.Integer(value.sampleZ) && reader.Float(value.offsetMeters) &&
                   reader.Float(value.scale) && reader.Float(value.yawRadians);
        }

        /** @brief Write an immutable actual asset binding. */
        void WriteReference(FoliageWire::Writer &writer, const FoliageAuthoringDefinitionReference &value) {
            writer.Bytes(value.asset.Bytes());
            writer.Bytes(value.type.Bytes());
            writer.Integer(value.revision.Value());
            writer.Bytes(value.sourceDigest.bytes);
        }

        /** @brief Read a fixed-size actual asset binding. */
        bool ReadReference(FoliageWire::Reader &reader, FoliageAuthoringDefinitionReference &value) {
            SerializedTerrainIdentity asset{};
            SerializedTerrainIdentity type{};
            std::uint64_t revision{};
            if (!reader.Bytes(asset) || !reader.Bytes(type) || !reader.Integer(revision) || !reader.Bytes(value.sourceDigest.bytes))
                return false;
            auto typed = FoliageTypeId::Create(type);
            auto rev = FoliageDefinitionRevision::Create(revision);
            if (typed.HasError() || rev.HasError())
                return false;
            value.asset = Assets::AssetId::FromBytes(asset);
            value.type = typed.Value();
            value.revision = rev.Value();
            return value.asset.IsValid();
        }

        /** @brief Read admitted collections with cancellation at every fixed-size record. */
        Result<FoliageAuthoringSource> ReadSource(FoliageWire::Reader &reader, std::uint32_t placements, std::uint32_t definitions,
                                                  const CancellationToken &cancel) {
            FoliageAuthoringSource source;
            source.placements.resize(placements);
            source.definitions.resize(definitions);
            for (auto &value : source.placements) {
                if (cancel.IsCancellationRequested())
                    return Result<FoliageAuthoringSource>::Failure(MakeError(TerrainSourceErrors::Cancelled));
                if (!ReadPlacement(reader, value))
                    return Invalid<FoliageAuthoringSource>();
            }
            for (auto &value : source.definitions) {
                if (cancel.IsCancellationRequested())
                    return Result<FoliageAuthoringSource>::Failure(MakeError(TerrainSourceErrors::Cancelled));
                if (!ReadReference(reader, value))
                    return Invalid<FoliageAuthoringSource>();
            }
            return Result<FoliageAuthoringSource>::Success(std::move(source));
        }

        /** @brief Validate stable placement uniqueness and complete referenced type use. */
        Result<void> ValidatePlacements(const FoliageAuthoringSource &source, const std::set<FoliageTypeId> &types, std::uint32_t width,
                                        std::uint32_t height, const CancellationToken &cancel) {
            std::set<FoliageInstanceId> ids;
            std::set<FoliageTypeId> used;
            for (const auto &value : source.placements) {
                if (cancel.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainSourceErrors::Cancelled));
                if (!ValidPlacement(value, width, height) || !ids.insert(value.id).second || !types.contains(value.type))
                    return Invalid<void>();
                used.insert(value.type);
            }
            if (used != types)
                return Invalid<void>();
            return Result<void>::Success();
        }

        /** @brief Read exact supported record counts and admit byte/work policy before allocation. */
        Result<void> ReadHeader(FoliageWire::Reader &reader, std::size_t bytes, const FoliageAuthoringSourceLimits &limits,
                                std::uint32_t &placements, std::uint32_t &definitions) {
            if (bytes > limits.maximumBytes)
                return Result<void>::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
            std::uint32_t magic{};
            if (!reader.Integer(magic) || !reader.Integer(placements) || !reader.Integer(definitions) || magic != Magic)
                return Invalid<void>();
            if (auto admitted = Admit(placements, definitions, limits); admitted.HasError())
                return admitted;
            if (bytes != HeaderBytes + placements * PlacementBytes + definitions * ReferenceBytes)
                return Invalid<void>();
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MeasureFoliageAuthoringSource */
    Result<FoliageAuthoringSourceFootprint> MeasureFoliageAuthoringSource(std::size_t placements, std::size_t definitions,
                                                                          const FoliageAuthoringSourceLimits &limits) {
        if (!limits.maximumBytes || !limits.maximumWorkItems || limits.maximumPlacements > 65'536 || limits.maximumDefinitions > 256)
            return Invalid<FoliageAuthoringSourceFootprint>();
        if (placements > limits.maximumPlacements || definitions > limits.maximumDefinitions)
            return Result<FoliageAuthoringSourceFootprint>::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
        const auto bytes = HeaderBytes + placements * PlacementBytes + definitions * ReferenceBytes;
        const auto placementWork = placements * (2 * std::bit_width(placements) + 2 * std::bit_width(definitions) + 32);
        const auto work = placementWork + definitions * (4 * std::bit_width(definitions) + 16);
        if (bytes > limits.maximumBytes || work > limits.maximumWorkItems)
            return Result<FoliageAuthoringSourceFootprint>::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
        return Result<FoliageAuthoringSourceFootprint>::Success({bytes, work});
    }

    /** @copydoc MeasureEncodedFoliageAuthoringSource */
    Result<FoliageAuthoringSourceFootprint> MeasureEncodedFoliageAuthoringSource(std::span<const std::byte> bytes,
                                                                                 const FoliageAuthoringSourceLimits &limits) {
        FoliageWire::Reader reader{bytes};
        std::uint32_t placements{};
        std::uint32_t definitions{};
        if (auto admitted = ReadHeader(reader, bytes.size(), limits, placements, definitions); admitted.HasError())
            return Result<FoliageAuthoringSourceFootprint>::Failure(admitted.ErrorValue());
        return MeasureFoliageAuthoringSource(placements, definitions, limits);
    }

    /** @copydoc ValidateFoliageAuthoringSource */
    Result<void> ValidateFoliageAuthoringSource(const FoliageAuthoringSource &source, std::uint32_t width, std::uint32_t height,
                                                const FoliageAuthoringSourceLimits &limits, const CancellationToken &cancel) {
        if (auto admitted = Admit(source.placements.size(), source.definitions.size(), limits); admitted.HasError())
            return admitted;
        if (cancel.IsCancellationRequested())
            return Result<void>::Failure(MakeError(TerrainSourceErrors::Cancelled));
        if (width < 2 || height < 2)
            return Invalid<void>();
        auto types = DefinitionTypes(source, cancel);
        if (types.HasError())
            return Result<void>::Failure(types.ErrorValue());
        return ValidatePlacements(source, types.Value(), width, height, cancel);
    }

    /** @copydoc EncodeFoliageAuthoringSource */
    Result<std::vector<std::byte>> EncodeFoliageAuthoringSource(const FoliageAuthoringSource &source, std::uint32_t width,
                                                                std::uint32_t height, const FoliageAuthoringSourceLimits &limits,
                                                                const CancellationToken &cancel) {
        if (auto valid = ValidateFoliageAuthoringSource(source, width, height, limits, cancel); valid.HasError())
            return Result<std::vector<std::byte>>::Failure(valid.ErrorValue());
        auto ordered = source;
        std::ranges::sort(ordered.placements, {}, &TerrainAuthoredPlacement::id);
        std::ranges::sort(ordered.definitions, {}, &FoliageAuthoringDefinitionReference::type);
        FoliageWire::Writer writer;
        writer.bytes.reserve(HeaderBytes + source.placements.size() * PlacementBytes + source.definitions.size() * ReferenceBytes);
        writer.Integer(Magic);
        writer.Integer(static_cast<std::uint32_t>(ordered.placements.size()));
        writer.Integer(static_cast<std::uint32_t>(ordered.definitions.size()));
        for (const auto &value : ordered.placements) {
            if (cancel.IsCancellationRequested())
                return Result<std::vector<std::byte>>::Failure(MakeError(TerrainSourceErrors::Cancelled));
            WritePlacement(writer, value);
        }
        for (const auto &value : ordered.definitions)
            WriteReference(writer, value);
        return Result<std::vector<std::byte>>::Success(std::move(writer.bytes));
    }

    /** @copydoc DecodeFoliageAuthoringSource */
    Result<FoliageAuthoringSource> DecodeFoliageAuthoringSource(std::span<const std::byte> bytes, std::uint32_t width, std::uint32_t height,
                                                                const FoliageAuthoringSourceLimits &limits,
                                                                const CancellationToken &cancel) {
        FoliageWire::Reader reader{bytes};
        std::uint32_t placements{};
        std::uint32_t definitions{};
        if (auto admitted = ReadHeader(reader, bytes.size(), limits, placements, definitions); admitted.HasError())
            return Result<FoliageAuthoringSource>::Failure(admitted.ErrorValue());
        auto source = ReadSource(reader, placements, definitions, cancel);
        if (source.HasError())
            return source;
        auto canonical = EncodeFoliageAuthoringSource(source.Value(), width, height, limits, cancel);
        if (canonical.HasError())
            return Result<FoliageAuthoringSource>::Failure(canonical.ErrorValue());
        if (!std::ranges::equal(bytes, canonical.Value()))
            return Invalid<FoliageAuthoringSource>();
        return source;
    }
}  // namespace Horo::Terrain
