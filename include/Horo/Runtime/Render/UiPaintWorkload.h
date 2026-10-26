#pragma once

/** @file UiPaintWorkload.h
 * @brief Bounded native paint instructions derived from a retained canonical Runtime UI snapshot.
 */
#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/RenderResource.h"
#include "Horo/Runtime/Render/RenderResourceDescriptors.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::Render {
    struct RenderGraphExecutionRequest;
    class IResidentUiPaintKernel;
    inline constexpr std::uint32_t NoUiPaintTexture = UINT32_MAX;

    /** @brief Fixed portable vertex ABI; positions are already transformed into target pixel coordinates. */
    struct UiPaintVertex final {
        std::array<float, 2> position{};
        std::array<float, 2> uv{};
        std::array<float, 4> color{}; /**< Linear straight-alpha source color; shader premultiplies exactly once. */
    };

    /** @brief Pixel scissor in canonical top-left target coordinates; zero extent means no covered pixels. */
    struct UiPaintScissor final {
        std::uint32_t x{}, y{}, width{}, height{};
        /** @brief Checks overflow-safe containment in a complete target. @param target Exact target extent.
         * @return Whether even an empty rectangle stays inside the target. */
        [[nodiscard]] bool Fits(FramebufferExtent target) const noexcept;
    };

    /** @brief Explicit stencil transition or premultiplied color contribution. */
    enum class UiPaintOperation : std::uint8_t {
        Paint,
        PushStencil,
        PopStencil,
        MultiplyCoverage /**< Multiplies the destination by the complete source alpha; never source-over. */
    };
    /** @brief Closed sampling interpretation of one explicitly declared source texture. */
    enum class UiPaintSampling : std::uint8_t {
        Solid,
        Color,
        Alpha,
        Premultiplied,   /**< One completed isolation surface; its RGB already contains source alpha. */
        RoundedRectangle /**< Analytic local-UV rounded rectangle coverage with exact logical extent/radii. */
    };

    /** @brief Complete source mip selection, including an explicit no-mipmap sampler. */
    enum class UiPaintMipmap : std::uint8_t {
        None,
        Nearest,
        Linear
    };

    /** @brief Fixed 64-byte raster parameter ABI shared with ui_paint.hlsl; native encoders set origin flags explicitly. */
    struct UiPaintParameters final {
        std::array<float, 2> extent{};
        float opacity{1.0F};
        std::uint32_t sampling{};
        std::uint32_t flags{}; /**< Bit 0 coverage, bit 1 sampled render-target Y flip, bit 2 coverage Y flip. */
        std::array<std::uint32_t, 3> reserved{};
        std::array<float, 2> maskExtent{1.0F, 1.0F}; /**< Local mask extent before the canonical affine transform. */
        std::array<std::uint32_t, 2> maskReserved{};
        std::array<float, 4> cornerRadii{}; /**< Clockwise top-left; adjacent sums fit maskExtent. */
    };

    static_assert(sizeof(UiPaintParameters) == 64);
    static_assert(offsetof(UiPaintParameters, maskExtent) == 32);
    static_assert(offsetof(UiPaintParameters, cornerRadii) == 48);

    /** @brief One ordered bounded indexed operation in a prepared native paint pass. */
    struct UiPaintDraw final {
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
        UiPaintScissor scissor;
        UiPaintOperation operation{UiPaintOperation::Paint};
        UiPaintSampling sampling{UiPaintSampling::Solid};
        std::uint8_t stencilDepth{}; /**< Before push/pop; paint requires equality with this depth. */
        float opacity{1.0F};         /**< Leaf opacity or one isolated-surface composite opacity, never distributed group opacity. */
        RenderSamplerFilter filter{RenderSamplerFilter::Linear};    /**< Exact minification filter; source textures are single-mip. */
        RenderSamplerFilter magFilter{RenderSamplerFilter::Linear}; /**< Independent magnification filter; both axes clamp to edge. */
        UiPaintMipmap mipmap{UiPaintMipmap::None}; /**< Preserved even for the currently admitted single-level source textures. */
        std::array<float, 2> maskExtent{1.0F, 1.0F};
        std::array<float, 4> cornerRadii{};
        std::uint32_t sampled{NoUiPaintTexture};  /**< Exact prepared table index for Color/Alpha/Premultiplied sampling. */
        std::uint32_t coverage{NoUiPaintTexture}; /**< Optional exact prepared table index for complete mask coverage. */
    };

    /** @brief Stable prepared pass with either active ordered draws or explicit hidden no-op contents. */
    enum class UiPaintPassActivity : std::uint8_t {
        Active,
        Hidden
    };

    /**
     * @brief One prepared raster pass, with all native resources named through exact graph identities.
     * @details Geometry and uniforms live in resident graph buffers pinned by the submission lease. Draw storage is borrowed
     * only during synchronous admission and is copied/encoded before return. Each isolation group is a separate graph target;
     * a subsequent Color draw samples the completed target once. No Runtime UI or native API type crosses RenderApi.
     */
    struct RenderGraphUiPaint final {
        RenderGraphResourceId color;   /**< Invalid selects the acquired primary output; otherwise an exact graph texture. */
        RenderGraphResourceId stencil; /**< Required whenever any draw changes or tests stencil. */
        RenderGraphResourceId vertices;
        RenderGraphResourceId indices;
        RenderGraphResourceId parameters;
        std::span<const RenderGraphResourceId>
            sampledResources; /**< Strictly sorted complete prepared table, including currently hidden reads. */
        FramebufferExtent extent;
        std::uint32_t vertexCount{};
        std::uint32_t indexCount{};
        std::span<const UiPaintDraw> draws;
        std::shared_ptr<IResidentUiPaintKernel> kernel;
        bool clearColor{};              /**< Isolated targets begin transparent; primary color is preserved. */
        bool clearStencil{};            /**< One exact depth-stencil target starts at zero before the first mask operation. */
        std::uint64_t frameRevision{};  /**< Exact accepted reusable vertex/index/uniform slot publication. */
        std::uint32_t firstParameter{}; /**< First 256-byte uniform record used by this pass's ordered draws. */
        UiPaintPassActivity activity{UiPaintPassActivity::Active};
    };

    /** @brief Finite realization and per-pass encoding envelope, independent of backend implementation. */
    struct UiPaintLimits final {
        static constexpr std::uint32_t MaximumDraws = 16'384;
        static constexpr std::uint32_t MaximumSampledResources = 2'048;
        static constexpr std::uint32_t MaximumVertices = 1'048'576;
        static constexpr std::uint32_t MaximumIndices = 1'572'864;
        static constexpr std::uint8_t MaximumStencilDepth = 32;
    };

    /** @brief Validates all range, stencil, sampling and exact graph-use evidence before any native command.
     * @param request Exact compiled graph and resolved submission resources. @param pass Owning retained graphics pass.
     * @param paint Synchronously borrowed prepared paint. @return Original typed invalid/unsupported failure or success. */
    [[nodiscard]] Result<void> ValidateUiPaintWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                       const RenderGraphUiPaint &paint);
}  // namespace Horo::Render
