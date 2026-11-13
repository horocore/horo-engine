#pragma once

/** @file ColorOutputProgram.h
 * @brief Immutable cold analytic output source and original auxiliary tables; no cook trust or native execution authority.
 */
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/RenderResourceDescriptors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Render {
    /** @brief Initial exact generated OCIO route; other output profiles require separate explicit implementations. */
    enum class ColorOutputProfile : std::uint8_t {
        Aces2Sdr100Rec709LinearV1
    };

    /** @brief Pins the exact offline source/options/reference generation without asserting authenticity or ACES conformance. */
    struct ColorOutputProgramDescriptor final {
        std::uint32_t schemaVersion{1};
        ColorOutputProfile profile{ColorOutputProfile::Aces2Sdr100Rec709LinearV1};
        std::uint32_t configurationRelease{}, ocioVersion{}; /**< Required40000(CG4.0.0) and20500(OCIO2.5.0). */
        Sha256Digest configurationDigest;                    /**< Exact CG4 configuration SHA9e3ec773...c02ff2ff, pinned by profileV1. */
        Sha256Digest optionsDigest;                 /**< Exact canonical producer options SHA5bbeb4a9...ad4d2772, pinned by profileV1. */
        Sha256Digest sourceDigest, referenceDigest; /**< Exact generated-source integrity and independently pinned CPU corpus identity. */
    };

    /** @brief Exact2D auxiliary descriptor. Samples are original little-endian IEEE754 float32, never a baked3D output LUT. */
    struct ColorOutputTableDescriptor final {
        std::uint32_t ordinal{};
        std::string textureName, samplerName;
        FramebufferExtent extent;
        std::uint32_t sourceChannels{};
        RenderTextureFormat format{RenderTextureFormat::R32Float};
        RenderSamplerDescriptor sampler; /**< Exactly nearest/clamp, LOD0, anisotropy1, no comparison. */
        Sha256Digest sourceDigest, uploadDigest;
    };

    /** @brief Borrowed cold factory input; Create copies descriptor and both exact payloads before returning. */
    struct ColorOutputTableInput final {
        ColorOutputTableDescriptor descriptor;
        std::span<const std::byte> source, upload;
    };

    /** @brief Owns the exact HoroColorOutput analytic processor and its two immutable2D auxiliary tables.
     * ProfileV1 is exposed ACEScg→official ACES2 SDR100 Rec709→display-linear Rec709, with no added shaper/range clamp.
     * HLSL source is opaque here: hashes/counts/finite tables establish integrity only. The authenticated shader cooker must
     * compile this exact source with its wrapper, validate reflection/native ABI and establish actual provenance separately.
     * No regex, CPU evaluation, baked3D approximation, file discovery, residency or GPU completion is provided by this value.
     */
    class CookedColorOutputProgram final {
    public:
        /** @brief Hard generation-source byte ceiling; no unbounded shader text is retained. */
        static constexpr std::size_t MaximumSourceBytes = 128U * 1024U;
        /** @brief Exact number of original analytic auxiliary2D tables in the closed profile. */
        static constexpr std::size_t TableCount = 2;
        /** @brief Copies one complete bounded analytic source/table generation after integrity validation.
         * @param descriptor Exact closed profile and pinned digests. @param source Original opaque generated HLSL bytes.
         * @param tables Exactly two ordered original/upload tables from the producer.
         * @return Immutable owned generation or typed invalid-transform/allocation failure; grants no trust/native support.
         */
        [[nodiscard]] static Result<std::shared_ptr<const CookedColorOutputProgram>> Create(ColorOutputProgramDescriptor descriptor,
                                                                                            std::string_view source,
                                                                                            std::span<const ColorOutputTableInput> tables);
        /** @brief Borrows exact immutable program identity. @return Descriptor for this object lifetime. */
        [[nodiscard]] const ColorOutputProgramDescriptor &Descriptor() const noexcept;
        /** @brief Borrows original generated HLSL. @return Exact bytes for this object lifetime. */
        [[nodiscard]] std::string_view Source() const noexcept;

        /** @brief Identifies the closed processor function. @return HoroColorOutput; source semantics are validated by the shader cooker.
         */
        [[nodiscard]] static constexpr std::string_view FunctionName() noexcept {
            return "HoroColorOutput";
        }

        /** @brief Borrows one exact table descriptor. @param ordinal Table index0or1. @return Descriptor or null for an invalid index. */
        [[nodiscard]] const ColorOutputTableDescriptor *Table(std::size_t ordinal) const noexcept;
        /** @brief Borrows original channel payload. @param ordinal Table index0or1. @return Exact bytes or empty for invalid index. */
        [[nodiscard]] std::span<const std::byte> TableSource(std::size_t ordinal) const noexcept;
        /** @brief Borrows lossless native upload layout. @param ordinal Table index0or1. @return Exact bytes or empty for invalid index. */
        [[nodiscard]] std::span<const std::byte> TableUpload(std::size_t ordinal) const noexcept;
        /** @brief Counts complete owned code/source/upload payload storage. @return Finite bytes charged during plan admission. */
        [[nodiscard]] std::uint64_t PayloadBytes() const noexcept;

    private:
        /** @brief Copies validated cold metadata/payloads. @param descriptor Exact identity. @param source Generated bytes.
         * @param tables Exact two borrowed tables. */
        CookedColorOutputProgram(ColorOutputProgramDescriptor descriptor, std::string_view source,
                                 std::span<const ColorOutputTableInput> tables);
        ColorOutputProgramDescriptor descriptor_;
        std::string source_;
        std::array<ColorOutputTableDescriptor, TableCount> tables_;
        std::array<std::vector<std::byte>, TableCount> original_, upload_;
    };
}  // namespace Horo::Render
