#include "TerrainAssetFormat.h"

#include <bit>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        constexpr std::uint64_t MaximumTerrainSamples = 1'048'576;
        constexpr std::size_t MaximumTerrainBytes = MaximumTerrainSamples * 4;

        struct Settings final {
            std::uint32_t width{}, height{};
            Terrain::TerrainSourceCoordinates coordinates;
        };

        /** @brief Closed typed settings resolution after generic Assets validation. */
        Result<Settings> ParseSettings(const std::vector<Assets::ImportSettingValue> &values) {
            using Return = Result<Settings>;
            if (values.size() != 7)
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::InvalidCoordinates));
            const auto width = std::get_if<std::int64_t>(&values[0]), height = std::get_if<std::int64_t>(&values[1]);
            if (!width || !height || *width < 2 || *height < 2 || *width > Terrain::TerrainDescriptorHardLimits::SamplesPerAxis ||
                *height > Terrain::TerrainDescriptorHardLimits::SamplesPerAxis ||
                std::uint64_t(*width) * std::uint64_t(*height) > MaximumTerrainSamples)
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::InvalidDimensions));
            const auto sx = std::get_if<double>(&values[2]), sz = std::get_if<double>(&values[3]), ox = std::get_if<double>(&values[4]),
                       oz = std::get_if<double>(&values[5]);
            const auto crs = std::get_if<std::string>(&values[6]);
            if (!sx || !sz || !ox || !oz || !crs || !std::isfinite(*sx) || !std::isfinite(*sz) || *sx <= 0 || *sz <= 0 ||
                !std::isfinite(*ox) || !std::isfinite(*oz) || crs->size() > 64 ||
                (!crs->empty() && (!crs->starts_with("EPSG:") || crs->size() == 5)))
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::InvalidCoordinates));
            if (!crs->empty())
                for (const char c : std::string_view(*crs).substr(5))
                    if (c < '0' || c > '9')
                        return Return::Failure(MakeError(Terrain::TerrainSourceErrors::InvalidCoordinates));
            Settings result{static_cast<std::uint32_t>(*width), static_cast<std::uint32_t>(*height)};
            result.coordinates.space =
                crs->empty() ? Terrain::TerrainCoordinateSpace::LocalMeters : Terrain::TerrainCoordinateSpace::ProjectedMeters;
            result.coordinates.projectedCrs = *crs;
            result.coordinates.spacingX = *sx;
            result.coordinates.spacingZ = *sz;
            result.coordinates.originX = *ox;
            result.coordinates.originZ = *oz;
            return Return::Success(std::move(result));
        }

        /** @brief The explicit importer validates source bytes without inventing a target asset or dataset identity. */
        class RawImporter final : public Assets::IAssetImporter {
        public:
            Result<Assets::PreparedAssetImport> Import(const Assets::AssetImportInput &input,
                                                       const CancellationToken &cancellation) const override {
                using Return = Result<Assets::PreparedAssetImport>;
                if (input.sourceExtension != "terrainraw")
                    return Return::Failure(MakeError(Terrain::TerrainSourceErrors::UnsupportedFormat));
                auto settings = ParseSettings(input.settings);
                if (settings.HasError())
                    return Return::Failure(std::move(settings).ErrorValue());
                const auto count = std::uint64_t{settings.Value().width} * settings.Value().height;
                if (input.sourceBytes.size() != count * 4)
                    return Return::Failure(MakeError(Terrain::TerrainSourceErrors::InvalidBytes));
                for (std::size_t index = 0; index < input.sourceBytes.size(); index += 4) {
                    if (index % 4096 == 0 && cancellation.IsCancellationRequested())
                        return Return::Failure(MakeError(Terrain::TerrainSourceErrors::Cancelled));
                    const auto *p = input.sourceBytes.data() + index;
                    const auto bits =
                        std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8U) | (std::uint32_t{p[2]} << 16U) | (std::uint32_t{p[3]} << 24U);
                    if (!std::isfinite(std::bit_cast<float>(bits)))
                        return Return::Failure(MakeError(Terrain::TerrainSourceErrors::InvalidSample));
                }
                Assets::PreparedAssetImport result;
                result.type = Assets::AssetTypeId::Parse("core.terrain").Value();
                result.editorPayload.assign(input.sourceBytes.begin(), input.sourceBytes.end());
                return Return::Success(std::move(result));
            }
        };
    }  // namespace

    /** @copydoc TerrainRawImporterContribution */
    Assets::AssetImporterContribution TerrainRawImporterContribution() {
        using namespace Assets;
        AssetImporterContribution result{.contributionId = std::string(TerrainRawImporterId),
                                         .packageId = "horo.builtin.assets",
                                         .moduleId = "horo.builtin.assets.importer.terrain",
                                         .moduleVersion = "1.0.0",
                                         .version = std::string(TerrainRawImporterVersion),
                                         .fileExtensions = {"terrainraw"},
                                         .assetTypes = {AssetTypeId::Parse("core.terrain").Value()},
                                         .subfolderCategory = "Terrain"};
        result.settings = {{.id = "terrain-width",
                            .labelKey = "terrain.import.width",
                            .descriptionKey = "terrain.import.width_help",
                            .kind = ImportSettingKind::Integer,
                            .defaultValue = std::int64_t{257},
                            .minimum = 2,
                            .maximum = 32769,
                            .includeInPresets = true},
                           {.id = "terrain-height",
                            .labelKey = "terrain.import.height",
                            .descriptionKey = "terrain.import.height_help",
                            .kind = ImportSettingKind::Integer,
                            .defaultValue = std::int64_t{257},
                            .minimum = 2,
                            .maximum = 32769,
                            .includeInPresets = true},
                           {.id = "terrain-spacing-x",
                            .labelKey = "terrain.import.spacing_x",
                            .descriptionKey = "terrain.import.spacing_help",
                            .kind = ImportSettingKind::Float,
                            .defaultValue = 1.0,
                            .minimum = 0.001,
                            .maximum = 1000000,
                            .includeInPresets = true},
                           {.id = "terrain-spacing-z",
                            .labelKey = "terrain.import.spacing_z",
                            .descriptionKey = "terrain.import.spacing_help",
                            .kind = ImportSettingKind::Float,
                            .defaultValue = 1.0,
                            .minimum = 0.001,
                            .maximum = 1000000,
                            .includeInPresets = true},
                           {.id = "terrain-origin-x",
                            .labelKey = "terrain.import.origin_x",
                            .descriptionKey = "terrain.import.origin_help",
                            .kind = ImportSettingKind::Float,
                            .defaultValue = 0.0,
                            .includeInPresets = true},
                           {.id = "terrain-origin-z",
                            .labelKey = "terrain.import.origin_z",
                            .descriptionKey = "terrain.import.origin_help",
                            .kind = ImportSettingKind::Float,
                            .defaultValue = 0.0,
                            .includeInPresets = true},
                           {.id = "terrain-crs",
                            .labelKey = "terrain.import.crs",
                            .descriptionKey = "terrain.import.crs_help",
                            .kind = ImportSettingKind::Text,
                            .defaultValue = std::string{},
                            .includeInPresets = true}};
        result.builtIn = true;
        result.strategy = std::make_shared<RawImporter>();
        result.previewFallback = AssetPreviewFallback::Automatic;
        return result;
    }

    /** @copydoc DecodeTerrainAsset */
    Result<Terrain::TerrainCanonicalSource> DecodeTerrainAsset(const Assets::AssetImportMetadata &metadata,
                                                               std::span<const std::byte> bytes, Terrain::TerrainSourceRevision revision,
                                                               const CancellationToken &cancellation) {
        using Return = Result<Terrain::TerrainCanonicalSource>;
        if (metadata.importerContributionId != TerrainRawImporterId || metadata.importerVersion != TerrainRawImporterVersion ||
            metadata.assetType.Value() != "core.terrain" || !metadata.assetId.IsValid())
            return Return::Failure(MakeError(Terrain::TerrainSourceErrors::UnsupportedFormat));
        auto contribution = TerrainRawImporterContribution();
        if (metadata.importerPackageId != contribution.packageId || metadata.importerModuleId != contribution.moduleId ||
            metadata.importerModuleVersion != contribution.moduleVersion || metadata.sourceExtension != "terrainraw" ||
            metadata.importSettings.size() != contribution.settings.size())
            return Return::Failure(MakeError(Terrain::TerrainSourceErrors::UnsupportedFormat));
        for (const auto &field : contribution.settings)
            if (!metadata.importSettings.contains("settings." + field.id))
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::InvalidCoordinates));
        auto resolved = Assets::ResolveImportSettings(contribution, metadata.importSettings);
        if (resolved.HasError())
            return Return::Failure(std::move(resolved).ErrorValue());
        auto settings = ParseSettings(resolved.Value());
        if (settings.HasError())
            return Return::Failure(std::move(settings).ErrorValue());
        auto dataset = Terrain::TerrainDatasetId::Create(metadata.assetId.Bytes());
        if (dataset.HasError())
            return Return::Failure(std::move(dataset).ErrorValue());
        Terrain::TerrainSourceImportRequest request{.dataset = dataset.Value(),
                                                    .sourceAsset = metadata.assetId,
                                                    .revision = revision,
                                                    .capability = Terrain::TerrainCapabilityRevision::Create(1).Value(),
                                                    .height = {.format = Terrain::TerrainRasterFormat::RawF32,
                                                               .bytes = bytes,
                                                               .width = settings.Value().width,
                                                               .height = settings.Value().height},
                                                    .coordinates = settings.Value().coordinates};
        request.limits.maximumSamples = MaximumTerrainSamples;
        request.limits.maximumCanonicalBytes = MaximumTerrainBytes;
        request.limits.maximumSourceBytes = MaximumTerrainBytes;
        request.limits.maximumWorkItems = MaximumTerrainSamples;
        return Terrain::NormalizeTerrainSource(request, cancellation);
    }

    /** @copydoc EncodeTerrainHeights */
    Result<std::vector<std::byte>> EncodeTerrainHeights(const Terrain::TerrainCanonicalSource &source,
                                                        const CancellationToken &cancellation) {
        using Return = Result<std::vector<std::byte>>;
        if (source.HasWeights() || source.HasHoles())
            return Return::Failure(MakeError(Terrain::TerrainSourceErrors::UnsupportedFormat));
        if (source.width < 2 || source.height < 2 || source.heightsMeters.size() != std::uint64_t{source.width} * source.height)
            return Return::Failure(MakeError(Terrain::TerrainSourceErrors::InvalidDimensions));
        if (source.heightsMeters.size() > MaximumTerrainSamples)
            return Return::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
        std::vector<std::byte> result(source.heightsMeters.size() * 4);
        for (std::size_t index = 0; index < source.heightsMeters.size(); ++index) {
            if (index % 1024 == 0 && cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::Cancelled));
            const auto value = source.heightsMeters[index];
            if (!std::isfinite(value))
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::InvalidSample));
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned byte = 0; byte < 4; ++byte)
                result[index * 4 + byte] = static_cast<std::byte>((bits >> (byte * 8U)) & 255U);
        }
        return Return::Success(std::move(result));
    }
}  // namespace Horo::Editor
