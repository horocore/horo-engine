#include "Horo/Terrain/TerrainSourceImport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string_view>
#include <utility>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace Horo::Terrain {
    namespace TerrainSourceErrors {
        namespace {
            const ErrorDomainId Domain{"horo.terrain.import"};
        }

        const ErrorCodeDescriptor UnsupportedFormat{.domain = Domain,
                                                    .code = ErrorCode{"terrain.import.unsupported_format"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "The terrain raster format is unsupported for this channel.",
                                                    .remediationHint = "Convert height to explicit RAW U16/F32, weights to RAW U8/U16, and "
                                                                       "holes to binary RAW U8; supply byte and row order.",
                                                    .retryable = false,
                                                    .userActionable = true,
                                                    .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor
            InvalidDimensions{.domain = Domain,
                              .code = ErrorCode{"terrain.import.invalid_dimensions"},
                              .defaultSeverity = ErrorSeverity::Error,
                              .summary = "Terrain source dimensions are empty, mismatched, or exceed the grid ceiling.",
                              .remediationHint =
                                  "Use one equal-sized grid of at least two samples per axis within captured project limits.",
                              .retryable = false,
                              .userActionable = true,
                              .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor
            InvalidBytes{.domain = Domain,
                         .code = ErrorCode{"terrain.import.invalid_bytes"},
                         .defaultSeverity = ErrorSeverity::Error,
                         .summary = "Terrain raster byte length does not match its declared shape and scalar encoding.",
                         .remediationHint =
                             "Provide exact tightly packed single-channel bytes without a header, padding, or trailing data.",
                         .retryable = false,
                         .userActionable = true,
                         .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor InvalidCoordinates{.domain = Domain,
                                                     .code = ErrorCode{"terrain.import.invalid_coordinates"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary =
                                                         "Terrain coordinate metadata is incomplete, non-finite, or requires reprojection.",
                                                     .remediationHint =
                                                         "Provide finite meter coordinates and positive spacing/scale; project geographic "
                                                         "degrees upstream and name a projected EPSG CRS.",
                                                     .retryable = false,
                                                     .userActionable = true,
                                                     .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor
            PrecisionLost{.domain = Domain,
                          .code = ErrorCode{"terrain.import.precision_lost"},
                          .defaultSeverity = ErrorSeverity::Error,
                          .summary = "Canonical float32 meters exceed the requested precision error.",
                          .remediationHint = "Adjust source units/offset or explicitly allow a larger finite meter error before import.",
                          .retryable = false,
                          .userActionable = true,
                          .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor InvalidSample{.domain = Domain,
                                                .code = ErrorCode{"terrain.import.invalid_sample"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary =
                                                    "Terrain raster contains a non-finite height, empty weight pixel, or non-binary hole.",
                                                .remediationHint = "Replace no-data/NaN/inf heights, supply positive weight at each pixel, "
                                                                   "and encode holes as exact zero or one.",
                                                .retryable = false,
                                                .userActionable = true,
                                                .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor
            LimitExceeded{.domain = Domain,
                          .code = ErrorCode{"terrain.import.limit_exceeded"},
                          .defaultSeverity = ErrorSeverity::Error,
                          .summary = "Terrain source samples, bytes, layers, or decode work exceed captured limits.",
                          .remediationHint = "Split or reduce the source, or select a compatible larger project import budget explicitly.",
                          .retryable = false,
                          .userActionable = true,
                          .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                            .code = ErrorCode{"terrain.import.cancelled"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "Terrain source normalization was cancelled before publication.",
                                            .remediationHint = "Start a new import operation when the source is still needed.",
                                            .retryable = true,
                                            .userActionable = false,
                                            .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor DecoderFailed{.domain = Domain,
                                                .code = ErrorCode{"terrain.import.decoder_failed"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "An optional terrain raster decoder failed outside its typed result contract.",
                                                .remediationHint =
                                                    "Disable or repair the exact importer contribution and retry with a supported source.",
                                                .retryable = false,
                                                .userActionable = true,
                                                .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor
            RevisionStale{.domain = Domain,
                          .code = ErrorCode{"terrain.import.revision_stale"},
                          .defaultSeverity = ErrorSeverity::Warning,
                          .summary = "Terrain source changed since import began or the candidate revision is not newer.",
                          .remediationHint =
                              "Reimport against the current authoring revision; do not overwrite intervening sculpt or paint edits.",
                          .retryable = true,
                          .userActionable = true,
                          .deprecatedBy = std::nullopt};
        const ErrorCodeDescriptor Closed{.domain = Domain,
                                         .code = ErrorCode{"terrain.import.closed"},
                                         .defaultSeverity = ErrorSeverity::Warning,
                                         .summary = "The terrain authoring document is closed to publication.",
                                         .remediationHint = "Open a new document owner before importing.",
                                         .retryable = false,
                                         .userActionable = false,
                                         .deprecatedBy = std::nullopt};
    }  // namespace TerrainSourceErrors

    namespace {
        template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &code, const std::string_view detail = {}) {
            return Result<T>::Failure(MakeError(code, std::string(detail)));
        }

        struct DecodedPngRaster final {
            TerrainRasterFormat format{TerrainRasterFormat::Count};
            std::vector<std::byte> bytes;
        };

        [[nodiscard]] std::uint32_t ReadBits(const TerrainRasterInput &raster, const std::uint64_t sourceIndex,
                                             const std::uint32_t bytesPerSample) noexcept {
            const std::size_t offset = static_cast<std::size_t>(sourceIndex * bytesPerSample);
            std::uint32_t bits = 0;
            if (raster.byteOrder == TerrainByteOrder::Little) {
                for (std::uint32_t i = 0; i < bytesPerSample; ++i)
                    bits |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(raster.bytes[offset + i])) << (8U * i);
            } else {
                for (std::uint32_t i = 0; i < bytesPerSample; ++i)
                    bits = (bits << 8U) | std::to_integer<std::uint8_t>(raster.bytes[offset + i]);
            }
            return bits;
        }

        [[nodiscard]] std::uint32_t ReadBig32(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
            return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) | (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16U) |
                   (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8U) | std::to_integer<std::uint32_t>(bytes[offset + 3]);
        }

        [[nodiscard]] Result<DecodedPngRaster> DecodePngGray(const TerrainRasterInput &source, const std::uint64_t maximumDecodedBytes) {
            static constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
            if (source.bytes.size() < 33 || source.bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return Failed<DecodedPngRaster>(TerrainSourceErrors::InvalidBytes);
            for (std::size_t i = 0; i < signature.size(); ++i) {
                if (std::to_integer<std::uint8_t>(source.bytes[i]) != signature[i])
                    return Failed<DecodedPngRaster>(TerrainSourceErrors::InvalidBytes);
            }
            if (ReadBig32(source.bytes, 8) != 13 || source.bytes[12] != std::byte{'I'} || source.bytes[13] != std::byte{'H'} ||
                source.bytes[14] != std::byte{'D'} || source.bytes[15] != std::byte{'R'})
                return Failed<DecodedPngRaster>(TerrainSourceErrors::InvalidBytes);
            const std::uint32_t width = ReadBig32(source.bytes, 16);
            const std::uint32_t height = ReadBig32(source.bytes, 20);
            const std::uint8_t bitDepth = std::to_integer<std::uint8_t>(source.bytes[24]);
            const std::uint8_t colorType = std::to_integer<std::uint8_t>(source.bytes[25]);
            if (colorType != 0 || (bitDepth != 8 && bitDepth != 16) || source.bytes[26] != std::byte{0} ||
                source.bytes[27] != std::byte{0} || source.bytes[28] != std::byte{0})
                return Failed<DecodedPngRaster>(TerrainSourceErrors::UnsupportedFormat,
                                                "Only non-interlaced grayscale PNG with 8 or 16-bit samples is supported.");
            if (width != source.width || height != source.height)
                return Failed<DecodedPngRaster>(TerrainSourceErrors::InvalidDimensions);
            const std::uint64_t decodedBytes = static_cast<std::uint64_t>(width) * height * (bitDepth / 8U);
            if (decodedBytes > maximumDecodedBytes)
                return Failed<DecodedPngRaster>(TerrainSourceErrors::LimitExceeded);
            const auto *input = reinterpret_cast<const stbi_uc *>(source.bytes.data());
            int decodedWidth = 0;
            int decodedHeight = 0;
            int components = 0;
            DecodedPngRaster result;
            result.format = bitDepth == 8 ? TerrainRasterFormat::RawU8 : TerrainRasterFormat::RawU16;
            result.bytes.resize(static_cast<std::size_t>(decodedBytes));
            if (bitDepth == 8) {
                stbi_uc *pixels =
                    stbi_load_from_memory(input, static_cast<int>(source.bytes.size()), &decodedWidth, &decodedHeight, &components, 1);
                if (pixels == nullptr)
                    return Failed<DecodedPngRaster>(TerrainSourceErrors::InvalidBytes);
                std::copy_n(reinterpret_cast<const std::byte *>(pixels), result.bytes.size(), result.bytes.begin());
                stbi_image_free(pixels);
            } else {
                stbi_us *pixels =
                    stbi_load_16_from_memory(input, static_cast<int>(source.bytes.size()), &decodedWidth, &decodedHeight, &components, 1);
                if (pixels == nullptr)
                    return Failed<DecodedPngRaster>(TerrainSourceErrors::InvalidBytes);
                for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(width) * height; ++i) {
                    result.bytes[static_cast<std::size_t>(2 * i)] = static_cast<std::byte>(pixels[i] & 0xFFU);
                    result.bytes[static_cast<std::size_t>(2 * i + 1)] = static_cast<std::byte>(pixels[i] >> 8U);
                }
                stbi_image_free(pixels);
            }
            if (decodedWidth != static_cast<int>(width) || decodedHeight != static_cast<int>(height) || components != 1)
                return Failed<DecodedPngRaster>(TerrainSourceErrors::InvalidBytes);
            return Result<DecodedPngRaster>::Success(std::move(result));
        }

        [[nodiscard]] std::uint64_t SourceIndex(const TerrainRasterInput &raster, const std::uint32_t x, const std::uint32_t z) noexcept {
            const std::uint32_t row = raster.rowOrder == TerrainRowOrder::IncreasingZ ? z : raster.height - 1U - z;
            return static_cast<std::uint64_t>(row) * raster.width + x;
        }

        [[nodiscard]] bool ValidProjectedCrs(const std::string_view crs) noexcept {
            if (!crs.starts_with("EPSG:") || crs.size() < 6 || crs.size() > 16)
                return false;
            return std::ranges::all_of(crs.substr(5), [](const char c) {
                return c >= '0' && c <= '9';
            });
        }

        [[nodiscard]] bool ValidCoordinates(const TerrainSourceCoordinates &coordinates, const std::uint32_t width,
                                            const std::uint32_t height) noexcept {
            if (coordinates.space == TerrainCoordinateSpace::GeographicDegrees || coordinates.space == TerrainCoordinateSpace::Count ||
                (coordinates.space == TerrainCoordinateSpace::LocalMeters && !coordinates.projectedCrs.empty()) ||
                (coordinates.space == TerrainCoordinateSpace::ProjectedMeters && !ValidProjectedCrs(coordinates.projectedCrs)))
                return false;
            if (!std::isfinite(coordinates.originX) || !std::isfinite(coordinates.originZ) || !std::isfinite(coordinates.spacingX) ||
                !std::isfinite(coordinates.spacingZ) || !std::isfinite(coordinates.heightScale) ||
                !std::isfinite(coordinates.heightOffset) || !std::isfinite(coordinates.maximumPrecisionError) ||
                coordinates.spacingX <= 0 || coordinates.spacingZ <= 0 || coordinates.heightScale <= 0 ||
                coordinates.maximumPrecisionError < 0)
                return false;
            return std::isfinite(coordinates.originX + static_cast<double>(width - 1U) * coordinates.spacingX) &&
                   std::isfinite(coordinates.originZ + static_cast<double>(height - 1U) * coordinates.spacingZ);
        }

        [[nodiscard]] Result<std::uint32_t> ValidateRaster(const TerrainRasterInput &raster, const std::uint32_t width,
                                                           const std::uint32_t height, const bool allowU8, const bool allowF32) {
            if (raster.width != width || raster.height != height)
                return Failed<std::uint32_t>(TerrainSourceErrors::InvalidDimensions);
            if (raster.rowOrder == TerrainRowOrder::Count || raster.byteOrder == TerrainByteOrder::Count)
                return Failed<std::uint32_t>(TerrainSourceErrors::UnsupportedFormat);
            std::uint32_t bytesPerSample = 0;
            switch (raster.format) {
                case TerrainRasterFormat::RawU8:
                    bytesPerSample = allowU8 ? 1U : 0U;
                    break;
                case TerrainRasterFormat::RawU16:
                    bytesPerSample = 2U;
                    break;
                case TerrainRasterFormat::RawF32:
                    bytesPerSample = allowF32 ? 4U : 0U;
                    break;
                case TerrainRasterFormat::PngGray:
                case TerrainRasterFormat::External:
                case TerrainRasterFormat::Count:
                    break;
            }
            if (bytesPerSample == 0)
                return Failed<std::uint32_t>(TerrainSourceErrors::UnsupportedFormat);
            const std::uint64_t required = static_cast<std::uint64_t>(width) * height * bytesPerSample;
            if (raster.bytes.size() != required)
                return Failed<std::uint32_t>(TerrainSourceErrors::InvalidBytes);
            return Result<std::uint32_t>::Success(bytesPerSample);
        }

        [[nodiscard]] bool ValidCandidate(const TerrainCanonicalSource &candidate) noexcept {
            const std::uint64_t samples = static_cast<std::uint64_t>(candidate.width) * candidate.height;
            if (!(candidate.dataset.IsValid() && candidate.sourceAsset.IsValid() && candidate.revision.IsValid() &&
                  candidate.capability.IsValid() && candidate.width >= 2 && candidate.height >= 2 &&
                  candidate.width <= TerrainDescriptorHardLimits::SamplesPerAxis &&
                  candidate.height <= TerrainDescriptorHardLimits::SamplesPerAxis && samples <= TerrainDescriptorHardLimits::WorkItems &&
                  candidate.heightsMeters.size() == samples && candidate.weights.size() == samples * candidate.layerCount &&
                  (candidate.holes.empty() || candidate.holes.size() == samples) &&
                  candidate.layerCount <= TerrainDescriptorHardLimits::LayersPerTile &&
                  ValidCoordinates(candidate.coordinates, candidate.width, candidate.height)))
                return false;
            for (std::size_t i = 0; i < samples; ++i) {
                if (!std::isfinite(candidate.heightsMeters[i]) || (!candidate.holes.empty() && candidate.holes[i] > 1))
                    return false;
                if (candidate.layerCount != 0) {
                    std::uint32_t sum = 0;
                    for (std::size_t layer = 0; layer < candidate.layerCount; ++layer)
                        sum += candidate.weights[i * candidate.layerCount + layer];
                    if (sum != 65'535U)
                        return false;
                }
            }
            return true;
        }
    }  // namespace

    /** @copydoc NormalizeTerrainSource */
    Result<TerrainCanonicalSource> NormalizeTerrainSource(const TerrainSourceImportRequest &request,
                                                          const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::Cancelled);
        if (!request.dataset.IsValid() || !request.sourceAsset.IsValid() || !request.revision.IsValid() || !request.capability.IsValid())
            return Failed<TerrainCanonicalSource>(TerrainErrors::IdentityInvalid);
        const std::uint32_t width = request.height.width;
        const std::uint32_t height = request.height.height;
        if (width < 2 || height < 2 || width > TerrainDescriptorHardLimits::SamplesPerAxis ||
            height > TerrainDescriptorHardLimits::SamplesPerAxis || width > request.limits.maximumSamplesPerAxis ||
            height > request.limits.maximumSamplesPerAxis)
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::InvalidDimensions);
        if (!ValidCoordinates(request.coordinates, width, height))
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::InvalidCoordinates);

        const std::uint64_t samples = static_cast<std::uint64_t>(width) * height;
        const std::uint64_t layers = request.weights.size();
        if (layers > TerrainDescriptorHardLimits::LayersPerTile || layers > request.limits.maximumLayers ||
            samples > TerrainDescriptorHardLimits::WorkItems || samples > request.limits.maximumSamples ||
            samples > request.limits.maximumWorkItems / (1U + layers + (request.holes ? 1U : 0U)) ||
            samples > TerrainDescriptorHardLimits::WorkItems / (1U + layers + (request.holes ? 1U : 0U)))
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::LimitExceeded);

        const std::uint64_t candidateBytes =
            samples * (sizeof(float) + layers * sizeof(std::uint16_t) + (request.holes ? sizeof(std::uint8_t) : 0U));
        const std::uint64_t stagingLimit = std::min(request.limits.maximumCanonicalBytes, TerrainDescriptorHardLimits::StagingBytes);
        if (candidateBytes > stagingLimit)
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::LimitExceeded);
        std::uint64_t decodeBudget = stagingLimit - candidateBytes;

        std::uint64_t originalBytes = request.height.bytes.size();
        if (originalBytes > TerrainDescriptorHardLimits::StagingBytes)
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::LimitExceeded);
        for (const TerrainRasterInput &weight : request.weights) {
            if (weight.bytes.size() > TerrainDescriptorHardLimits::StagingBytes - originalBytes)
                return Failed<TerrainCanonicalSource>(TerrainSourceErrors::LimitExceeded);
            originalBytes += weight.bytes.size();
        }
        if (request.holes) {
            if (request.holes->bytes.size() > TerrainDescriptorHardLimits::StagingBytes - originalBytes)
                return Failed<TerrainCanonicalSource>(TerrainSourceErrors::LimitExceeded);
            originalBytes += request.holes->bytes.size();
        }
        if (originalBytes > request.limits.maximumSourceBytes || originalBytes > TerrainDescriptorHardLimits::StagingBytes)
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::LimitExceeded);

        std::vector<std::vector<std::byte>> decodedStorage;
        decodedStorage.reserve(2 + request.weights.size());
        auto prepare = [&](TerrainRasterInput &raster) -> Result<void> {
            if (raster.format != TerrainRasterFormat::PngGray && raster.format != TerrainRasterFormat::External)
                return Result<void>::Success();
            if (raster.rowOrder == TerrainRowOrder::Count)
                return Failed<void>(TerrainSourceErrors::UnsupportedFormat);
            // PNG decoding temporarily owns a second pixel buffer inside stb; reserve both copies.
            const std::uint64_t maximumDecoded =
                std::min<std::uint64_t>(raster.format == TerrainRasterFormat::PngGray ? decodeBudget / 2U : decodeBudget,
                                        samples * sizeof(float));
            if (raster.format == TerrainRasterFormat::PngGray) {
                auto decoded = DecodePngGray(raster, maximumDecoded);
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                DecodedPngRaster value = std::move(decoded).Value();
                decodeBudget -= value.bytes.size();
                raster.format = value.format;
                raster.byteOrder = TerrainByteOrder::Little;
                decodedStorage.push_back(std::move(value.bytes));
            } else {
                if (raster.formatId.empty() || !request.decoder)
                    return Failed<void>(TerrainSourceErrors::UnsupportedFormat);
                try {
                    auto info = request.decoder->Probe(raster, cancellation);
                    if (info.HasError())
                        return Result<void>::Failure(info.ErrorValue());
                    const TerrainRasterDecodeInfo decoded = info.Value();
                    const std::uint64_t bytesPerSample = decoded.format == TerrainRasterFormat::RawU8    ? 1U
                                                         : decoded.format == TerrainRasterFormat::RawU16 ? 2U
                                                         : decoded.format == TerrainRasterFormat::RawF32 ? 4U
                                                                                                         : 0U;
                    if (bytesPerSample == 0 || decoded.byteOrder == TerrainByteOrder::Count)
                        return Failed<void>(TerrainSourceErrors::UnsupportedFormat);
                    if (decoded.decodedBytes != samples * bytesPerSample)
                        return Failed<void>(TerrainSourceErrors::InvalidBytes);
                    if (decoded.decodedBytes > maximumDecoded)
                        return Failed<void>(TerrainSourceErrors::LimitExceeded);
                    decodedStorage.emplace_back(static_cast<std::size_t>(decoded.decodedBytes));
                    if (auto decodedResult = request.decoder->DecodeInto(raster, decodedStorage.back(), cancellation);
                        decodedResult.HasError())
                        return decodedResult;
                    decodeBudget -= decoded.decodedBytes;
                    raster.format = decoded.format;
                    raster.byteOrder = decoded.byteOrder;
                } catch (...) {
                    return Failed<void>(TerrainSourceErrors::DecoderFailed);
                }
            }
            raster.bytes = decodedStorage.back();
            return Result<void>::Success();
        };
        TerrainRasterInput heightRaster = request.height;
        std::vector<TerrainRasterInput> weightRasters = request.weights;
        std::optional<TerrainRasterInput> holeRaster = request.holes;
        if (auto prepared = prepare(heightRaster); prepared.HasError())
            return Result<TerrainCanonicalSource>::Failure(prepared.ErrorValue());
        for (TerrainRasterInput &weight : weightRasters) {
            if (auto prepared = prepare(weight); prepared.HasError())
                return Result<TerrainCanonicalSource>::Failure(prepared.ErrorValue());
        }
        if (holeRaster) {
            if (auto prepared = prepare(*holeRaster); prepared.HasError())
                return Result<TerrainCanonicalSource>::Failure(prepared.ErrorValue());
        }
        if (cancellation.IsCancellationRequested())
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::Cancelled);

        auto heightBytes = ValidateRaster(heightRaster, width, height, false, true);
        if (heightBytes.HasError())
            return Result<TerrainCanonicalSource>::Failure(heightBytes.ErrorValue());
        std::uint64_t inputBytes = heightRaster.bytes.size();
        for (const TerrainRasterInput &weight : weightRasters) {
            auto valid = ValidateRaster(weight, width, height, true, false);
            if (valid.HasError())
                return Result<TerrainCanonicalSource>::Failure(valid.ErrorValue());
            inputBytes += weight.bytes.size();
        }
        if (holeRaster) {
            auto valid = ValidateRaster(*holeRaster, width, height, true, false);
            if (valid.HasError())
                return Result<TerrainCanonicalSource>::Failure(valid.ErrorValue());
            if (holeRaster->format != TerrainRasterFormat::RawU8)
                return Failed<TerrainCanonicalSource>(TerrainSourceErrors::UnsupportedFormat);
            inputBytes += holeRaster->bytes.size();
        }
        if (inputBytes > request.limits.maximumSourceBytes || inputBytes > TerrainDescriptorHardLimits::StagingBytes)
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::LimitExceeded);

        TerrainCanonicalSource candidate;
        candidate.dataset = request.dataset;
        candidate.sourceAsset = request.sourceAsset;
        candidate.revision = request.revision;
        candidate.capability = request.capability;
        candidate.width = width;
        candidate.height = height;
        candidate.coordinates = request.coordinates;
        candidate.layerCount = static_cast<std::uint8_t>(layers);
        candidate.heightsMeters.resize(static_cast<std::size_t>(samples));
        candidate.weights.resize(static_cast<std::size_t>(samples * layers));
        if (holeRaster)
            candidate.holes.resize(static_cast<std::size_t>(samples));

        std::array<std::uint32_t, TerrainDescriptorHardLimits::LayersPerTile> rawWeights{};
        std::array<std::uint64_t, TerrainDescriptorHardLimits::LayersPerTile> remainders{};
        for (std::uint32_t z = 0; z < height; ++z) {
            if (cancellation.IsCancellationRequested())
                return Failed<TerrainCanonicalSource>(TerrainSourceErrors::Cancelled);
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(z) * width + x;
                const std::uint32_t heightBits = ReadBits(heightRaster, SourceIndex(heightRaster, x, z), heightBytes.Value());
                const double rawHeight = heightRaster.format == TerrainRasterFormat::RawF32
                                             ? static_cast<double>(std::bit_cast<float>(heightBits))
                                             : static_cast<double>(heightBits);
                const double meters = rawHeight * request.coordinates.heightScale + request.coordinates.heightOffset;
                if (!std::isfinite(rawHeight) || !std::isfinite(meters) ||
                    std::abs(meters) > static_cast<double>(std::numeric_limits<float>::max()))
                    return Failed<TerrainCanonicalSource>(TerrainSourceErrors::InvalidSample);
                const float canonical = static_cast<float>(meters);
                if (std::abs(static_cast<double>(canonical) - meters) > request.coordinates.maximumPrecisionError)
                    return Failed<TerrainCanonicalSource>(TerrainSourceErrors::PrecisionLost);
                candidate.heightsMeters[index] = canonical;

                if (holeRaster) {
                    const std::uint32_t hole = ReadBits(*holeRaster, SourceIndex(*holeRaster, x, z), 1);
                    if (hole > 1)
                        return Failed<TerrainCanonicalSource>(TerrainSourceErrors::InvalidSample);
                    candidate.holes[index] = static_cast<std::uint8_t>(hole);
                }
                if (layers == 0)
                    continue;

                std::uint64_t sum = 0;
                for (std::size_t layer = 0; layer < layers; ++layer) {
                    const auto &raster = weightRasters[layer];
                    const std::uint32_t raw =
                        ReadBits(raster, SourceIndex(raster, x, z), raster.format == TerrainRasterFormat::RawU8 ? 1U : 2U);
                    rawWeights[layer] = raw;
                    sum += raw;
                }
                if (sum == 0)
                    return Failed<TerrainCanonicalSource>(TerrainSourceErrors::InvalidSample);
                std::uint32_t assigned = 0;
                for (std::size_t layer = 0; layer < layers; ++layer) {
                    const std::uint64_t numerator = static_cast<std::uint64_t>(rawWeights[layer]) * 65'535U;
                    candidate.weights[index * layers + layer] = static_cast<std::uint16_t>(numerator / sum);
                    remainders[layer] = numerator % sum;
                    assigned += candidate.weights[index * layers + layer];
                }
                while (assigned < 65'535U) {
                    const auto best = std::max_element(remainders.begin(), remainders.begin() + static_cast<std::ptrdiff_t>(layers));
                    const std::size_t layer = static_cast<std::size_t>(best - remainders.begin());
                    ++candidate.weights[index * layers + layer];
                    *best = 0;
                    ++assigned;
                }
            }
        }
        if (cancellation.IsCancellationRequested())
            return Failed<TerrainCanonicalSource>(TerrainSourceErrors::Cancelled);
        return Result<TerrainCanonicalSource>::Success(std::move(candidate));
    }

    /** @copydoc TerrainSourceDocument::Publish */
    Result<void> TerrainSourceDocument::Publish(TerrainCanonicalSource candidate,
                                                const std::optional<TerrainSourceRevision> expectedCurrentRevision) {
        if (closed_)
            return Failed<void>(TerrainSourceErrors::Closed);
        if (!ValidCandidate(candidate))
            return Failed<void>(TerrainSourceErrors::InvalidSample);
        if (source_) {
            if (!expectedCurrentRevision || *expectedCurrentRevision != source_->revision || candidate.dataset != source_->dataset ||
                candidate.sourceAsset != source_->sourceAsset || candidate.capability != source_->capability ||
                candidate.revision.Value() <= source_->revision.Value())
                return Failed<void>(TerrainSourceErrors::RevisionStale);
        } else if (expectedCurrentRevision) {
            return Failed<void>(TerrainSourceErrors::RevisionStale);
        }
        source_ = std::move(candidate);
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
