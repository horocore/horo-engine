#pragma once

/** @file ReconstructionFrame.h @brief Provider-neutral inputs and history admission for one real reconstruction frame. */
#include "Horo/Runtime/Render/MotionHistory.h"
#include "Horo/Runtime/Render/RenderCapabilities.h"
#include "Horo/Runtime/Render/RenderGraph.h"
#include "Horo/Runtime/Render/RenderScene.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <variant>

namespace Horo::Render {
    /** @brief Reconstruction operation selected before frame admission. */
    enum class ReconstructionKind : std::uint8_t {
        Spatial,
        Temporal
    };

    /** @brief Linear ACEScg representation preserved by reconstruction. */
    enum class ReconstructionColorStage : std::uint8_t {
        Exposed,
        PreExposed
    };

    /** @brief Exact borrowed same-frame texture and its immutable resident descriptor. */
    struct ReconstructionTextureInput final {
        RenderTextureHandle texture;
        RenderTextureDescriptor descriptor;
        RenderViewId view;
        std::uint64_t frameId{};
    };

    /** @brief Mask output from a declared pass. Producers guarantee finite samples in [0, 1]. */
    struct ReconstructionMaskInput final {
        ReconstructionTextureInput image;
        RenderGraphPassRef producer;
    };

    /** @brief Optional masks keep their individual meaning regardless of storage format. */
    struct ReconstructionMasks final {
        std::optional<ReconstructionMaskInput> reactive;
        std::optional<ReconstructionMaskInput> transparency;
        std::optional<ReconstructionMaskInput> disocclusion;
        std::optional<ReconstructionMaskInput> composition;
    };

    /** @brief Fixed exposure scales, each finite and strictly positive. */
    struct ReconstructionExposureScales final {
        float current{1.0F};
        float previous{1.0F};
    };

    /** @brief Exact view-owned GPU scale buffers. No CPU readback is required. */
    struct ReconstructionExposureBuffers final {
        RenderViewId view;
        std::uint64_t generation{};
        RenderBufferHandle current;
        std::optional<RenderBufferHandle> previous; /**< May be absent for a history-reset frame. */
    };

    /** @brief Exposure supplied by the existing view owner as scales or device-local resources. */
    using ReconstructionExposure = std::variant<ReconstructionExposureScales, ReconstructionExposureBuffers>;

    /** @brief Exact real-frame sequence and its source simulation timing. */
    struct ReconstructionTiming final {
        std::uint64_t frameId{};
        std::uint64_t predecessorFrameId{}; /**< Zero represents an unavailable predecessor. */
        std::uint64_t simulationTick{};
        std::chrono::nanoseconds realFrameDelta{};
        std::uint64_t jitterSample{}; /**< Zero-based index within the admitted jitter generation. */
    };

    /** @brief Target-extent color write and its graph lifetime. */
    struct ReconstructionOutput final {
        RenderTextureDescriptor descriptor;
        RenderGraphResourceClass lifetime{RenderGraphResourceClass::Transient};
        RenderGraphUsageKind writeUsage{RenderGraphUsageKind::ColorAttachment};
        std::optional<RenderTextureHandle> resident; /**< Absent for graph-owned transient storage. */
    };

    /** @brief Complete real-frame inputs. The active graph/resource owners retain all storage.
     * @details UV has a top-left origin with texel centers at ((x + 0.5)/width, (y + 0.5)/height).
     * Motion excludes projection jitter and stores previous UV minus current UV at render extent.
     * Depth stores positive linear view-space meters at render extent. Color and output share one linear stage.
     */
    struct ReconstructionFrameRequest final {
        ReconstructionKind kind{ReconstructionKind::Temporal};
        ReconstructionColorStage colorStage{ReconstructionColorStage::PreExposed};
        TemporalHistoryCompatibility
            compatibility; /**< Spatial requires view, source/target extents and device only. Temporal requires the complete identity. */
        RenderResourceOwnerId resourceOwner;
        ReconstructionTiming timing;
        RenderProjectionDescriptor projection;
        Math::Vec2 currentJitterUv{};
        Math::Vec2 previousJitterUv{};
        RenderMotionVectorConvention motionConvention{RenderMotionVectorConvention::PreviousUvMinusCurrentUv};
        ReconstructionTextureInput color;
        std::optional<ReconstructionTextureInput> positiveLinearDepth;
        std::optional<ReconstructionTextureInput> motion;
        ReconstructionExposure exposure;
        ReconstructionMasks masks;
        ReconstructionOutput output;
        std::optional<TemporalHistoryResetCause> reset;
    };

    namespace ReconstructionErrors {
        extern const ErrorCodeDescriptor InvalidFrame;
        extern const ErrorCodeDescriptor MissingGuides;
        extern const ErrorCodeDescriptor IncompatibleInput;
        extern const ErrorCodeDescriptor IncompatibleOutput;
        extern const ErrorCodeDescriptor UnsupportedResource;
    }  // namespace ReconstructionErrors

    /** @brief Admits exact frame metadata and logical resources before the provider schedules work.
     * @param request Provider-neutral frame description. Input storage remains borrowed.
     * @param capabilities Selected device capability snapshot.
     * @return Success or a typed invalid, missing, incompatible or unsupported result.
     * @details Validation does not read texels, allocate GPU resources, alter history or select a fallback.
     */
    [[nodiscard]] Result<void> ValidateReconstructionFrame(const ReconstructionFrameRequest &request,
                                                           const RenderCapabilitySnapshot &capabilities);

    /** @brief Validates one temporal reconstruction request and opens the existing owner's exact history attempt.
     * @param request Complete temporal frame description.
     * @param capabilities Selected device capability snapshot.
     * @param store Owner-thread temporal history store.
     * @param history History handle issued by that store for the requested view and generations.
     * @return Exact pending frame for Publish or Abandon, or a typed failure with no pending attempt.
     * @details Only the caller's successful real-frame completion may Publish. No native or GPU work occurs here.
     */
    [[nodiscard]] Result<TemporalHistoryFrame> BeginTemporalReconstructionFrame(const ReconstructionFrameRequest &request,
                                                                                const RenderCapabilitySnapshot &capabilities,
                                                                                TemporalHistoryStore &store, TemporalHistoryHandle history);
}  // namespace Horo::Render
