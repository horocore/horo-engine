#include "Horo/Runtime/Ui/UiVisualAccessibility.h"

#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiRenderSnapshot.h"
#include "Horo/Runtime/Ui/UiStyle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace Horo::Runtime::Ui::UiErrors {
    /** @copydoc VisualAccessibilityInvalid */
    const ErrorCodeDescriptor VisualAccessibilityInvalid{ErrorDomainId{"horo.runtime_ui"},
                                                         ErrorCode{"runtime_ui.visual_accessibility.invalid"},
                                                         ErrorSeverity::Error,
                                                         "UI accessibility preferences or text size are invalid.",
                                                         "Compose UI visual settings and use finite readable sizes within the scale range.",
                                                         false,
                                                         false};
}  // namespace Horo::Runtime::Ui::UiErrors

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Inert UI-owned keys, initialized before frame capture by explicit contribution composition. */
        const std::array<SettingKey, 2> &Keys() {
            static const std::array<SettingKey, 2> keys{SettingKey{"accessibility.visual.ui.ui_scale"},
                                                        SettingKey{"accessibility.visual.ui.high_contrast"}};
            return keys;
        }

        /** @brief Integer cell bounds relative to the interior of a five-cell status badge. */
        struct IndicatorStroke final {
            std::int32_t x{};
            std::int32_t y{};
            std::int32_t width{};
            std::int32_t height{};
        };

        /** @brief Distinct silhouettes in stable order so simultaneous states retain independent meaning. */
        struct IndicatorShape final {
            UiVisualState state;
            std::array<IndicatorStroke, 3> strokes;
        };

        constexpr std::array IndicatorShapes{
            IndicatorShape{UiVisualState::Checked, {{{0, 1, 1, 1}, {1, 2, 1, 1}, {2, 0, 1, 2}}}},
            IndicatorShape{UiVisualState::Selected, {{{1, 1, 1, 1}}}},
            IndicatorShape{UiVisualState::Pressed, {{{0, 0, 3, 1}, {1, 1, 1, 2}}}},
            IndicatorShape{UiVisualState::Disabled, {{{0, 1, 3, 1}}}},
            IndicatorShape{UiVisualState::Invalid, {{{1, 0, 1, 1}, {1, 2, 1, 1}}}},
            IndicatorShape{UiVisualState::Busy, {{{0, 0, 1, 1}, {1, 1, 1, 1}, {2, 2, 1, 1}}}},
        };

        constexpr auto IndicatorStateMask = [] {
            UiVisualStateMask state{UiVisualState::Focused};
            for (const auto &shape : IndicatorShapes)
                state = state | shape.state;
            return state;
        }();

        constexpr auto IndicatorCommandCount = [] {
            unsigned count = UiVisualIndicatorLimits::FocusCommands;
            for (const auto &shape : IndicatorShapes) {
                ++count;
                for (const auto &stroke : shape.strokes)
                    count += stroke.width != 0;
            }
            return count;
        }();
        static_assert(IndicatorCommandCount == UiVisualIndicatorLimits::Commands);

        constexpr UiLinearColor IndicatorBackground{0, 0, 0, 1};
        constexpr UiLinearColor IndicatorForeground{1, 1, 1, 1};
        constexpr std::int32_t IndicatorCellMaximum = 2 * 64;
        constexpr std::int32_t BadgeCells = 5;
        constexpr std::int32_t BadgeStride = BadgeCells + 1;

        /** @brief Internal writer preserving the surface's inherited presentation scopes on every emitted draw. */
        struct IndicatorWriter final {
            const UiDrawCommand &surface;
            std::span<UiDrawCommand> output;
            std::size_t count{};

            void Add(UiLogicalRect rect, UiDrawPayload paint) noexcept {
                auto draw = surface;
                draw.rect = rect;
                draw.opacity = 1.0F;
                draw.payload = paint;
                output[count++] = draw;
            }

            void DrawFocus() noexcept {
                auto [origin, extent] = surface.rect;
                const auto width = std::min({IndicatorCellMaximum / 2, extent.width / 4, extent.height / 4});
                if (width == 0)
                    return;
                Add(surface.rect, UiBorderDraw{IndicatorBackground, width});
                origin.x += width;
                origin.y += width;
                extent.width -= 2 * width;
                extent.height -= 2 * width;
                Add({origin, extent}, UiBorderDraw{IndicatorForeground, width});
            }

            void DrawBadge(const IndicatorShape &shape, const UiLogicalPoint origin, const std::int32_t cell) noexcept {
                Add({origin, {BadgeCells * cell, BadgeCells * cell}}, UiSolidDraw{IndicatorBackground});
                for (const auto &stroke : shape.strokes) {
                    if (stroke.width == 0)
                        break;
                    const UiLogicalPoint point{origin.x + (stroke.x + 1) * cell, origin.y + (stroke.y + 1) * cell};
                    Add({point, {stroke.width * cell, stroke.height * cell}}, UiSolidDraw{IndicatorForeground});
                }
            }
        };
    }  // namespace

    /** @copydoc HasUiVisualIndicators */
    bool HasUiVisualIndicators(const UiVisualStateMask state) noexcept {
        return (state.bits & IndicatorStateMask.bits) != 0;
    }

    /** @copydoc ProjectUiVisualIndicators */
    Result<std::size_t> ProjectUiVisualIndicators(const UiVisualStateMask state, const UiDrawCommand &surface,
                                                  const std::span<UiDrawCommand> output) {
        if (!HasUiVisualIndicators(state) || surface.opacity == 0 || surface.rect.extent.width == 0 || surface.rect.extent.height == 0)
            return Result<std::size_t>::Success(0);
        const bool focused = state.Contains(UiVisualStateMask{UiVisualState::Focused});
        const auto badges = static_cast<std::int32_t>(std::ranges::count_if(IndicatorShapes, [state](const auto &shape) {
            return state.Contains(UiVisualStateMask{shape.state});
        }));
        const auto [origin, extent] = surface.rect;
        const auto right = static_cast<std::int64_t>(origin.x) + extent.width;
        const auto bottom = static_cast<std::int64_t>(origin.y) + extent.height;
        if (!surface.rect.IsValid() || right > std::numeric_limits<std::int32_t>::max() ||
            bottom > std::numeric_limits<std::int32_t>::max())
            return Result<std::size_t>::Failure(MakeError(UiErrors::HandleMalformed));
        if (output.size() < UiVisualIndicatorLimits::Commands)
            return Result<std::size_t>::Failure(MakeError(UiErrors::CapacityExceeded));
        IndicatorWriter writer{surface, output};
        if (focused)
            writer.DrawFocus();
        const auto cell = std::min({IndicatorCellMaximum, extent.width / (badges * BadgeStride + 1), extent.height / 10});
        if (badges == 0 || cell == 0)
            return Result<std::size_t>::Success(writer.count);
        UiLogicalPoint position{static_cast<std::int32_t>(right) - (badges * BadgeStride + 1) * cell, origin.y + 3 * cell};
        for (const auto &shape : IndicatorShapes) {
            if (state.Contains(UiVisualStateMask{shape.state})) {
                writer.DrawBadge(shape, position, cell);
                position.x += BadgeStride * cell;
            }
        }
        return Result<std::size_t>::Success(writer.count);
    }

    /** @copydoc UiVisualPresentationPolicy::ValidateReplacement */
    Result<void> UiVisualPresentationPolicy::ValidateReplacement(const UiVisualPresentationPolicy &current) const {
        if (revision == 0 || !scale.IsValid() || 2 * scale.numerator < scale.denominator || scale.numerator > 3 * scale.denominator ||
            minimumControlTargetDips > MaximumUiAccessibleTargetDips || (revision == current.revision && *this != current))
            return Result<void>::Failure(MakeError(UiErrors::VisualAccessibilityInvalid));
        if (revision < current.revision)
            return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
        return Result<void>::Success();
    }

    /** @copydoc MakeUiVisualAccessibilityContribution */
    ModuleConfigurationContribution MakeUiVisualAccessibilityContribution() {
        ModuleConfigurationContribution contribution{.module = ModuleId{"horo.runtime.ui.accessibility"},
                                                     .ownerPrefix = "accessibility.visual.ui"};
        const auto &keys = Keys();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const SettingValue value = i == 0 ? SettingValue{std::int64_t{1000}} : SettingValue{false};
            contribution.settings.push_back(
                {keys[i], i == 0 ? SettingValueType::Integer : SettingValueType::Boolean, value, SettingScope::User,
                 ReloadPolicy::NextFrame, SettingSensitivity::Public,
                 ConfigurationSourcePolicy{ConfigurationSourceMask::User | ConfigurationSourceMask::Session |
                                               ConfigurationSourceMask::Invocation | ConfigurationSourceMask::PackagedProfile,
                                           false}});
        }
        return contribution;
    }

    /** @copydoc UiVisualAccessibilitySnapshot::Capture */
    Result<UiVisualAccessibilitySnapshot> UiVisualAccessibilitySnapshot::Capture(ConfigurationSnapshot snapshot) {
        const auto &keys = Keys();
        const auto *scaleEntry = snapshot.FindResolved(keys[0]);
        const auto *contrastEntry = snapshot.FindResolved(keys[1]);
        const auto *scale = scaleEntry ? std::get_if<std::int64_t>(&scaleEntry->value) : nullptr;
        const auto *contrast = contrastEntry ? std::get_if<bool>(&contrastEntry->value) : nullptr;
        if (!scale || !contrast || *scale < 500 || *scale > 3000)
            return Result<UiVisualAccessibilitySnapshot>::Failure(MakeError(UiErrors::VisualAccessibilityInvalid));
        const auto divisor = std::gcd(*scale, std::int64_t{1000});
        const UiCanvasScaleFactor multiplier{static_cast<std::uint32_t>(*scale / divisor), static_cast<std::uint32_t>(1000 / divisor)};
        return Result<UiVisualAccessibilitySnapshot>::Success(UiVisualAccessibilitySnapshot{std::move(snapshot), multiplier, *contrast});
    }

    /** @copydoc UiVisualAccessibilitySnapshot::UiVisualAccessibilitySnapshot */
    UiVisualAccessibilitySnapshot::UiVisualAccessibilitySnapshot(ConfigurationSnapshot snapshot, const UiCanvasScaleFactor scale,
                                                                 const bool highContrast) noexcept
        : snapshot_(std::move(snapshot)), scale_(scale), highContrast_(highContrast) {}

    /** @copydoc UiVisualAccessibilitySnapshot::UiScale */
    float UiVisualAccessibilitySnapshot::UiScale() const noexcept {
        return static_cast<float>(scale_.numerator) / static_cast<float>(scale_.denominator);
    }

    /** @copydoc UiVisualAccessibilitySnapshot::ScaleFactor */
    UiCanvasScaleFactor UiVisualAccessibilitySnapshot::ScaleFactor() const noexcept {
        return scale_;
    }

    /** @copydoc UiVisualAccessibilitySnapshot::HighContrast */
    bool UiVisualAccessibilitySnapshot::HighContrast() const noexcept {
        return highContrast_;
    }

    /** @copydoc UiVisualAccessibilitySnapshot::Revision */
    ConfigurationRevision UiVisualAccessibilitySnapshot::Revision() const noexcept {
        return snapshot_.Revision();
    }

    /** @copydoc UiVisualAccessibilitySnapshot::TextSize */
    Result<float> UiVisualAccessibilitySnapshot::TextSize(const float authoredDips) const {
        if (!std::isfinite(authoredDips) || authoredDips <= 0 || authoredDips > std::numeric_limits<float>::max() / 3)
            return Result<float>::Failure(MakeError(UiErrors::VisualAccessibilityInvalid));
        return Result<float>::Success(std::max(16.0F, authoredDips * UiScale()));
    }
}  // namespace Horo::Runtime::Ui
