#include "Horo/Runtime/Render/ColorPipeline.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Rejects missing cook provenance digest fields. */
        bool DigestPresent(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](auto byte) {
                return byte != 0;
            });
        }

        /** @brief Checks finite ordered cubic LUT domains. */
        bool DomainValid(const Math::Vec3 &low, const Math::Vec3 &high) noexcept {
            const std::array lows{low.x, low.y, low.z};
            const std::array highs{high.x, high.y, high.z};
            for (std::size_t i = 0; i < lows.size(); ++i)
                if (!std::isfinite(lows[i]) || !std::isfinite(highs[i]) || lows[i] >= highs[i] || !std::isfinite(highs[i] - lows[i]))
                    return false;
            return true;
        }

        /** @brief Checks complete versioned cook provenance metadata. */
        bool IdentityValid(const ColorTransformDescriptor &d) noexcept {
            return d.schemaVersion == 2 && d.configurationRelease > 0 && DigestPresent(d.configurationDigest) &&
                   DigestPresent(d.payloadDigest) && DigestPresent(d.referenceDigest);
        }

        /** @brief Checks the closed LUT descriptor vocabulary and finite bounds. */
        bool DescriptorValid(const ColorTransformDescriptor &d) noexcept {
            const bool enums = d.role == ColorTransformRole::CreativeLook &&
                               static_cast<unsigned>(d.interpolation) <= static_cast<unsigned>(ColorLutInterpolation::Tetrahedral);
            return enums && d.domainPolicy == ColorLookDomainPolicy::RejectOutsideV1 && IdentityValid(d) && d.dimension >= 2 &&
                   d.dimension <= 65 && DomainValid(d.domainMinimum, d.domainMaximum);
        }

        /** @brief Hashes canonical little-endian IEEE754 sample bytes without native-endian dependence. */
        Sha256Digest PayloadDigest(std::span<const float> rgb) noexcept {
            static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
            Sha256Builder hash;
            for (float value : rgb) {
                const auto bits = std::bit_cast<std::uint32_t>(value);
                const std::array bytes{std::byte(bits & 255U), std::byte((bits >> 8U) & 255U), std::byte((bits >> 16U) & 255U),
                                       std::byte((bits >> 24U) & 255U)};
                if (!hash.Update(bytes))
                    std::terminate();
            }
            return hash.Finalize();
        }

        /** @brief Appends one exact IEEE754 value in canonical little-endian byte order. */
        void AppendFloat(std::vector<std::byte> &bytes, float value) {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(std::byte((bits >> shift) & 255U));
        }

        /** @brief Derives only storage padding; no color sample is converted, interpolated or approximated. */
        std::vector<std::byte> Atlas(std::span<const float> rgb) {
            std::vector<std::byte> bytes;
            bytes.reserve(rgb.size() / 3 * 16);
            for (std::size_t i = 0; i < rgb.size(); i += 3) {
                AppendFloat(bytes, rgb[i]);
                AppendFloat(bytes, rgb[i + 1]);
                AppendFloat(bytes, rgb[i + 2]);
                AppendFloat(bytes, 1.0F);
            }
            return bytes;
        }
    }  // namespace

    /** @copydoc CookedColorTransform::CookedColorTransform */
    CookedColorTransform::CookedColorTransform(ColorTransformDescriptor descriptor, std::vector<float> rgb)
        : descriptor_(std::move(descriptor)), rgb_(std::move(rgb)), atlas_(Atlas(rgb_)), atlasDigest_(ComputeSha256(atlas_)) {}

    /** @copydoc CookedColorTransform::Create */
    Result<std::shared_ptr<const CookedColorTransform>> CookedColorTransform::Create(ColorTransformDescriptor descriptor,
                                                                                     std::span<const float> rgb) {
        using Return = Result<std::shared_ptr<const CookedColorTransform>>;
        if (!DescriptorValid(descriptor))
            return Return::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
        if (const auto expected = static_cast<std::uint64_t>(descriptor.dimension) * descriptor.dimension * descriptor.dimension * 3U;
            rgb.size() != expected)
            return Return::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
        for (float value : rgb)
            if (!std::isfinite(value))
                return Return::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
        if (PayloadDigest(rgb) != descriptor.payloadDigest)
            return Return::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
        try {
            return Return::Success(std::shared_ptr<const CookedColorTransform>(
                new CookedColorTransform{std::move(descriptor), std::vector<float>{rgb.begin(), rgb.end()}}));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(ColorPipelineErrors::AllocationFailed));
        }
    }

    /** @copydoc CookedColorTransform::Descriptor */
    const ColorTransformDescriptor &CookedColorTransform::Descriptor() const noexcept {
        return descriptor_;
    }

    /** @copydoc CookedColorTransform::Rgb */
    std::span<const float> CookedColorTransform::Rgb() const noexcept {
        return rgb_;
    }
}  // namespace Horo::Render

namespace Horo::Render {
    /** @copydoc CookedColorTransform::AtlasDescriptor */
    RenderTextureDescriptor CookedColorTransform::AtlasDescriptor() const noexcept {
        const auto n = descriptor_.dimension;
        return {.extent = {n * n, n},
                .format = RenderTextureFormat::Rgba32Float,
                .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination};
    }

    /** @copydoc CookedColorTransform::AtlasUpload */
    std::span<const std::byte> CookedColorTransform::AtlasUpload() const noexcept {
        return atlas_;
    }

    /** @copydoc CookedColorTransform::AtlasDigest */
    Sha256Digest CookedColorTransform::AtlasDigest() const noexcept {
        return atlasDigest_;
    }

    /** @copydoc CookedColorTransform::PayloadBytes */
    std::uint64_t CookedColorTransform::PayloadBytes() const noexcept {
        return rgb_.size() * sizeof(float) + atlas_.size();
    }
}  // namespace Horo::Render
