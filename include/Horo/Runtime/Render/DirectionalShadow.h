#pragma once

/** @file DirectionalShadow.h @brief Fixed-capacity camera-relative directional cascades and explicit native cache invalidation. */
#include "Horo/Runtime/Render/RenderScene.h"

#include <array>
#include <optional>

namespace Horo::Render {
    /** @brief Maximum complete directional-light cascade family supported by this interface. */
    inline constexpr std::uint32_t MaximumDirectionalShadowCascades = 4;
    /** @brief Unsampled guard width in pixels on every allocated tile edge; bounds the supported PCF radius. */
    inline constexpr std::uint32_t DirectionalShadowGuardPixels = 2;

    /** @brief Positive camera-forward split-distance partitioning policy. */
    enum class DirectionalShadowSplitMode : std::uint8_t {
        Mixed,      /**< Linear/logarithmic interpolation controlled by splitLambda. */
        Logarithmic /**< Pure logarithmic partitioning, independent of splitLambda. */
    };
    /** @brief Explicit product presets; resource admission remains the selected native owner's responsibility. */
    enum class DirectionalShadowQuality : std::uint8_t {
        Off,    /**< Disabled sampling and rendering; retained native resources may remain allocated. */
        Low,    /**< One 1024-pixel tile and a single depth comparison. */
        Medium, /**< Two 1024-pixel tiles and a three-by-three PCF kernel. */
        High,   /**< Four 1024-pixel tiles and a three-by-three PCF kernel; default preset. */
        Ultra   /**< Four 2048-pixel tiles and a five-by-five PCF kernel. */
    };

    /** @brief Owned settings copied into an immutable prepared frame; contains no native resource or borrowed state.
     * @details tileResolution includes the guard on every edge. Successful validation permits a profile,
     * but does not allocate an atlas or prove device admission. Native owners publish replacement resources atomically.
     */
    struct DirectionalShadowProfile final {
        bool enabled{true};                 /**< Off retains valid positive count/resolution settings but produces no prepared frame. */
        std::uint32_t cascadeCount{4};      /**< Active family size, one through four. */
        std::uint32_t tileResolution{1024}; /**< Allocated tile edge in pixels; power of two from 64 through 2048. */
        float maximumDistance{100.0F};      /**< Coverage in meters, [0.01,100000], capped by the camera far plane. */
        DirectionalShadowSplitMode splitMode{DirectionalShadowSplitMode::Mixed}; /**< Explicit supported partitioning policy. */
        float splitLambda{0.65F};      /**< Mixed-policy logarithmic weight in [0,1]; zero is linear. */
        float blendFraction{0.1F};     /**< Transition fraction of each logical slice in [0,0.5]. */
        float depthBias{0.001F};       /**< Normalized shadow-depth comparison offset in [0,0.1]. */
        float slopeBias{0.002F};       /**< Normalized depth offset in [0,0.1], scaled by receiver slope. */
        float normalBias{0.02F};       /**< Receiver normal offset in world meters, [0,10], independent of resolution. */
        std::uint32_t filterRadius{1}; /**< PCF radius in atlas texels, zero through two; kernel edge is 2*radius+1. */
        bool stabilize{true}; /**< Snaps the light-space receiver center to the texel grid; radius fitting remains outward-quantized. */
        /** @brief Validates finite split/bias settings and the fixed cascade/tile/filter budget without native work.
         * @return True for supported enabled or disabled profiles; false for malformed values or unsupported budgets.
         */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Compares every stored setting exactly. @return True when both profile values are identical. */
        [[nodiscard]] bool operator==(const DirectionalShadowProfile &) const noexcept = default;
    };

    /** @brief Top-left-origin inner raster rectangle in atlas pixels; excludes every tile's guard pixels. */
    struct DirectionalShadowViewport final {
        std::uint32_t x{}, y{}, width{}, height{}; /**< Pixel origin x/y and positive inner width/height. */
    };

    /** @brief Complete allocated atlas dimensions in pixels; zero dimensions represent a disabled profile. */
    struct DirectionalShadowAtlasExtent final {
        std::uint32_t width{}, height{}; /**< Allocated dimensions including all tile guards. */
        /** @brief Compares allocated dimensions exactly. @return True when both extents match. */
        [[nodiscard]] bool operator==(const DirectionalShadowAtlasExtent &) const noexcept = default;
    };

    /** @brief One overlapped receiver slice. Distances are positive camera-forward distances in scene meters. */
    struct DirectionalShadowCascade final {
        Math::Mat4 viewProjection{Math::Mat4::Identity()};               /**< World-input compatibility matrix. */
        Math::Mat4 cameraRelativeViewProjection{Math::Mat4::Identity()}; /**< Preferred matrix for localized instance transforms. */
        float splitNear{}, splitFar{}, blendStart{}; /**< Logical near/far distances and the outgoing blend start, in meters. */
        DirectionalShadowViewport viewport;          /**< Inner raster viewport, excluding the guard. */
        Math::Vec4 atlasScaleBias{};                 /**< Normalized inner rectangle: scale.xy, bias.zw; top-left origin. */
        float worldUnitsPerTexel{};                  /**< Receiver-fit world-space scale in meters per inner shadow texel. */
    };

    /** @brief Immutable synchronous pass input; no pointer, frame counter or backend resource ownership. */
    struct DirectionalShadowFrame final {
        std::array<DirectionalShadowCascade, MaximumDirectionalShadowCascades> cascades{}; /**< Only the cascadeCount prefix is active. */
        std::uint32_t cascadeCount{};                                                      /**< Complete required family size. */
        std::size_t lightIndex{};                  /**< Selected positive-intensity directional light's index in the source scene. */
        std::uint32_t atlasWidth{}, atlasHeight{}; /**< Complete allocated atlas dimensions in pixels. */
        Math::Vec3 cameraOrigin{};                 /**< World-space origin subtracted from models used with cameraRelativeViewProjection. */
        DirectionalShadowProfile profile;          /**< Exact admitted settings used for fitting and receiver sampling. */
        std::uint64_t fingerprint{}; /**< Actual camera, selected light, caster identity/transform/bounds and profile signature. */
        bool cacheable{true};        /**< False when actual geometry is absent or exceeds the finite hash cap; render again. */
        /** @brief Checks stored family, light selection, matrix and inner viewport invariants against a source scene.
         * @param scene Synchronously borrowed scene supplying the selected light; nothing is retained.
         * @return True for a structurally supported frame. This is not a geometry-freshness or native-render-completion check.
         */
        [[nodiscard]] bool IsValid(const RenderSceneView &scene) const noexcept;
    };

    /** @brief Native owner commits this value only after successful complete atlas rendering. Reset on resource loss. */
    struct DirectionalShadowCacheState final {
        std::uint64_t fingerprint{}, resourceIncarnation{},
            generation{}; /**< Completed input signature, exact native atlas identity, and accepted generation. */
        bool valid{};     /**< True only when a complete native render finished and actual geometry permits reuse. */
    };

    /** @brief Constructs one deterministic product preset without allocating or admitting native resources.
     * @param quality Supported explicit product quality selection.
     * @return Owned preset, or an invalid profile for an unknown enum value. High is the four-cascade, 1024-pixel-tile default.
     */
    [[nodiscard]] DirectionalShadowProfile MakeDirectionalShadowProfile(DirectionalShadowQuality quality) noexcept;
    /** @brief Computes one tile, two horizontal tiles, or a bounded two-by-two atlas without allocating resources.
     * @param profile Borrowed finite settings, including positive count/resolution when disabled.
     * @return Complete pixel extent, zero extent for Off, or InvalidProfile for unsupported settings.
     */
    [[nodiscard]] Result<DirectionalShadowAtlasExtent> ComputeDirectionalShadowAtlasExtent(const DirectionalShadowProfile &profile);
    /** @brief Fits camera-frustum receiver slices with conservative actual caster depth bounds, without frame allocations.
     * @details Off, no positive directional light, or no caster returns empty. Native owners retain all atlas resources.
     * Actual vertices/indices are fingerprinted within a four-MiB work cap. Missing CPU geometry or larger
     * submissions disable reuse rather than trusting a mesh ID/generation alone. Successful preparation allocates no frame storage.
     * @param scene Valid synchronously borrowed camera, lights, caster transforms/bounds and CPU mesh views; never retained.
     * @param aspect Finite positive viewport width-to-height ratio in [0.001,1000].
     * @param depthRange Explicit NegativeOneToOne for OpenGL or ZeroToOne for Metal/Vulkan; unknown values fail.
     * @param profile Exact finite settings to copy into the resulting frame.
     * @return Owned immutable frame, successful empty optional when no pass is needed, or typed profile/scene/coordinate/math failure.
     * @post Preparation neither creates native resources nor marks an atlas rendered; failed preparation commits no cache state.
     */
    [[nodiscard]] Result<std::optional<DirectionalShadowFrame>> BuildDirectionalShadowFrame(const RenderSceneView &scene, float aspect,
                                                                                            Math::ClipDepthRange depthRange,
                                                                                            const DirectionalShadowProfile &profile);
    /** @brief Compares a prepared input with one native owner's last successfully rendered atlas.
     * @param frame Owned prepared values, synchronously borrowed for this comparison.
     * @param resourceIncarnation Exact live nonzero native atlas incarnation; replace it when atlas storage changes.
     * @param cache Previously committed completion state, or a reset value after device/resource loss.
     * @return True for uncacheable geometry, absent completion, changed input or changed atlas identity; false permits reuse.
     * @details A false result is usable only by an owner that retained the completed atlas. Encoding commands alone is not completion.
     */
    [[nodiscard]] bool DirectionalShadowRenderRequired(const DirectionalShadowFrame &frame, std::uint64_t resourceIncarnation,
                                                       const DirectionalShadowCacheState &cache) noexcept;
    /** @brief Produces replacement cache state after the native owner has successfully rendered the complete atlas.
     * @param frame Exact frame whose complete native pass succeeded; borrowed only for this call.
     * @param resourceIncarnation Live nonzero identity of the actual rendered atlas allocation.
     * @param previous Prior accepted state; caller retains it if this operation fails.
     * @return Replacement state, unchanged state for an already accepted reusable identity, or InvalidCache on invalid identity/overflow.
     * @pre The native pass has completed successfully on its graphics owner. This does not require GPU idle.
     * Do not call merely because fitting or command encoding succeeded.
     * @details Generation advances only for accepted changed/repeated uncacheable renders. No frame counter is synthesized.
     */
    [[nodiscard]] Result<DirectionalShadowCacheState> CommitDirectionalShadowRender(const DirectionalShadowFrame &frame,
                                                                                    std::uint64_t resourceIncarnation,
                                                                                    const DirectionalShadowCacheState &previous);
}  // namespace Horo::Render
