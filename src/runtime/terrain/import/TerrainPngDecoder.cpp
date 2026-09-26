#include "TerrainPngDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace Horo::Terrain::ImportDetail {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &code, const std::string_view detail = {}) {
            return Result<T>::Failure(MakeError(code, std::string(detail)));
        }

        [[nodiscard]] std::uint32_t ReadBig32(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
            return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) | (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16U) |
                   (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8U) | std::to_integer<std::uint32_t>(bytes[offset + 3]);
        }

        /** @brief Verifies the PNG envelope and exact grayscale shape before stb can allocate pixels. */
        [[nodiscard]] Result<std::uint8_t> ValidatePngHeader(const TerrainRasterInput &source, const std::uint64_t maximumDecodedBytes) {
            static constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
            if (source.bytes.size() < 33 || source.bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return Failed<std::uint8_t>(TerrainSourceErrors::InvalidBytes);
            for (std::size_t i = 0; i < signature.size(); ++i) {
                if (std::to_integer<std::uint8_t>(source.bytes[i]) != signature[i])
                    return Failed<std::uint8_t>(TerrainSourceErrors::InvalidBytes);
            }
            if (ReadBig32(source.bytes, 8) != 13 || source.bytes[12] != std::byte{'I'} || source.bytes[13] != std::byte{'H'} ||
                source.bytes[14] != std::byte{'D'} || source.bytes[15] != std::byte{'R'})
                return Failed<std::uint8_t>(TerrainSourceErrors::InvalidBytes);
            const std::uint32_t width = ReadBig32(source.bytes, 16);
            const std::uint32_t height = ReadBig32(source.bytes, 20);
            const std::uint8_t bitDepth = std::to_integer<std::uint8_t>(source.bytes[24]);
            if (const auto colorType = std::to_integer<std::uint8_t>(source.bytes[25]);
                colorType != 0 || (bitDepth != 8 && bitDepth != 16) || source.bytes[26] != std::byte{0} ||
                source.bytes[27] != std::byte{0} || source.bytes[28] != std::byte{0})
                return Failed<std::uint8_t>(TerrainSourceErrors::UnsupportedFormat,
                                            "Only non-interlaced grayscale PNG with 8 or 16-bit samples is supported.");
            if (width != source.width || height != source.height)
                return Failed<std::uint8_t>(TerrainSourceErrors::InvalidDimensions);
            if (const auto decodedBytes = static_cast<std::uint64_t>(width) * height * (bitDepth / 8U); decodedBytes > maximumDecodedBytes)
                return Failed<std::uint8_t>(TerrainSourceErrors::LimitExceeded);
            return Result<std::uint8_t>::Success(bitDepth);
        }

        /** @brief Copies decoded byte grayscale pixels into operation-owned canonical bytes. */
        [[nodiscard]] bool DecodePng8(const TerrainRasterInput &source, DecodedPngRaster &result, int &width, int &height,
                                      int &components) {
            // NOSONAR(cpp:S6022) stb_image requires unsigned-byte input; the source remains owned as std::byte.
            std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(
                                                                                                  source.bytes.data()),
                                                                                              static_cast<int>(source.bytes.size()), &width,
                                                                                              &height, &components, 1),
                                                                        stbi_image_free};
            if (pixels == nullptr)
                return false;
            if (width != static_cast<int>(source.width) || height != static_cast<int>(source.height) || components != 1)
                return false;
            std::copy_n(reinterpret_cast<const std::byte *>(pixels.get()), result.bytes.size(), result.bytes.begin());
            return true;
        }

        /** @brief Converts decoded host-order 16-bit PNG samples to canonical little-endian bytes. */
        [[nodiscard]] bool DecodePng16(const TerrainRasterInput &source, DecodedPngRaster &result, int &width, int &height,
                                       int &components) {
            // NOSONAR(cpp:S6022) stb_image requires unsigned-byte input; the source remains owned as std::byte.
            std::unique_ptr<stbi_us, decltype(&stbi_image_free)> pixels{stbi_load_16_from_memory(reinterpret_cast<const stbi_uc *>(
                                                                                                     source.bytes.data()),
                                                                                                 static_cast<int>(source.bytes.size()),
                                                                                                 &width, &height, &components, 1),
                                                                        stbi_image_free};
            if (pixels == nullptr)
                return false;
            if (width != static_cast<int>(source.width) || height != static_cast<int>(source.height) || components != 1 ||
                result.bytes.size() % 2 != 0)
                return false;
            for (std::size_t i = 0; i < result.bytes.size() / 2; ++i) {
                result.bytes[2 * i] = static_cast<std::byte>(pixels.get()[i] & 0xFFU);
                result.bytes[2 * i + 1] = static_cast<std::byte>(pixels.get()[i] >> 8U);
            }
            return true;
        }
    }  // namespace

    /** @copydoc DecodePngGray */
    Result<DecodedPngRaster> DecodePngGray(const TerrainRasterInput &source, const std::uint64_t maximumDecodedBytes) {
        auto bitDepth = ValidatePngHeader(source, maximumDecodedBytes);
        if (bitDepth.HasError())
            return Result<DecodedPngRaster>::Failure(bitDepth.ErrorValue());
        DecodedPngRaster result;
        result.format = bitDepth.Value() == 8 ? TerrainRasterFormat::RawU8 : TerrainRasterFormat::RawU16;
        result.bytes.resize(static_cast<std::size_t>(static_cast<std::uint64_t>(source.width) * source.height * (bitDepth.Value() / 8U)));
        int decodedWidth = 0;
        int decodedHeight = 0;
        int components = 0;
        if (const bool decoded = bitDepth.Value() == 8 ? DecodePng8(source, result, decodedWidth, decodedHeight, components)
                                                       : DecodePng16(source, result, decodedWidth, decodedHeight, components);
            !decoded || decodedWidth != static_cast<int>(source.width) || decodedHeight != static_cast<int>(source.height) ||
            components != 1)
            return Failed<DecodedPngRaster>(TerrainSourceErrors::InvalidBytes);
        return Result<DecodedPngRaster>::Success(std::move(result));
    }
}  // namespace Horo::Terrain::ImportDetail
