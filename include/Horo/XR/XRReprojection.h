#pragma once

/**
 * @file XRReprojection.h
 * @brief Neutral motion, depth and pose evidence for optional runtime-owned frame synthesis.
 */

#include "Horo/XR/XRViewRenderPlan.h"

#include <array>
#include <optional>

namespace Horo::XR {
    /** @brief Exact two-frame motion convention; scene-only UV or motion-blur inputs are incompatible. */
    enum class XRReprojectionMotionConvention : std::uint8_t {
        CurrentNdcMinusPreviousNdcXYZ,
        Count
    };

    /** @brief Admitted formats and ordered view extents shared by the runtime provider and Renderer producer.
     * @details Copy the exact provider extent for each projection view, repeating the extent for uniform providers.
     */
    struct XRReprojectionPolicy final {
        Render::RenderTextureFormat motionFormat{Render::RenderTextureFormat::Rgba16Float}; /**< Three signed components in RGB. */
        Render::RenderTextureFormat depthFormat{Render::RenderTextureFormat::Depth32Float}; /**< Normalized device depth. */
        std::array<Render::FramebufferExtent, XRHardLimits::MaximumViews> viewExtents{}; /**< Motion/depth extents in projection order. */
        std::uint32_t viewCount{};                                                       /**< Number of admitted entries in viewExtents. */
        std::int64_t maximumPredictionIntervalNanoseconds{}; /**< Positive provider-supported predecessor interval ceiling. */
        XRReprojectionMotionConvention motion{XRReprojectionMotionConvention::Count}; /**< Explicit producer/provider agreement. */
    };

    /** @brief Auxiliary allocation role, independent of ordinary projection-layer depth. */
    enum class XRReprojectionImageKind : std::uint8_t {
        Motion,
        Depth,
        Count
    };

    /** @brief Exact immutable allocation evidence registered by its runtime acquisition owner. */
    struct XRReprojectionImage final {
        XRSwapchainImageId image;
        XRReprojectionImageKind kind{XRReprojectionImageKind::Count};
        Render::RenderTextureDescriptor allocation;
    };

    /** @brief Rendered subresource of a currently acquired and retired auxiliary allocation. */
    struct XRReprojectionSubImage final {
        XRSwapchainImageId image;
        XRRenderRectangle rectangle;
        std::uint32_t arrayLayer{};
        [[nodiscard]] constexpr auto operator<=>(const XRReprojectionSubImage &) const noexcept = default;
    };

    /** @brief Per-view inputs in exact runtime projection order, independent of native extension structures. */
    struct XRReprojectionViewInput final {
        XRViewId view;
        XRReprojectionSubImage motion;
        XRReprojectionSubImage depth;
        float minimumDepth{}; /**< Device depth range in [0, 1]. */
        float maximumDepth{1};
        float nearMeters{}; /**< Positive distance at minimumDepth. */
        float farMeters{};  /**< Positive distance at maximumDepth, possibly nearer for reverse depth. */
    };

    /** @brief Application-applied reference-space delta, separate from runtime-tracked view motion. */
    struct XRReprojectionSpaceDelta final {
        XRCoordinateSpace reference; /**< Exact current projection-layer reference and origin revision. */
        Math::Vec3 translationMeters;
        Math::Quaternion rotation;
    };

    /** @brief Completed per-eye classification over the exact output subresources.
     * @details An unresolved sample is any submitted
     * pixel for which motion/depth still lack a
     * provider-usable predecessor or a qualified producer policy. Cleared, skipped, and
     * newly
     * visible samples enter that count unless the Renderer actually resolved them.
     */
    struct XRReprojectionViewCoverage final {
        XRViewId view;
        XRReprojectionSubImage motion;
        XRReprojectionSubImage depth;
        std::uint64_t classifiedSamples{};
        std::uint64_t unresolvedSamples{};
    };

    /** @brief Coverage observed by the Renderer for this exact projection and predecessor.
     * @details The producer supplies this only
     * after its classification completed. Successful
     * draw encoding, attachment clears, and clipped per-object history do not
     * establish coverage.
     * The submission borrows the bounded observations synchronously.
     */
    struct XRReprojectionCoverage final {
        XRSessionId session;
        std::uint64_t frameSequence{};
        XRRenderPredictionTime prediction;
        std::uint64_t previousFrameSequence{};
        XRRenderPredictionTime previousPrediction;
        std::span<const XRReprojectionViewCoverage> views;
    };

    /** @brief Evidence from the exact renderer history and external allocation owners. */
    struct XRReprojectionInputs final {
        XRRenderPredictionTime previousPrediction; /**< Exact prediction associated with the renderer's predecessor. */
        std::uint64_t previousFrameSequence{};     /**< Exact preceding successfully projected frame, never a reused sequence. */
        std::optional<XRReprojectionSpaceDelta> applicationDelta; /**< Explicit current-space evidence; absence is never identity. */
        std::span<const XRReprojectionViewInput> views;           /**< Borrowed for the validation call only. */
        std::optional<XRReprojectionCoverage> coverage;           /**< Absent before exact producer evidence is available. */
    };

    /** @brief Result of optional input evaluation; only Ready permits the selected provider to receive frame inputs. */
    enum class XRReprojectionStatus : std::uint8_t {
        Ready,
        BaselineProjection,  /**< Feature disabled, unrequested or fell back at exact session admission. */
        ProjectionInvalid,   /**< Baseline frame is itself stale or incomplete; preserve its debt, do not submit it. */
        HistoryUnavailable,  /**< No adjacent compatible, actually submitted predecessor. */
        InputsInvalid,       /**< Active optional path cannot use the supplied format, range, pose or synchronization evidence. */
        CoverageUnavailable, /**< Renderer coverage for the exact pair was not available by submission cutoff. */
        CoverageRejected     /**< Reported output was incomplete, stale, mismatched, or contained unresolved samples. */
    };
}  // namespace Horo::XR
