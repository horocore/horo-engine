#pragma once

/**
 * @file UiCanvasSpace.h
 * @brief Backend-neutral Runtime UI canvas modes and deterministic screen-space resolution.
 */

#include "Horo/Runtime/Ui/UiIdentity.h"

#include <compare>
#include <cstdint>
#include <limits>

namespace Horo::Runtime::Ui {
    /** @brief Largest whole-DIP reference axis representable as signed 1/64-DIP layout geometry. */
    inline constexpr std::uint32_t MaximumUiCanvasReferenceDip = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max() / 64);

    /** @brief Closed semantic canvas projection mode; native renderer types are deliberately absent. */
    enum class UiRenderMode : std::uint8_t {
        ScreenSpaceOverlay,
        ScreenSpaceCamera,
        WorldSpace,
    };

    /** @brief Closed screen scaling policy defined by the Runtime UI architecture. */
    enum class UiScaleMode : std::uint8_t {
        ScaleWithScreenSize,
        ConstantPixelSize,
        ConstantPhysicalSize,
    };

    /** @brief Closed policy for mapping a platform-reported safe area into canvas content. */
    enum class UiSafeAreaMode : std::uint8_t {
        Ignore,
        Inset,
    };

    /** @brief Closed downstream presentation policy for physical pixel alignment. */
    enum class UiPixelSnapMode : std::uint8_t {
        Disabled,
        Edges,
    };

    /** @brief Largest numerator or denominator admitted for a user-facing UI scale factor. */
    inline constexpr std::uint32_t MaximumUiCanvasScaleFactorComponent = 4'096;

    /** @brief Positive bounded rational used for UI and accessibility text scale. */
    struct UiCanvasScaleFactor final {
        std::uint32_t numerator{1};   /**< Positive scale numerator. */
        std::uint32_t denominator{1}; /**< Positive scale denominator. */

        /** @brief Checks positive bounded scale evidence. @return Whether the factor is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiCanvasScaleFactor &) const noexcept = default;
    };

    /** @brief Positive authored reference resolution in whole logical DIPs. */
    struct UiCanvasReferenceResolution final {
        std::uint32_t width{1920};  /**< Positive horizontal reference DIPs. */
        std::uint32_t height{1080}; /**< Positive vertical reference DIPs. */

        /** @brief Checks that both axes fit the canonical 1/64-DIP domain. @return Whether the resolution is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiCanvasReferenceResolution &) const noexcept = default;
    };

    /** @brief Positive physical pixel extent supplied by a viewport/output owner. */
    struct UiCanvasPixelExtent final {
        std::uint32_t width{};  /**< Physical viewport width in pixels. */
        std::uint32_t height{}; /**< Physical viewport height in pixels. */

        /** @brief Checks that both physical axes are non-zero. @return Whether the extent is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiCanvasPixelExtent &) const noexcept = default;
    };

    /** @brief Reduced positive rational pixel scale supplied by the output-policy owner. */
    struct UiCanvasDeviceScale final {
        std::uint32_t pixelUnits{};  /**< Positive physical pixel units when evidence is present. */
        std::uint32_t logicalDips{}; /**< Positive logical DIP units when evidence is present. */

        /** @brief Checks positive scale evidence. @return Whether the evidence is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiCanvasDeviceScale &) const noexcept = default;
    };

    /** @brief Non-negative physical-pixel safe-area insets supplied by a viewport owner. */
    struct UiCanvasPixelInsets final {
        std::uint32_t left{};   /**< Inset from the physical left edge. */
        std::uint32_t top{};    /**< Inset from the physical top edge. */
        std::uint32_t right{};  /**< Inset from the physical right edge. */
        std::uint32_t bottom{}; /**< Inset from the physical bottom edge. */

        [[nodiscard]] auto operator<=>(const UiCanvasPixelInsets &) const noexcept = default;
    };

    /** @brief Positive physical-pixel rectangle bounded by its viewport extent. */
    struct UiCanvasPixelRect final {
        std::uint32_t x{};      /**< Physical left coordinate. */
        std::uint32_t y{};      /**< Physical top coordinate. */
        std::uint32_t width{};  /**< Positive physical width. */
        std::uint32_t height{}; /**< Positive physical height. */

        /** @brief Checks that the rectangle has positive dimensions. @return Whether the rectangle is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiCanvasPixelRect &) const noexcept = default;
    };

    /** @brief Authored and accessibility presentation choices resolved against one viewport evidence snapshot. */
    struct UiCanvasPresentationPolicy final {
        UiSafeAreaMode safeArea{UiSafeAreaMode::Ignore};      /**< Safe-area behavior. */
        UiCanvasScaleFactor uiScale{};                        /**< Uniform scale applied to all logical UI geometry. */
        UiCanvasScaleFactor fontScale{};                      /**< Accessibility scale applied to text measurement and shaping. */
        UiPixelSnapMode pixelSnap{UiPixelSnapMode::Disabled}; /**< Downstream screen-space edge snapping. */

        /** @brief Checks closed policies and bounded scale factors. @return Whether the policy is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiCanvasPresentationPolicy &) const noexcept = default;
    };

    /** @brief Authored canvas identity, root, projection mode, and screen scaling policy. */
    struct UiCanvasDescriptor final {
        UiCanvasId id;                                             /**< Stable identity of the canvas within the document. */
        UiElementId rootElement;                                   /**< Stable identity of the canvas root element. */
        UiRenderMode renderMode{UiRenderMode::ScreenSpaceOverlay}; /**< Semantic projection mode. */
        UiCanvasReferenceResolution referenceResolution{};         /**< Authored logical design resolution. */
        UiScaleMode scaleMode{UiScaleMode::ScaleWithScreenSize};   /**< Screen-space scaling policy. */
        UiCanvasPresentationPolicy presentation{};                 /**< Safe-area, accessibility, and output presentation policy. */

        /** @brief Checks identities, enum values, and reference bounds. @return Whether the descriptor is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiCanvasDescriptor &) const noexcept = default;
    };

    /** @brief Complete native-free viewport evidence consumed by Runtime UI resolution. */
    struct UiCanvasViewportEvidence final {
        UiCanvasPixelExtent pixelExtent;    /**< Positive drawable physical extent. */
        UiCanvasDeviceScale dpiScale;       /**< Optional positive physical-pixels-per-DIP evidence. */
        UiCanvasPixelInsets safeAreaInsets; /**< Platform-reported physical safe-area insets. */

        /** @brief Checks viewport and optional DPI evidence without inventing unavailable values. @return Whether evidence is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiCanvasViewportEvidence &) const noexcept = default;
    };

    /** @brief Resolved non-negative logical canvas extent in deterministic 1/64-DIP units. */
    struct UiCanvasLogicalExtent final {
        std::int32_t width{};  /**< Horizontal logical extent in 1/64 DIP. */
        std::int32_t height{}; /**< Vertical logical extent in 1/64 DIP. */

        [[nodiscard]] auto operator<=>(const UiCanvasLogicalExtent &) const noexcept = default;
    };

    /** @brief Allocation-free screen-space projection resolved from immutable caller evidence. */
    struct UiResolvedScreenCanvas final {
        UiCanvasLogicalExtent logicalExtent;                  /**< Complete logical viewport visible to layout. */
        UiCanvasPixelExtent pixelExtent;                      /**< Exact physical viewport evidence consumed. */
        UiCanvasDeviceScale pixelsPerDip;                     /**< Reduced uniform physical-pixels-per-DIP ratio. */
        UiCanvasPixelRect contentPixelRect;                   /**< Physical safe content rectangle; zero means the legacy full viewport. */
        UiCanvasPixelInsets safeAreaInsets;                   /**< Insets actually applied to contentPixelRect. */
        UiCanvasDeviceScale dpiScale;                         /**< Reduced caller-supplied DPI evidence, when available. */
        UiCanvasScaleFactor uiScale{};                        /**< Canonical all-UI scale used by pixelsPerDip. */
        UiCanvasScaleFactor fontScale{};                      /**< Canonical text-only accessibility scale. */
        UiPixelSnapMode pixelSnap{UiPixelSnapMode::Disabled}; /**< Downstream pixel-snap policy. */

        /** @brief Checks resolved extents, scales, safe rectangle, and closed policy values. @return Whether the projection is usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the safe content rectangle, treating an all-zero legacy value as the full viewport. @return Content rectangle. */
        [[nodiscard]] UiCanvasPixelRect ContentPixelRect() const noexcept;
        [[nodiscard]] auto operator<=>(const UiResolvedScreenCanvas &) const noexcept = default;
    };

    /**
     * @brief Resolves a screen overlay/camera canvas without renderer, platform, or editor objects.
     * @details ScaleWithScreenSize uses the smaller width/height ratio, preserving authored aspect while exposing any additional
     *          logical extent. ConstantPhysicalSize consumes already-resolved DPI evidence. Safe-area insets are applied only
     *          when the authored presentation policy requests them; UI scale affects the logical viewport while font scale is
     *          carried independently for text measurement. Pixel snapping remains downstream derived render data.
     * @param canvas Valid authored screen-space canvas descriptor.
     * @param viewport Exact positive physical viewport extent before the authored safe-area policy is applied.
     * @param deviceScale Caller-owned DPI evidence required only by ConstantPhysicalSize and ignored otherwise.
     * @param safeAreaInsets Caller-owned physical safe-area evidence consumed when the canvas policy is Inset.
     * @return Deterministic resolved metrics, or a typed malformed/mode/overflow error.
     */
    [[nodiscard]] Result<UiResolvedScreenCanvas> ResolveUiScreenCanvas(const UiCanvasDescriptor &canvas, UiCanvasPixelExtent viewport,
                                                                       UiCanvasDeviceScale deviceScale = {},
                                                                       UiCanvasPixelInsets safeAreaInsets = {});

    /**
     * @brief Resolves a screen canvas from one complete immutable viewport evidence value.
     * @param canvas Valid authored screen-space canvas descriptor.
     * @param evidence Exact viewport, optional DPI, and physical safe-area evidence.
     * @return Deterministic resolved metrics, or a typed malformed/mode/overflow error.
     */
    [[nodiscard]] Result<UiResolvedScreenCanvas> ResolveUiScreenCanvasWithEvidence(const UiCanvasDescriptor &canvas,
                                                                                   const UiCanvasViewportEvidence &evidence);

    /**
     * @brief Resolves the authored logical extent of a world-space canvas for headless layout.
     * @details Camera projection and device pixels remain Renderer-owned and are intentionally not fabricated here.
     * @param canvas Valid authored world-space canvas descriptor.
     * @return Exact 1/64-DIP logical extent, or a typed malformed/mode error.
     */
    [[nodiscard]] Result<UiCanvasLogicalExtent> ResolveUiWorldCanvas(const UiCanvasDescriptor &canvas);
}  // namespace Horo::Runtime::Ui
