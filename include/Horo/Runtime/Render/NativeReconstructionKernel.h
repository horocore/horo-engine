#pragma once

/** @file NativeReconstructionKernel.h @brief Closed spatial and temporal compute shader preparation. */
#include "Horo/Runtime/Render/ReconstructionFrame.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Render {
    /** @brief Closed image roles represented by each compiled native shader entry. */
    enum class NativeReconstructionEntry : std::uint8_t {
        Spatial,         /**< Reads current color and writes resolved color. */
        TemporalSeed,    /**< Writes resolved color/depth without motion or prior-history descriptors. */
        TemporalHistory, /**< Uses motion and exact previous resolved color/depth. */
    };

    /** @brief Returns the exact NUL-terminated shader entry literal or an empty view for an unknown entry. */
    [[nodiscard]] std::string_view NativeReconstructionEntryName(NativeReconstructionEntry entry) noexcept;

    /** @brief Image shader slots in exact native descriptor order. */
    enum class NativeReconstructionImage : std::uint8_t {
        CurrentColor,
        OutputColor,
        CurrentDepth,
        Motion,
        PreviousColor,
        PreviousDepth,
        OutputDepth,
        Count,
    };

    /** @brief Returns whether one entry declares this image descriptor. */
    [[nodiscard]] bool NativeReconstructionUsesImage(NativeReconstructionEntry entry, NativeReconstructionImage image) noexcept;

    /** @brief Checks the closed floating-point native texture format for one image role. */
    [[nodiscard]] bool NativeReconstructionImageFormatMatches(NativeReconstructionImage role, RenderTextureFormat format) noexcept;

    /** @brief Returns whether this image role is exclusively written by the shader. */
    [[nodiscard]] constexpr bool NativeReconstructionWritesImage(NativeReconstructionImage image) noexcept {
        return image == NativeReconstructionImage::OutputColor || image == NativeReconstructionImage::OutputDepth;
    }

    /** @brief Exact native reconstruction kernel constants shared with native_reconstruction.hlsl. */
    struct alignas(16) NativeReconstructionDispatch final {
        std::array<std::uint32_t, 4> extents{}; /**< Render width/height followed by resolved target width/height. */
        std::array<float, 4> sample{};          /**< Current jitter UV, history pre-exposure scale and effective history weight. */
        std::array<float, 4> depth{};           /**< Absolute depth tolerance in meters, relative tolerance and zero padding. */

        /** @brief Width and height of one compute group in target pixels. */
        static constexpr std::uint32_t GroupExtent = 8;

        /** @brief Returns ceil-divided target group counts. @return Zero when either extent is unset. */
        [[nodiscard]] constexpr FramebufferExtent Groups() const noexcept {
            if (extents[2] == 0 || extents[3] == 0)
                return {};
            return {(extents[2] - 1) / GroupExtent + 1, (extents[3] - 1) / GroupExtent + 1};
        }
    };

    /** @brief Selected shader entry with its exact bounded packed constants. */
    struct NativeReconstructionDispatchPlan final {
        NativeReconstructionEntry entry{NativeReconstructionEntry::Spatial};
        NativeReconstructionDispatch constants;
    };

    /** @brief Explicit provider tuning for a temporal mode. Spatial filtering ignores these fields. */
    struct NativeReconstructionTemporalSettings final {
        float historyWeight{};          /**< Finite blend weight in [0, 1). */
        float absoluteDepthTolerance{}; /**< Finite nonnegative tolerance in view-space meters. */
        float relativeDepthTolerance{}; /**< Finite fractional tolerance in [0, 1]. */
    };

    /** @brief Borrowed owner and frame evidence for synchronous temporal preparation. */
    struct NativeReconstructionHistory final {
        TemporalHistoryStore &owner;
        const TemporalHistoryFrame &frame;
    };

    /** @brief One canonical manifest and authored names for the existing generic Vulkan cook pipeline. */
    struct NativeReconstructionShaderContract final {
        ShaderManifest manifest;
        std::vector<ShaderReflectionBindingName> reflectionNames;
    };

    /** @brief Exact offline-cooked compute stage for cold native pipeline realization. */
    struct CookedNativeReconstructionKernel final {
        NativeReconstructionEntry entry;
        ShaderTargetRequirement target;
        CompiledShaderArtifact artifact;
    };

    /** @brief Backend-owned native pipeline lease with no native handles in the public API.
     * @details The host releases the lease on the render owner thread. Accepted submissions retain the exact lease
     * through completion. Backend shutdown revokes it after the command owner has drained.
     */
    class IResidentNativeReconstructionKernel {
    public:
        virtual ~IResidentNativeReconstructionKernel() = default;
    };

    namespace NativeReconstructionErrors {
        extern const ErrorCodeDescriptor InvalidDispatch;
        extern const ErrorCodeDescriptor Unsupported;
        extern const ErrorCodeDescriptor InvalidArtifact;
    }  // namespace NativeReconstructionErrors

    /** @brief Validates and packs one dispatch without touching native or history state.
     * @param request Already resolved reconstruction settings and current frame resources.
     * @param capabilities Selected device resource and compute capabilities.
     * @param tuning Explicit temporal blending and depth rejection policy.
     * @param history Borrowed history owner and pending attempt, required only for Temporal.
     * @return Exact shader entry and bounded constants, or a typed input/unsupported result.
     * @details The initial kernel uses RGBA32F target color, separate R32F temporal depth, and fixed exposure scale
     * inputs. Masks and buffer exposure require a separately qualified variant. History reads are gated by the existing
     * owner's exact attempt. First/reset frames use a separate entry without prior descriptors. This function grants no native submission
     * or provider availability.
     */
    [[nodiscard]] Result<NativeReconstructionDispatchPlan> PrepareNativeReconstructionDispatch(
        const ReconstructionFrameRequest &request, const RenderCapabilitySnapshot &capabilities,
        const NativeReconstructionTemporalSettings &tuning, std::optional<NativeReconstructionHistory> history = std::nullopt);

    /** @brief Describes one exact native shader entry for real offline compilation.
     * @param entry Exact spatial, history seed, or history accumulation shader entry. @param target Explicit Vulkan SPIR-V target.
     * @return Canonical manifest and reflection names, or typed unsupported failure.
     */
    [[nodiscard]] Result<NativeReconstructionShaderContract> DescribeNativeReconstructionShader(NativeReconstructionEntry entry,
                                                                                                const ShaderTargetRequirement &target);

    /** @brief Revalidates a real cook-bound artifact before a concrete renderer can realize it.
     * @param entry Selected closed kernel entry. @param target Exact target used for cooking.
     * @param artifact Owned cooked bytes and bound final-target reflection.
     * @return Borrowed compute program bytes, or typed unsupported/stale/incorrect layout failure.
     */
    [[nodiscard]] Result<std::span<const std::uint8_t>> AdmitNativeReconstructionShader(NativeReconstructionEntry entry,
                                                                                        const ShaderTargetRequirement &target,
                                                                                        const CompiledShaderArtifact &artifact);
}  // namespace Horo::Render
