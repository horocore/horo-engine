#include "Horo/Runtime/Render/ColorOutputProgram.h"

#include "Horo/Runtime/Render/ColorPipeline.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr Sha256Digest ConfigurationDigest{{0x9e, 0x3e, 0xc7, 0x73, 0xa7, 0xfb, 0xc0, 0xbb, 0x66, 0x66, 0xe4,
                                                    0x28, 0xde, 0xdd, 0x8c, 0xce, 0xc4, 0xf5, 0x9b, 0x8b, 0x6d, 0x40,
                                                    0x50, 0x6c, 0x82, 0x5c, 0xc7, 0xa8, 0xc0, 0x2f, 0xf2, 0xff}};
        constexpr Sha256Digest OptionsDigest{{0x5b, 0xbe, 0xb4, 0xa9, 0x2e, 0x31, 0x19, 0xc0, 0x05, 0x77, 0x9d,
                                              0xe9, 0x78, 0xed, 0x58, 0x26, 0xa0, 0xb2, 0x37, 0x74, 0x18, 0x4a,
                                              0x27, 0x3f, 0x9b, 0x6f, 0xe2, 0x59, 0xad, 0x4d, 0x27, 0x72}};

        /** @brief Rejects absent provenance identity without treating a digest as authentication. */
        bool Present(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](auto value) {
                return value != 0;
            });
        }

        /** @brief Checks the one declared official processor/configuration version vocabulary. */
        bool ProgramIdentityValid(const ColorOutputProgramDescriptor &d) noexcept {
            return d.schemaVersion == 1 && d.profile == ColorOutputProfile::Aces2Sdr100Rec709LinearV1 && d.configurationRelease == 40000 &&
                   d.ocioVersion == 20500 && d.configurationDigest == ConfigurationDigest && d.optionsDigest == OptionsDigest &&
                   Present(d.sourceDigest) && Present(d.referenceDigest);
        }

        /** @brief Requires exact nearest-only filtering and clamp addressing for the two official tables. */
        bool SamplerShapeValid(const RenderSamplerDescriptor &s) noexcept {
            const bool filters = s.minFilter == RenderSamplerFilter::Nearest && s.magFilter == RenderSamplerFilter::Nearest &&
                                 s.mipmapMode == RenderSamplerMipmapMode::Nearest;
            const bool addresses = s.addressU == RenderSamplerAddressMode::ClampToEdge &&
                                   s.addressV == RenderSamplerAddressMode::ClampToEdge &&
                                   s.addressW == RenderSamplerAddressMode::ClampToEdge;
            return filters && addresses;
        }

        /** @brief Checks all remaining fixed sampler fields, including finite zero LOD. */
        bool SamplerValid(const RenderSamplerDescriptor &s) noexcept {
            return SamplerShapeValid(s) && s.minimumLod == 0 && s.maximumLod == 0 && s.maximumAnisotropy == 1 && !s.compareEnabled &&
                   s.compare == RenderCompareFunction::Always;
        }

        /** @brief Checks exact producer table/sampler symbol names without scanning generated shader text. */
        bool NamesValid(const ColorOutputTableDescriptor &d, std::size_t index) noexcept {
            constexpr std::array textures{"horo_color_reach_m_table_0", "horo_color_gamut_cusp_table_0"};
            constexpr std::array samplers{"horo_color_reach_m_table_0Sampler", "horo_color_gamut_cusp_table_0Sampler"};
            return d.textureName == textures[index] && d.samplerName == samplers[index];
        }

        /** @brief Validates the exact ordered 362×1 R32/RGBA32 auxiliary table layouts. */
        bool TableDescriptorValid(const ColorOutputTableDescriptor &d, std::size_t index) noexcept {
            const auto format = index == 0 ? RenderTextureFormat::R32Float : RenderTextureFormat::Rgba32Float;
            const std::uint32_t channels = index == 0 ? 1U : 3U;
            return d.ordinal == index && d.extent == FramebufferExtent{362, 1} && d.format == format && d.sourceChannels == channels &&
                   NamesValid(d, index) && SamplerValid(d.sampler);
        }

        /** @brief Decodes one bounds-checked little-endian IEEE754 word without native-endian assumptions. */
        std::uint32_t Word(std::span<const std::byte> data, std::size_t offset) noexcept {
            std::uint32_t value{};
            for (std::size_t byte = 0; byte < 4; ++byte)
                value |= std::to_integer<std::uint32_t>(data[offset + byte]) << (byte * 8U);
            return value;
        }

        /** @brief Rejects non-finite original or upload samples while preserving every finite signed bit pattern. */
        bool Finite(std::span<const std::byte> data) noexcept {
            static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
            for (std::size_t offset = 0; offset < data.size(); offset += 4)
                if (!std::isfinite(std::bit_cast<float>(Word(data, offset))))
                    return false;
            return true;
        }

        /** @brief Checks exact RGB preservation and IEEE754 alpha-one padding for the native RGBA table. */
        bool PaddingValid(const ColorOutputTableInput &t, std::size_t index) noexcept {
            if (index == 0)
                return std::ranges::equal(t.source, t.upload);
            for (std::size_t texel = 0; texel < 362; ++texel) {
                if (!std::ranges::equal(t.source.subspan(texel * 12, 12), t.upload.subspan(texel * 16, 12)))
                    return false;
                if (Word(t.upload, texel * 16 + 12) != 0x3f800000U)
                    return false;
            }
            return true;
        }

        /** @brief Checks exact payload counts before bounded decoding, then validates both portable hashes. */
        bool PayloadValid(const ColorOutputTableInput &t, std::size_t index) noexcept {
            const std::size_t sourceBytes = 362U * (index == 0 ? 1U : 3U) * 4U;
            const std::size_t uploadBytes = 362U * (index == 0 ? 1U : 4U) * 4U;
            if (t.source.size() != sourceBytes || t.upload.size() != uploadBytes)
                return false;
            return Finite(t.source) && Finite(t.upload) && PaddingValid(t, index) && ComputeSha256(t.source) == t.descriptor.sourceDigest &&
                   ComputeSha256(t.upload) == t.descriptor.uploadDigest;
        }

        /** @brief Validates the complete inert source/table generation without asserting HLSL semantics or cook trust. */
        bool InputsValid(const ColorOutputProgramDescriptor &d, std::string_view source,
                         std::span<const ColorOutputTableInput> tables) noexcept {
            if (!ProgramIdentityValid(d) || source.empty() || source.size() > CookedColorOutputProgram::MaximumSourceBytes ||
                tables.size() != 2)
                return false;
            if (ComputeSha256(std::as_bytes(std::span{source.data(), source.size()})) != d.sourceDigest)
                return false;
            for (std::size_t index = 0; index < tables.size(); ++index)
                if (!TableDescriptorValid(tables[index].descriptor, index) || !PayloadValid(tables[index], index))
                    return false;
            return true;
        }
    }  // namespace

    /** @copydoc CookedColorOutputProgram::CookedColorOutputProgram */
    CookedColorOutputProgram::CookedColorOutputProgram(ColorOutputProgramDescriptor descriptor, std::string_view source,
                                                       std::span<const ColorOutputTableInput> tables)
        : descriptor_(std::move(descriptor)), source_(source) {
        for (std::size_t index = 0; index < TableCount; ++index) {
            tables_[index] = tables[index].descriptor;
            original_[index].assign(tables[index].source.begin(), tables[index].source.end());
            upload_[index].assign(tables[index].upload.begin(), tables[index].upload.end());
        }
    }

    /** @copydoc CookedColorOutputProgram::Create */
    Result<std::shared_ptr<const CookedColorOutputProgram>> CookedColorOutputProgram::Create(
        ColorOutputProgramDescriptor descriptor, std::string_view source, std::span<const ColorOutputTableInput> tables) {
        using Return = Result<std::shared_ptr<const CookedColorOutputProgram>>;
        if (!InputsValid(descriptor, source, tables))
            return Return::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
        try {
            return Return::Success(
                std::shared_ptr<const CookedColorOutputProgram>(new CookedColorOutputProgram{std::move(descriptor), source, tables}));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(ColorPipelineErrors::AllocationFailed));
        }
    }

    /** @copydoc CookedColorOutputProgram::Descriptor */
    const ColorOutputProgramDescriptor &CookedColorOutputProgram::Descriptor() const noexcept {
        return descriptor_;
    }

    /** @copydoc CookedColorOutputProgram::Source */
    std::string_view CookedColorOutputProgram::Source() const noexcept {
        return source_;
    }

    /** @copydoc CookedColorOutputProgram::Table */
    const ColorOutputTableDescriptor *CookedColorOutputProgram::Table(std::size_t ordinal) const noexcept {
        return ordinal < TableCount ? &tables_[ordinal] : nullptr;
    }

    /** @copydoc CookedColorOutputProgram::TableSource */
    std::span<const std::byte> CookedColorOutputProgram::TableSource(std::size_t ordinal) const noexcept {
        return ordinal < TableCount ? std::span<const std::byte>{original_[ordinal]} : std::span<const std::byte>{};
    }

    /** @copydoc CookedColorOutputProgram::TableUpload */
    std::span<const std::byte> CookedColorOutputProgram::TableUpload(std::size_t ordinal) const noexcept {
        return ordinal < TableCount ? std::span<const std::byte>{upload_[ordinal]} : std::span<const std::byte>{};
    }

    /** @copydoc CookedColorOutputProgram::PayloadBytes */
    std::uint64_t CookedColorOutputProgram::PayloadBytes() const noexcept {
        std::uint64_t total = source_.size();
        for (std::size_t index = 0; index < TableCount; ++index)
            total += original_[index].size() + upload_[index].size();
        return total;
    }
}  // namespace Horo::Render
