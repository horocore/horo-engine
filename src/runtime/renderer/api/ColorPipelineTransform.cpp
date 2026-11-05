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
                if (!std::isfinite(lows[i]) || !std::isfinite(highs[i]) || lows[i] >= highs[i])
                    return false;
            return true;
        }

        /** @brief Checks bounded target luminance metadata. */
        bool LuminanceValid(const ColorTransformDescriptor &d) noexcept {
            const std::array values{d.referenceWhiteNits, d.peakNits, d.blackNits};
            for (float value : values)
                if (!std::isfinite(value) || value < 0 || value > 10'000)
                    return false;
            return d.referenceWhiteNits > 0 && d.blackNits < d.referenceWhiteNits && d.referenceWhiteNits <= d.peakNits;
        }

        /** @brief Checks complete versioned cook provenance metadata. */
        bool IdentityValid(const ColorTransformDescriptor &d) noexcept {
            return d.schemaVersion == 1 && d.configurationRelease > 0 && DigestPresent(d.configurationDigest) &&
                   DigestPresent(d.payloadDigest) && DigestPresent(d.referenceDigest);
        }

        /** @brief Checks the closed LUT descriptor vocabulary and finite bounds. */
        bool DescriptorValid(const ColorTransformDescriptor &d) noexcept {
            const bool enums = static_cast<unsigned>(d.role) <= static_cast<unsigned>(ColorTransformRole::NeutralDiagnostic) &&
                               static_cast<unsigned>(d.interpolation) <= static_cast<unsigned>(ColorLutInterpolation::Tetrahedral);
            return enums && IdentityValid(d) && d.dimension >= 2 && d.dimension <= 65 && DomainValid(d.domainMinimum, d.domainMaximum) &&
                   d.output.IsValid() && LuminanceValid(d);
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
    }  // namespace

    /** @copydoc CookedColorTransform::CookedColorTransform */
    CookedColorTransform::CookedColorTransform(ColorTransformDescriptor descriptor, std::vector<float> rgb)
        : descriptor_(std::move(descriptor)), rgb_(std::move(rgb)) {}

    /** @copydoc CookedColorTransform::Create */
    Result<std::shared_ptr<const CookedColorTransform>> CookedColorTransform::Create(ColorTransformDescriptor descriptor,
                                                                                     std::span<const float> rgb) {
        using Return = Result<std::shared_ptr<const CookedColorTransform>>;
        if (!DescriptorValid(descriptor))
            return Return::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
        const auto expected = static_cast<std::uint64_t>(descriptor.dimension) * descriptor.dimension * descriptor.dimension * 3U;
        if (rgb.size() != expected)
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
