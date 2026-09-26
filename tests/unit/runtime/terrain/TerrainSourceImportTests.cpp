#include "Horo/Terrain/TerrainSourceImport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Horo::Terrain {
    namespace {
        TerrainDatasetId Dataset() {
            SerializedTerrainIdentity projectBytes{};
            projectBytes[0] = 7;
            const auto project = TerrainProjectId::Create(projectBytes).Value();
            constexpr std::string_view key = "terrain/import-test";
            return DeriveTerrainDatasetId(project, std::as_bytes(std::span{key.data(), key.size()})).Value();
        }

        std::vector<std::byte> Bytes(const std::initializer_list<std::uint8_t> values) {
            std::vector<std::byte> result;
            for (const auto value : values)
                result.push_back(static_cast<std::byte>(value));
            return result;
        }

        std::vector<std::byte> Hex(const std::string_view hex) {
            const auto digit = [](const char c) -> std::uint8_t {
                return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
            };
            std::vector<std::byte> bytes;
            for (std::size_t i = 0; i < hex.size(); i += 2)
                bytes.push_back(static_cast<std::byte>((digit(hex[i]) << 4U) | digit(hex[i + 1])));
            return bytes;
        }

        TerrainRasterInput Raster(const std::vector<std::byte> &bytes, const TerrainRasterFormat format) {
            return {.format = format, .bytes = bytes, .width = 2, .height = 2};
        }

        TerrainSourceImportRequest Request(const std::vector<std::byte> &height, const TerrainRasterFormat format,
                                           const std::uint64_t revision = 1) {
            TerrainSourceImportRequest request;
            request.dataset = Dataset();
            std::array<std::uint8_t, 16> assetBytes{};
            assetBytes[0] = 1;
            request.sourceAsset = Assets::AssetId::FromBytes(assetBytes);
            request.revision = TerrainSourceRevision::Create(revision).Value();
            request.capability = TerrainCapabilityRevision::Create(1).Value();
            request.height = Raster(height, format);
            return request;
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == code.domain.Value());
            CHECK(result.ErrorValue().code.Value() == code.code.Value());
        }

        class ConstantDecoder final : public ITerrainRasterDecoder {
        public:
            [[nodiscard]] Result<TerrainRasterDecodeInfo> Probe(const TerrainRasterInput &source,
                                                                const CancellationToken &cancellation) const override {
                if (source.formatId != "test.constant-u16" || cancellation.IsCancellationRequested())
                    return Result<TerrainRasterDecodeInfo>::Failure(MakeError(TerrainSourceErrors::UnsupportedFormat));
                return Result<TerrainRasterDecodeInfo>::Success({.format = TerrainRasterFormat::RawU16, .decodedBytes = 8});
            }

            [[nodiscard]] Result<void> DecodeInto(const TerrainRasterInput &, const std::span<std::byte> output,
                                                  const CancellationToken &cancellation) const override {
                if (output.size() != 8 || cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainSourceErrors::Cancelled));
                const auto values = Bytes({1, 0, 2, 0, 3, 0, 4, 0});
                std::copy(values.begin(), values.end(), output.begin());
                return Result<void>::Success();
            }
        };

        class InvalidProbeDecoder final : public ITerrainRasterDecoder {
        public:
            [[nodiscard]] Result<TerrainRasterDecodeInfo> Probe(const TerrainRasterInput &, const CancellationToken &) const override {
                return Result<TerrainRasterDecodeInfo>::Success({.format = TerrainRasterFormat::RawU16, .decodedBytes = 16});
            }

            [[nodiscard]] Result<void> DecodeInto(const TerrainRasterInput &, std::span<std::byte>,
                                                  const CancellationToken &) const override {
                FAIL("Invalid probe output must be rejected before decoder allocation or invocation");
                return Result<void>::Success();
            }
        };

        class ThrowingDecoder final : public ITerrainRasterDecoder {
        public:
            [[nodiscard]] Result<TerrainRasterDecodeInfo> Probe(const TerrainRasterInput &, const CancellationToken &) const override {
                throw std::runtime_error("decoder failure");
            }

            [[nodiscard]] Result<void> DecodeInto(const TerrainRasterInput &, std::span<std::byte>,
                                                  const CancellationToken &) const override {
                return Result<void>::Success();
            }
        };
    }  // namespace

    TEST_CASE("Raw height, weights and holes normalize into one canonical increasing-Z grid", "[terrain][import]") {
        const auto height = Bytes({1, 0, 2, 0, 3, 0, 4, 0});
        const auto first = Bytes({1, 0, 4, 2});
        const auto second = Bytes({1, 4, 0, 2});
        const auto holes = Bytes({0, 1, 0, 1});
        auto request = Request(height, TerrainRasterFormat::RawU16);
        request.height.rowOrder = TerrainRowOrder::DecreasingZ;
        request.weights = {Raster(first, TerrainRasterFormat::RawU8), Raster(second, TerrainRasterFormat::RawU8)};
        request.holes = Raster(holes, TerrainRasterFormat::RawU8);
        request.coordinates.heightScale = 0.5;
        request.coordinates.heightOffset = -1;
        request.coordinates.space = TerrainCoordinateSpace::ProjectedMeters;
        request.coordinates.projectedCrs = "EPSG:32632";
        const auto result = NormalizeTerrainSource(request, {});
        REQUIRE(result.HasValue());
        const auto &source = result.Value();
        CHECK(source.dataset == request.dataset);
        CHECK(source.sourceAsset == request.sourceAsset);
        CHECK(source.revision == request.revision);
        CHECK(source.capability == request.capability);
        CHECK(source.heightsMeters == std::vector<float>{0.5F, 1.0F, -0.5F, 0.0F});
        CHECK(source.holes == std::vector<std::uint8_t>{0, 1, 0, 1});
        CHECK(source.weights[0] == 32'768);
        CHECK(source.weights[1] == 32'767);
        for (std::size_t i = 0; i < 4; ++i)
            CHECK(static_cast<std::uint32_t>(source.weights[2 * i]) + source.weights[2 * i + 1] == 65'535);
        CHECK(source.coordinates.projectedCrs == "EPSG:32632");
    }

    TEST_CASE("Existing PNG decoder imports grayscale 16-bit height and 8-bit weight rasters", "[terrain][import]") {
        const auto height = Hex("89504e470d0a1a0a0000000d4948445200000002000000021000000000074d8ebb0000001249444154789c63606064606260606660"
                                "0100002b000b63bf1b1a0000000049454e44ae426082");
        const auto weight = Hex("89504e470d0a1a0a0000000d494844520000000200000002080000000057dd52f80000000e49444154789c6360646260660100001d"
                                "000b0db552060000000049454e44ae426082");
        auto request = Request(height, TerrainRasterFormat::PngGray);
        request.weights.push_back(Raster(weight, TerrainRasterFormat::PngGray));
        const auto result = NormalizeTerrainSource(request, {});
        REQUIRE(result.HasValue());
        CHECK(result.Value().heightsMeters == std::vector<float>{1, 2, 3, 4});
        CHECK(result.Value().weights == std::vector<std::uint16_t>{65'535, 65'535, 65'535, 65'535});

        request.limits.maximumCanonicalBytes = 35;
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::LimitExceeded);
    }

    TEST_CASE("Unsupported PNG channels, external formats and roles return typed errors", "[terrain][import]") {
        const auto rawHeight = Bytes({1, 0, 2, 0, 3, 0, 4, 0});
        auto request = Request(rawHeight, TerrainRasterFormat::RawU16);
        request.height.format = TerrainRasterFormat::Count;
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::UnsupportedFormat);

        request.height.format = TerrainRasterFormat::RawU8;
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::UnsupportedFormat);

        request.height.format = TerrainRasterFormat::External;
        request.height.formatId = "vendor.height";
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::UnsupportedFormat);
        request.decoder = std::make_shared<ConstantDecoder>();
        request.height.formatId = "test.constant-u16";
        CHECK(NormalizeTerrainSource(request, {}).HasValue());
        request.decoder = std::make_shared<InvalidProbeDecoder>();
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::InvalidBytes);
        request.decoder = std::make_shared<ThrowingDecoder>();
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::DecoderFailed);

        const auto png = Hex("89504e470d0a1a0a0000000d4948445200000002000000021000000000074d8ebb0000001249444154789c63606064606260606660010"
                             "0002b000b63bf1b1a0000000049454e44ae426082");
        request = Request(png, TerrainRasterFormat::PngGray);
        auto colorPng = png;
        colorPng[25] = std::byte{2};
        request.height = Raster(colorPng, TerrainRasterFormat::PngGray);
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::UnsupportedFormat);

        request.height = Raster(png, TerrainRasterFormat::PngGray);
        request.holes = Raster(png, TerrainRasterFormat::PngGray);
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::UnsupportedFormat);
    }

    TEST_CASE("Raw float32 byte order is explicit and non-finite samples are rejected", "[terrain][import]") {
        const auto bigEndian = Bytes({0x3f, 0xc0, 0, 0, 0x40, 0, 0, 0, 0x40, 0x40, 0, 0, 0x40, 0x80, 0, 0});
        auto request = Request(bigEndian, TerrainRasterFormat::RawF32);
        request.height.byteOrder = TerrainByteOrder::Big;
        const auto normalized = NormalizeTerrainSource(request, {});
        REQUIRE(normalized.HasValue());
        CHECK(normalized.Value().heightsMeters == std::vector<float>{1.5F, 2.0F, 3.0F, 4.0F});

        const auto nanHeight = Bytes({0, 0, 0xc0, 0x7f, 0, 0, 0, 0x40, 0, 0, 0x40, 0x40, 0, 0, 0x80, 0x40});
        request = Request(nanHeight, TerrainRasterFormat::RawF32);
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::InvalidSample);
    }

    TEST_CASE("Invalid dimensions, bytes, sample precision and coordinate metadata fail before publication", "[terrain][import]") {
        auto height = Bytes({1, 0, 2, 0, 3, 0, 4, 0});
        auto request = Request(height, TerrainRasterFormat::RawU16);
        request.height.width = 3;
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::InvalidBytes);
        request = Request(height, TerrainRasterFormat::RawU16);
        request.limits.maximumSamples = 3;
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::LimitExceeded);
        request = Request(height, TerrainRasterFormat::RawU16);
        request.coordinates.space = TerrainCoordinateSpace::GeographicDegrees;
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::InvalidCoordinates);
        request = Request(height, TerrainRasterFormat::RawU16);
        request.coordinates.originX = 1e20;
        request.coordinates.maximumPrecisionError = 0;
        request.coordinates.heightOffset = 1e20;
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::PrecisionLost);
        request = Request(height, TerrainRasterFormat::RawU16);
        const auto invalidHoles = Bytes({0, 1, 2, 0});
        request.holes = Raster(invalidHoles, TerrainRasterFormat::RawU8);
        RequireError(NormalizeTerrainSource(request, {}), TerrainSourceErrors::InvalidSample);
    }

    TEST_CASE("Cancellation and exact revision fencing preserve the current source through failure and shutdown", "[terrain][import]") {
        const auto height = Bytes({1, 0, 2, 0, 3, 0, 4, 0});
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(NormalizeTerrainSource(Request(height, TerrainRasterFormat::RawU16), cancellation.Token()),
                     TerrainSourceErrors::Cancelled);

        TerrainSourceDocument document;
        auto malformed = NormalizeTerrainSource(Request(height, TerrainRasterFormat::RawU16), {}).Value();
        malformed.heightsMeters[0] = std::numeric_limits<float>::infinity();
        RequireError(document.Publish(std::move(malformed), std::nullopt), TerrainSourceErrors::InvalidSample);
        CHECK(document.Current() == nullptr);
        auto first = NormalizeTerrainSource(Request(height, TerrainRasterFormat::RawU16), {}).Value();
        REQUIRE(document.Publish(std::move(first), std::nullopt).HasValue());
        auto second = NormalizeTerrainSource(Request(height, TerrainRasterFormat::RawU16, 2), {}).Value();
        RequireError(document.Publish(second, TerrainSourceRevision::Create(2).Value()), TerrainSourceErrors::RevisionStale);
        REQUIRE(document.Current() != nullptr);
        CHECK(document.Current()->revision.Value() == 1);
        REQUIRE(document.Publish(std::move(second), TerrainSourceRevision::Create(1).Value()).HasValue());
        CHECK(document.Current()->revision.Value() == 2);
        document.Close();
        auto third = NormalizeTerrainSource(Request(height, TerrainRasterFormat::RawU16, 3), {}).Value();
        RequireError(document.Publish(std::move(third), TerrainSourceRevision::Create(2).Value()), TerrainSourceErrors::Closed);
        CHECK(document.Current()->revision.Value() == 2);
    }
}  // namespace Horo::Terrain
