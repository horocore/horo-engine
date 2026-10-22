#pragma once

/**
 * @file RenderDisplay.h
 * @brief Backend-neutral revisioned display capability and change contracts.
 */

#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Horo::Render {
    /** @brief Maximum display records admitted in one platform snapshot. */
    inline constexpr std::size_t MaximumRenderDisplays = 32;
    /** @brief Maximum modes admitted for one display. */
    inline constexpr std::size_t MaximumRenderDisplayModes = 256;
    /** @brief Maximum supported pixel extent on either display-mode axis. */
    inline constexpr std::uint32_t MaximumRenderDisplayPixelExtent = 32'768;
    /** @brief Maximum supported display refresh fact in hertz. */
    inline constexpr float MaximumRenderDisplayRefreshHertz = 1'000.0F;
    /** @brief Maximum supported display luminance fact in nits. */
    inline constexpr float MaximumRenderDisplayLuminanceNits = 10'000.0F;

    /** @brief Stable machine-local display identity with no native handle semantics. */
    class RenderDisplayId {
    public:
        RenderDisplayId() = default;

        /** @brief Creates an identity from a platform-published stable value. @param value Stable platform value. */
        explicit RenderDisplayId(std::string value);

        /** @brief Returns the platform-published stable value. @return Stored stable value. */
        [[nodiscard]] const std::string &Value() const noexcept;

        /** @brief Reports whether the value is a bounded printable identifier. */
        [[nodiscard]] bool IsValid() const noexcept;

        [[nodiscard]] auto operator<=>(const RenderDisplayId &) const noexcept = default;

    private:
        std::string value_;
    };

    /** @brief Backend-neutral color primaries and white-point family. */
    enum class RenderDisplayColorSpace : std::uint8_t {
        Srgb,
        DisplayP3,
        Rec2020,
    };

    /** @brief Transfer function attached to a reported output mode.
     * @details High + Linear with sRGB/Display P3 denotes separately qualified platform linear EDR, not HDR10 PQ.
     * Reported transfer facts never select or prove a scene tone map or output conversion.
     */
    enum class RenderDisplayTransferFunction : std::uint8_t {
        Srgb,
        Linear,
        Pq,
        Hlg,
    };

    /** @brief Dynamic-range class of a reported output mode. */
    enum class RenderDisplayDynamicRange : std::uint8_t {
        Standard,
        High,
    };

    /** @brief Whether the OS/display pair currently reports HDR output support. */
    enum class RenderDisplayHdrSupport : std::uint8_t {
        Unknown,
        Unsupported,
        Supported,
    };

    /** @brief Optional reported luminance facts in nits. */
    struct RenderDisplayLuminance final {
        float minimumNits{};      /**< Minimum representable luminance. */
        float maximumFullFrame{}; /**< Maximum sustained full-frame luminance. */
        float maximumPeak{};      /**< Maximum peak luminance. */

        [[nodiscard]] bool operator==(const RenderDisplayLuminance &) const noexcept = default;
    };

    /** @brief One exact platform-reported pixel, refresh, color, and range mode. */
    struct RenderDisplayMode final {
        std::uint32_t widthPixels{};                                                 /**< Drawable pixel width. */
        std::uint32_t heightPixels{};                                                /**< Drawable pixel height. */
        float refreshHertz{};                                                        /**< Finite positive refresh rate. */
        RenderDisplayColorSpace colorSpace{RenderDisplayColorSpace::Srgb};           /**< Reported color-space family. */
        RenderDisplayTransferFunction transfer{RenderDisplayTransferFunction::Srgb}; /**< Reported transfer function. */
        RenderDisplayDynamicRange dynamicRange{RenderDisplayDynamicRange::Standard}; /**< Reported SDR/HDR class. */

        [[nodiscard]] bool operator==(const RenderDisplayMode &) const noexcept = default;
    };

    /** @brief Immutable facts for one display, including its exact current mode. */
    struct RenderDisplayProperties final {
        RenderDisplayId id;                                            /**< Stable identity within the platform owner lifetime. */
        RenderDisplayHdrSupport hdr{RenderDisplayHdrSupport::Unknown}; /**< Current OS/display HDR support. */
        std::optional<RenderDisplayLuminance> luminance;               /**< Reported luminance, or absent when unknown. */
        std::vector<RenderDisplayMode> modes;                          /**< Strictly sorted unique supported modes. */
        RenderDisplayMode currentMode;                                 /**< Exact member of modes currently reported active. */

        [[nodiscard]] bool operator==(const RenderDisplayProperties &) const noexcept = default;
    };

    /** @brief Owned bounded display facts published by the platform owner. */
    struct RenderDisplaySnapshot final {
        std::uint64_t revision{};                      /**< Non-zero strictly increasing platform revision. */
        std::vector<RenderDisplayProperties> displays; /**< Strictly identity-sorted unique records. */

        [[nodiscard]] bool operator==(const RenderDisplaySnapshot &) const noexcept = default;
    };

    /** @brief Stable reason one display appears in a revision delta. */
    enum class RenderDisplayChangeReason : std::uint8_t {
        Added,
        Removed,
        CurrentModeChanged,
        CapabilitiesChanged,
    };

    /** @brief One canonical display change notification without native state. */
    struct RenderDisplayChange final {
        RenderDisplayId display;            /**< Display affected by the transition. */
        RenderDisplayChangeReason reason{}; /**< Exact typed reason for publication. */

        [[nodiscard]] bool operator==(const RenderDisplayChange &) const noexcept = default;
    };

    /** @brief Bounded canonical delta between two validated display revisions. */
    struct RenderDisplaySnapshotDiff final {
        std::uint64_t previousRevision{};         /**< Prior revision, or zero for initial publication. */
        std::uint64_t currentRevision{};          /**< Newly accepted non-zero revision. */
        std::vector<RenderDisplayChange> changes; /**< Identity/reason-sorted bounded change records. */
    };

    /**
     * @brief Validates a complete display snapshot before publication.
     * @param snapshot Owned native-free platform facts.
     * @return Success, malformed-snapshot failure, or unsupported-fact failure.
     */
    [[nodiscard]] Result<void> ValidateRenderDisplaySnapshot(const RenderDisplaySnapshot &snapshot);

    /**
     * @brief Computes a bounded deterministic delta between validated snapshots.
     * @param previous Previously committed snapshot.
     * @param current Candidate snapshot with a strictly newer revision.
     * @return Canonical delta, or a typed malformed, unsupported, stale, or equal-revision failure.
     */
    [[nodiscard]] Result<RenderDisplaySnapshotDiff> DiffRenderDisplaySnapshots(const RenderDisplaySnapshot &previous,
                                                                               const RenderDisplaySnapshot &current);

    /**
     * @brief Owner-thread display publication boundary with no native enumeration or callbacks.
     *
     * The host feeds complete immutable snapshots at its platform safe point. Publication returns
     * the bounded change value directly; consumers pull the last owned snapshot separately. Stop
     * closes admission idempotently and releases the retained snapshot.
     */
    class RenderDisplaySnapshotFeed final {
    public:
        /** @brief Creates an empty running feed owned by the calling thread. */
        RenderDisplaySnapshotFeed() noexcept;

        RenderDisplaySnapshotFeed(const RenderDisplaySnapshotFeed &) = delete;
        RenderDisplaySnapshotFeed &operator=(const RenderDisplaySnapshotFeed &) = delete;

        /**
         * @brief Validates and commits one strictly newer snapshot on the owner thread.
         * @param snapshot Candidate owned platform facts.
         * @return Initial additions or the canonical revision delta; failure leaves current state unchanged.
         */
        [[nodiscard]] Result<RenderDisplaySnapshotDiff> Publish(RenderDisplaySnapshot snapshot);

        /** @brief Returns an owned copy of the last committed snapshot on the owner thread. */
        [[nodiscard]] Result<RenderDisplaySnapshot> Snapshot() const;

        /**
         * @brief Idempotently closes publication admission and releases retained facts.
         * @pre Called by the owner thread.
         */
        void Stop() noexcept;

    private:
        std::thread::id ownerThread_;
        std::optional<RenderDisplaySnapshot> current_;
        bool stopped_{false};
    };
}  // namespace Horo::Render
