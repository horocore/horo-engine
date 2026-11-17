#pragma once
/** @file ColorOutputPackage.h @brief Narrow read-only product color/exposure data decoding; no compiler, process or trust activation. */
#include "Horo/Runtime/Render/ColorOutputProgram.h"
#include "Horo/Runtime/Render/ExposureGpuKernel.h"

#include <array>
#include <filesystem>

namespace Horo::Render::Internal {
    /** @brief Owns seven exact analytic producer files and their integrity-validated immutable program. */
    struct ColorOutputAnalyticBundle final {
        std::shared_ptr<const CookedColorOutputProgram> program;
        std::array<std::vector<std::uint8_t>, 7> files;
    };

    /** @brief Actual decoded native pairs in scene/encode/optional-look order; identity is not cook trust. */
    struct ColorOutputCookPackage final {
        ColorOutputAnalyticBundle analytic;
        std::array<CompiledShaderArtifact, 3> programs;
        std::size_t programCount{2};

        /** @brief Borrows the real optional look variant without substituting the baseline scene artifact.
         * @return Third native artifact, or null when the published package does not contain it. */
        [[nodiscard]] const CompiledShaderArtifact *LookProgram() const noexcept {
            return programCount == 3 ? &programs[2] : nullptr;
        }
    };

    /** @brief Returns fixed original producer filenames. @return Seven exact ordered paths; no discovery. */
    [[nodiscard]] std::span<const std::string_view, 7> ColorOutputAnalyticFilenames() noexcept;
    /** @brief Returns baseline scene/encode package prefixes. @return Two stable legacy identities. */
    [[nodiscard]] std::span<const std::string_view, 2> ColorOutputCookPrefixes() noexcept;
    /** @brief Returns one actual variant prefix. @param index Scene0,encode1,look2. @return Exact prefix or empty for invalid index. */
    [[nodiscard]] std::string_view ColorOutputCookPrefix(std::size_t index) noexcept;
    /** @brief Returns fixed complete source filenames in scene-combined/wrapper,encode,look-combined/wrapper order.
     * @return Five inventory paths; baseline packages use the first three. */
    [[nodiscard]] std::span<const std::string_view, 5> ColorOutputSourceFilenames() noexcept;
    /** @brief Returns the fixed product Metal descriptor. @return Inert bounded target metadata, no device/tool activity. */
    [[nodiscard]] ShaderTargetRequirement ColorOutputMetalCookTarget();
    /** @brief Resolves an explicit ordinary directory. @param root Absolute supplied root. @return Canonical root or typed IO/schema error.
     */
    [[nodiscard]] Result<std::filesystem::path> ResolveColorPackageRoot(const std::filesystem::path &root);
    /** @brief Reads a bounded exact file beneath a previously resolved root without following a final symlink.
     * @param root Canonical root. @param relative Explicit relative child. @param maximum Finite byte limit.
     * @return Actual owned bytes or preserved typed failure; no directory discovery occurs. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> ReadColorPackageFile(const std::filesystem::path &root, std::string_view relative,
                                                                         std::size_t maximum);
    /** @brief Loads original analytic data only. @param root Exact producer directory. @return Owned validated bytes/program or failure. */
    [[nodiscard]] Result<ColorOutputAnalyticBundle> LoadColorOutputAnalyticBundle(const std::filesystem::path &root);
    /** @brief Decodes complete baseline schema1 or look-enabled schema2 product data.
     * @param root Exact published package. @return Actual artifact pairs and pinned analytic bytes, without trust/native qualification. */
    [[nodiscard]] Result<ColorOutputCookPackage> LoadColorOutputCookPackage(const std::filesystem::path &root);
    /** @brief Encodes the shared generation schema from actual artifacts/source bytes; compiler provenance remains the cooker's authority.
     * @param analytic Exact original producer generation. @param programs Actual ordered2or3 artifacts.
     * @param sources Ordered scene-combined,scene-wrapper,encode,optional-look-combined,optional-look-wrapper bytes.
     * @return Canonical finite generation JSON bytes or typed inconsistency. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeColorOutputGeneration(const ColorOutputAnalyticBundle &analytic,
                                                                                std::span<const CompiledShaderArtifact> programs,
                                                                                std::span<const std::vector<std::uint8_t>> sources);
    /** @brief Returns fixed clear/build/adapt Metal data metadata. @return Inert bounded target descriptor. */
    [[nodiscard]] ShaderTargetRequirement ExposureGpuMetalCookTarget();
    /** @brief Returns exact exposure package names. @return Three ordered prefixes; no discovery. */
    [[nodiscard]] std::span<const std::string_view, 3> ExposureGpuCookPrefixes() noexcept;
    /** @brief Decodes actual closed exposure artifacts against the canonical kernel contract.
     * @param root Exact published package. @return Immutable owned native bytes/reflection or typed failure; no native realization. */
    [[nodiscard]] Result<CookedExposureGpuKernel> LoadExposureGpuCookPackage(const std::filesystem::path &root);
}  // namespace Horo::Render::Internal
