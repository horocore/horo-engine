#pragma once

/** @file NativeReconstructionKernel.h @brief Closed spatial and temporal compute shader preparation. */
#include "Horo/Runtime/Render/ReconstructionFrame.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Render {
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
     * @return Bounded constants or a typed input/unsupported result.
     * @details The initial kernel uses RGBA32F target color, separate R32F temporal depth, and fixed exposure scale
     * inputs. Masks and buffer exposure require a separately qualified variant. History reads are gated by the existing
     * owner's exact attempt. This function grants no native submission or provider availability.
     */
    [[nodiscard]] Result<NativeReconstructionDispatch> PrepareNativeReconstructionDispatch(
        const ReconstructionFrameRequest &request, const RenderCapabilitySnapshot &capabilities,
        const NativeReconstructionTemporalSettings &tuning, std::optional<NativeReconstructionHistory> history = std::nullopt);

    /** @brief Describes one exact native shader entry for real offline compilation.
     * @param kind Spatial or temporal operation. @param target Explicit Vulkan SPIR-V target.
     * @return Canonical manifest and reflection names, or typed unsupported failure.
     */
    [[nodiscard]] Result<NativeReconstructionShaderContract> DescribeNativeReconstructionShader(ReconstructionKind kind,
                                                                                                const ShaderTargetRequirement &target);

    /** @brief Revalidates a real cook-bound artifact before a concrete renderer can realize it.
     * @param kind Selected closed kernel. @param target Exact target used for cooking.
     * @param artifact Owned cooked bytes and bound final-target reflection.
     * @return Borrowed compute program bytes, or typed unsupported/stale/incorrect layout failure.
     */
    [[nodiscard]] Result<std::span<const std::uint8_t>> AdmitNativeReconstructionShader(ReconstructionKind kind,
                                                                                        const ShaderTargetRequirement &target,
                                                                                        const CompiledShaderArtifact &artifact);
}  // namespace Horo::Render
