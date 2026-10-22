#include "Horo/Runtime/Render/RenderDisplay.h"

#include "Horo/Runtime/Render/RenderDisplayErrors.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <tuple>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::size_t MaxIdentityLength = 128;

        /** @brief Reports whether a byte belongs to the stable display identity alphabet. */
        [[nodiscard]] bool IsIdentityCharacter(const unsigned char value) noexcept {
            constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-";
            return alphabet.find(static_cast<char>(value)) != std::string_view::npos;
        }

        /** @brief Reports whether a public color-space value is known. */
        [[nodiscard]] constexpr bool IsKnown(const RenderDisplayColorSpace value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(RenderDisplayColorSpace::Rec2020);
        }

        /** @brief Reports whether a public transfer-function value is known. */
        [[nodiscard]] constexpr bool IsKnown(const RenderDisplayTransferFunction value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(RenderDisplayTransferFunction::Hlg);
        }

        /** @brief Reports whether a public dynamic-range value is known. */
        [[nodiscard]] constexpr bool IsKnown(const RenderDisplayDynamicRange value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(RenderDisplayDynamicRange::High);
        }

        /** @brief Reports whether a public HDR-support value is known. */
        [[nodiscard]] constexpr bool IsKnown(const RenderDisplayHdrSupport value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(RenderDisplayHdrSupport::Supported);
        }

        /** @brief Provides canonical ordering for validated display modes. */
        [[nodiscard]] bool ModeLess(const RenderDisplayMode &left, const RenderDisplayMode &right) noexcept {
            return std::tuple{left.widthPixels,
                              left.heightPixels,
                              left.refreshHertz,
                              static_cast<std::uint8_t>(left.colorSpace),
                              static_cast<std::uint8_t>(left.transfer),
                              static_cast<std::uint8_t>(left.dynamicRange)} < std::tuple{right.widthPixels,
                                                                                         right.heightPixels,
                                                                                         right.refreshHertz,
                                                                                         static_cast<std::uint8_t>(right.colorSpace),
                                                                                         static_cast<std::uint8_t>(right.transfer),
                                                                                         static_cast<std::uint8_t>(right.dynamicRange)};
        }

        /** @brief Validates finite physical bounds for one optional luminance report. */
        [[nodiscard]] bool ValidLuminance(const RenderDisplayLuminance &value) noexcept {
            return std::isfinite(value.minimumNits) && std::isfinite(value.maximumFullFrame) && std::isfinite(value.maximumPeak) &&
                   value.minimumNits >= 0.0F && value.maximumFullFrame > 0.0F && value.minimumNits <= value.maximumFullFrame &&
                   value.maximumFullFrame <= value.maximumPeak && value.maximumPeak <= MaximumRenderDisplayLuminanceNits;
        }

        /** @brief Validates numeric bounds without interpreting color/HDR combinations. */
        [[nodiscard]] bool ValidModeBounds(const RenderDisplayMode &mode) noexcept {
            return mode.widthPixels > 0 && mode.widthPixels <= MaximumRenderDisplayPixelExtent && mode.heightPixels > 0 &&
                   mode.heightPixels <= MaximumRenderDisplayPixelExtent && std::isfinite(mode.refreshHertz) && mode.refreshHertz > 0.0F &&
                   mode.refreshHertz <= MaximumRenderDisplayRefreshHertz;
        }

        /** @brief Validates closed color, transfer, and dynamic-range combinations. */
        [[nodiscard]] bool SupportedModeFacts(const RenderDisplayMode &mode) noexcept {
            using enum RenderDisplayTransferFunction;
            if (!IsKnown(mode.colorSpace) || !IsKnown(mode.transfer) || !IsKnown(mode.dynamicRange))
                return false;
            if (mode.dynamicRange == RenderDisplayDynamicRange::Standard)
                return mode.transfer == Srgb || mode.transfer == Linear;
            const bool linearEdr = mode.transfer == Linear && (mode.colorSpace == RenderDisplayColorSpace::Srgb ||
                                                               mode.colorSpace == RenderDisplayColorSpace::DisplayP3);
            return linearEdr || (mode.colorSpace == RenderDisplayColorSpace::Rec2020 && mode.transfer == Pq);
        }

        /** @brief Validates one mode while preserving malformed versus unsupported failure semantics. */
        [[nodiscard]] Result<void> ValidateMode(const RenderDisplayMode &mode) {
            if (!ValidModeBounds(mode))
                return Result<void>::Failure(MakeError(RenderDisplayErrors::InvalidSnapshot));
            if (!SupportedModeFacts(mode))
                return Result<void>::Failure(MakeError(RenderDisplayErrors::UnsupportedFact));
            return Result<void>::Success();
        }

        /** @brief Validates one bounded display record independently of snapshot ordering. */
        [[nodiscard]] Result<void> ValidateDisplay(const RenderDisplayProperties &display) {
            if (!display.id.IsValid() || display.modes.empty() || display.modes.size() > MaximumRenderDisplayModes)
                return Result<void>::Failure(MakeError(RenderDisplayErrors::InvalidSnapshot));
            if (!IsKnown(display.hdr))
                return Result<void>::Failure(MakeError(RenderDisplayErrors::UnsupportedFact));
            if (display.luminance && !ValidLuminance(*display.luminance))
                return Result<void>::Failure(MakeError(RenderDisplayErrors::InvalidSnapshot));
            if (const Result<void> valid = ValidateMode(display.currentMode); valid.HasError())
                return valid;

            bool hasCurrentMode = false;
            bool hasHdrMode = false;
            for (std::size_t modeIndex = 0; modeIndex < display.modes.size(); ++modeIndex) {
                const RenderDisplayMode &mode = display.modes[modeIndex];
                if (const Result<void> valid = ValidateMode(mode); valid.HasError())
                    return valid;
                if (modeIndex > 0 && !ModeLess(display.modes[modeIndex - 1], mode))
                    return Result<void>::Failure(MakeError(RenderDisplayErrors::InvalidSnapshot));
                hasCurrentMode = hasCurrentMode || mode == display.currentMode;
                hasHdrMode = hasHdrMode || mode.dynamicRange == RenderDisplayDynamicRange::High;
            }

            if (!hasCurrentMode)
                return Result<void>::Failure(MakeError(RenderDisplayErrors::InvalidSnapshot));
            if ((hasHdrMode && display.hdr != RenderDisplayHdrSupport::Supported) ||
                (!hasHdrMode && display.hdr == RenderDisplayHdrSupport::Supported) ||
                (display.luminance.has_value() && display.hdr != RenderDisplayHdrSupport::Supported))
                return Result<void>::Failure(MakeError(RenderDisplayErrors::UnsupportedFact));
            return Result<void>::Success();
        }

        /** @brief Compares capability facts while excluding identity and current-mode selection. */
        [[nodiscard]] bool SameCapabilities(const RenderDisplayProperties &left, const RenderDisplayProperties &right) noexcept {
            return left.hdr == right.hdr && left.luminance == right.luminance && left.modes == right.modes;
        }

        /** @brief Adds one canonical change after capacity has been proven by snapshot bounds. */
        void AddChange(RenderDisplaySnapshotDiff &diff, const RenderDisplayId &display, const RenderDisplayChangeReason reason) {
            diff.changes.emplace_back(display, reason);
        }
    }  // namespace

    /** @copydoc RenderDisplayId::RenderDisplayId */
    RenderDisplayId::RenderDisplayId(std::string value) : value_(std::move(value)) {}

    /** @copydoc RenderDisplayId::Value */
    const std::string &RenderDisplayId::Value() const noexcept {
        return value_;
    }

    /** @copydoc RenderDisplayId::IsValid */
    bool RenderDisplayId::IsValid() const noexcept {
        if (value_.empty() || value_.size() > MaxIdentityLength)
            return false;
        return std::ranges::all_of(value_, [](const char value) {
            return IsIdentityCharacter(static_cast<unsigned char>(value));
        });
    }

    /** @copydoc ValidateRenderDisplaySnapshot */
    Result<void> ValidateRenderDisplaySnapshot(const RenderDisplaySnapshot &snapshot) {
        if (snapshot.revision == 0 || snapshot.displays.size() > MaximumRenderDisplays)
            return Result<void>::Failure(MakeError(RenderDisplayErrors::InvalidSnapshot));

        for (std::size_t displayIndex = 0; displayIndex < snapshot.displays.size(); ++displayIndex) {
            const RenderDisplayProperties &display = snapshot.displays[displayIndex];
            if (displayIndex > 0 && snapshot.displays[displayIndex - 1].id.Value() >= display.id.Value())
                return Result<void>::Failure(MakeError(RenderDisplayErrors::InvalidSnapshot));
            if (const Result<void> valid = ValidateDisplay(display); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc DiffRenderDisplaySnapshots */
    Result<RenderDisplaySnapshotDiff> DiffRenderDisplaySnapshots(const RenderDisplaySnapshot &previous,
                                                                 const RenderDisplaySnapshot &current) {
        if (const Result<void> valid = ValidateRenderDisplaySnapshot(previous); valid.HasError())
            return Result<RenderDisplaySnapshotDiff>::Failure(valid.ErrorValue());
        if (const Result<void> valid = ValidateRenderDisplaySnapshot(current); valid.HasError())
            return Result<RenderDisplaySnapshotDiff>::Failure(valid.ErrorValue());
        if (current.revision < previous.revision)
            return Result<RenderDisplaySnapshotDiff>::Failure(MakeError(RenderDisplayErrors::StaleRevision));
        if (current.revision == previous.revision)
            return Result<RenderDisplaySnapshotDiff>::Failure(MakeError(RenderDisplayErrors::RevisionUnchanged));

        RenderDisplaySnapshotDiff diff{previous.revision, current.revision, {}};
        diff.changes.reserve(previous.displays.size() + current.displays.size());
        using enum RenderDisplayChangeReason;
        std::size_t previousIndex = 0;
        std::size_t currentIndex = 0;
        while (previousIndex < previous.displays.size() || currentIndex < current.displays.size()) {
            if (previousIndex == previous.displays.size()) {
                AddChange(diff, current.displays[currentIndex++].id, Added);
                continue;
            }
            if (currentIndex == current.displays.size()) {
                AddChange(diff, previous.displays[previousIndex++].id, Removed);
                continue;
            }

            const RenderDisplayProperties &oldDisplay = previous.displays[previousIndex];
            const RenderDisplayProperties &newDisplay = current.displays[currentIndex];
            if (oldDisplay.id < newDisplay.id) {
                AddChange(diff, oldDisplay.id, Removed);
                ++previousIndex;
                continue;
            }
            if (newDisplay.id < oldDisplay.id) {
                AddChange(diff, newDisplay.id, Added);
                ++currentIndex;
                continue;
            }

            if (oldDisplay.currentMode != newDisplay.currentMode)
                AddChange(diff, newDisplay.id, CurrentModeChanged);
            if (!SameCapabilities(oldDisplay, newDisplay))
                AddChange(diff, newDisplay.id, CapabilitiesChanged);
            ++previousIndex;
            ++currentIndex;
        }
        return Result<RenderDisplaySnapshotDiff>::Success(std::move(diff));
    }

    /** @copydoc RenderDisplaySnapshotFeed::RenderDisplaySnapshotFeed */
    RenderDisplaySnapshotFeed::RenderDisplaySnapshotFeed() noexcept : ownerThread_(std::this_thread::get_id()) {}

    /** @copydoc RenderDisplaySnapshotFeed::Publish */
    Result<RenderDisplaySnapshotDiff> RenderDisplaySnapshotFeed::Publish(RenderDisplaySnapshot snapshot) {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<RenderDisplaySnapshotDiff>::Failure(MakeError(RenderDisplayErrors::ThreadAffinityViolation));
        if (stopped_)
            return Result<RenderDisplaySnapshotDiff>::Failure(MakeError(RenderDisplayErrors::FeedStopped));
        if (const Result<void> valid = ValidateRenderDisplaySnapshot(snapshot); valid.HasError())
            return Result<RenderDisplaySnapshotDiff>::Failure(valid.ErrorValue());

        RenderDisplaySnapshotDiff diff;
        if (current_) {
            Result<RenderDisplaySnapshotDiff> compared = DiffRenderDisplaySnapshots(*current_, snapshot);
            if (compared.HasError())
                return compared;
            diff = std::move(compared).Value();
        } else {
            diff.currentRevision = snapshot.revision;
            diff.changes.reserve(snapshot.displays.size());
            for (const RenderDisplayProperties &display : snapshot.displays)
                AddChange(diff, display.id, RenderDisplayChangeReason::Added);
        }
        current_ = std::move(snapshot);
        return Result<RenderDisplaySnapshotDiff>::Success(std::move(diff));
    }

    /** @copydoc RenderDisplaySnapshotFeed::Snapshot */
    Result<RenderDisplaySnapshot> RenderDisplaySnapshotFeed::Snapshot() const {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<RenderDisplaySnapshot>::Failure(MakeError(RenderDisplayErrors::ThreadAffinityViolation));
        if (stopped_)
            return Result<RenderDisplaySnapshot>::Failure(MakeError(RenderDisplayErrors::FeedStopped));
        if (!current_)
            return Result<RenderDisplaySnapshot>::Failure(MakeError(RenderDisplayErrors::SnapshotUnavailable));
        return Result<RenderDisplaySnapshot>::Success(*current_);
    }

    /** @copydoc RenderDisplaySnapshotFeed::Stop */
    void RenderDisplaySnapshotFeed::Stop() noexcept {
        stopped_ = true;
        current_.reset();
    }
}  // namespace Horo::Render
