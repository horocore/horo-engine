#include "Horo/Extensions/EditorUiForm.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Extensions {
    namespace {
        constexpr std::size_t HardMaximumIdentityBytes = 1024;
        constexpr std::size_t HardMaximumTextBytes = 64U * 1024U;
        constexpr std::size_t HardMaximumNodes = 4096;
        constexpr std::size_t HardMaximumDepth = 64;
        constexpr std::size_t HardMaximumChoices = 4096;

        [[nodiscard]] Result<void> Invalid(const std::string_view reason) {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid, std::string{reason}));
        }

        [[nodiscard]] Result<void> CapacityExceeded(const std::string_view reason) {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorUiFormCapacityExceeded, std::string{reason}));
        }

        [[nodiscard]] Result<void> ThemeInvalid(const std::string_view reason) {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorUiThemeInvalid, std::string{reason}));
        }

        [[nodiscard]] bool IsKnownTextKind(const EditorUiTextKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(EditorUiTextKind::TechnicalText);
        }

        [[nodiscard]] bool IsKnownComponentSize(const EditorUiComponentSize size) noexcept {
            return static_cast<std::uint8_t>(size) <= static_cast<std::uint8_t>(EditorUiComponentSize::Large);
        }

        [[nodiscard]] bool IsKnownTone(const EditorUiSemanticTone tone) noexcept {
            return static_cast<std::uint8_t>(tone) <= static_cast<std::uint8_t>(EditorUiSemanticTone::Critical);
        }

        [[nodiscard]] bool IsKnownFocusPolicy(const EditorUiFocusPolicy policy) noexcept {
            return static_cast<std::uint8_t>(policy) <= static_cast<std::uint8_t>(EditorUiFocusPolicy::Never);
        }

        [[nodiscard]] bool IsKnownValidationSeverity(const EditorUiValidationSeverity severity) noexcept {
            return static_cast<std::uint8_t>(severity) <= static_cast<std::uint8_t>(EditorUiValidationSeverity::Error);
        }

        [[nodiscard]] bool IsKnownNumberKind(const EditorUiNumberKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(EditorUiNumberKind::Decimal);
        }

        [[nodiscard]] bool IsKnownPathKind(const EditorUiPathKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(EditorUiPathKind::Directory);
        }

        [[nodiscard]] bool IsKnownActionKind(const EditorUiActionKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(EditorUiActionKind::Destructive);
        }

        [[nodiscard]] bool IsKnownLayoutKind(const EditorUiLayoutKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(EditorUiLayoutKind::Grid);
        }

        [[nodiscard]] bool IsCanonicalIdentity(const std::string_view value, const std::size_t maximumBytes) noexcept {
            if (value.empty() || value.size() > maximumBytes || value.front() == '.' || value.back() == '.')
                return false;

            bool previousDot = false;
            for (const unsigned char character : value) {
                if (character == '.') {
                    if (previousDot)
                        return false;
                    previousDot = true;
                    continue;
                }
                if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' ||
                      character == '-'))
                    return false;
                previousDot = false;
            }
            return !previousDot;
        }

        [[nodiscard]] bool IsCanonicalLocalizationKey(const std::string_view value, const std::size_t maximumBytes) noexcept {
            return IsCanonicalIdentity(value, maximumBytes) && value.find('.') != std::string_view::npos;
        }

        [[nodiscard]] bool IsValidLimits(const EditorUiFormLimits &limits) noexcept {
            return limits.maximumFormIdentityBytes != 0 && limits.maximumFormIdentityBytes <= HardMaximumIdentityBytes &&
                   limits.maximumNodeIdentityBytes != 0 && limits.maximumNodeIdentityBytes <= HardMaximumIdentityBytes &&
                   limits.maximumBindingIdentityBytes != 0 && limits.maximumBindingIdentityBytes <= HardMaximumIdentityBytes &&
                   limits.maximumActionIdentityBytes != 0 && limits.maximumActionIdentityBytes <= HardMaximumIdentityBytes &&
                   limits.maximumLocalizationKeyBytes != 0 && limits.maximumLocalizationKeyBytes <= HardMaximumTextBytes &&
                   limits.maximumTextBytes != 0 && limits.maximumTextBytes <= HardMaximumTextBytes && limits.maximumNodes != 0 &&
                   limits.maximumNodes <= HardMaximumNodes && limits.maximumDepth != 0 && limits.maximumDepth <= HardMaximumDepth &&
                   limits.maximumChoices != 0 && limits.maximumChoices <= HardMaximumChoices && limits.maximumChoiceLabelBytes != 0 &&
                   limits.maximumChoiceLabelBytes <= HardMaximumTextBytes && limits.maximumValidationMessages != 0 &&
                   limits.maximumValidationMessages <= HardMaximumNodes;
        }

        [[nodiscard]] bool IsValidText(const EditorUiText &text, const EditorUiFormLimits &limits, const bool required,
                                       const std::size_t technicalMaximum = 0) noexcept {
            if (!IsKnownTextKind(text.kind))
                return false;
            if (text.value.empty())
                return !required;
            const std::size_t maximum = text.kind == EditorUiTextKind::LocalizationKey
                                            ? limits.maximumLocalizationKeyBytes
                                            : (technicalMaximum == 0 ? limits.maximumTextBytes : technicalMaximum);
            if (text.value.size() > maximum || !IsValidUtf8ScalarSequence(text.value))
                return false;
            if (text.kind == EditorUiTextKind::LocalizationKey)
                return IsCanonicalLocalizationKey(text.value, maximum);
            return true;
        }

        [[nodiscard]] bool IsValidIdentity(const EditorUiId &id, const std::size_t maximumBytes, const bool required = true) noexcept {
            return id.value.empty() ? !required : IsCanonicalIdentity(id.value, maximumBytes);
        }

        [[nodiscard]] bool IsValidBinding(const EditorUiBindingId &binding, const EditorUiFormLimits &limits) noexcept {
            return IsCanonicalIdentity(binding.value, limits.maximumBindingIdentityBytes);
        }

        [[nodiscard]] bool IsValidAction(const EditorUiActionId &action, const EditorUiFormLimits &limits) noexcept {
            return IsCanonicalIdentity(action.value, limits.maximumActionIdentityBytes);
        }

        [[nodiscard]] bool HasAccessibleName(const EditorUiNodeBase &base) noexcept {
            return !base.label.value.empty() || !base.accessibleLabel.value.empty();
        }

        [[nodiscard]] bool IsInteractive(const EditorUiNodeKind kind) noexcept {
            switch (kind) {
                case EditorUiNodeKind::TextField:
                case EditorUiNodeKind::Number:
                case EditorUiNodeKind::Boolean:
                case EditorUiNodeKind::Choice:
                case EditorUiNodeKind::Path:
                case EditorUiNodeKind::Color:
                case EditorUiNodeKind::Vector:
                case EditorUiNodeKind::Action:
                    return true;
                case EditorUiNodeKind::Label:
                case EditorUiNodeKind::Text:
                case EditorUiNodeKind::Validation:
                case EditorUiNodeKind::Group:
                case EditorUiNodeKind::Stack:
                case EditorUiNodeKind::Row:
                case EditorUiNodeKind::Grid:
                case EditorUiNodeKind::Help:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool IsContainer(const EditorUiNodeKind kind) noexcept {
            return kind == EditorUiNodeKind::Group || kind == EditorUiNodeKind::Stack || kind == EditorUiNodeKind::Row ||
                   kind == EditorUiNodeKind::Grid;
        }

        [[nodiscard]] Result<void> ValidateBase(const EditorUiNodeBase &base, const EditorUiFormLimits &limits) {
            if (!IsValidIdentity(base.id, limits.maximumNodeIdentityBytes) ||
                !IsValidIdentity(base.parent, limits.maximumNodeIdentityBytes, false))
                return Invalid("Editor UI node identity or parent is not canonical.");
            if (!IsValidText(base.label, limits, false) || !IsValidText(base.description, limits, false) ||
                !IsValidText(base.accessibleLabel, limits, false))
                return Invalid("Editor UI node text is invalid or oversized.");
            if (!IsKnownComponentSize(base.size) || !IsKnownTone(base.tone) || !IsKnownFocusPolicy(base.focusPolicy))
                return Invalid("Editor UI node presentation state is unsupported.");
            if (base.id == base.parent)
                return Invalid("An editor UI node cannot parent itself.");
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsFiniteNumber(const double value) noexcept {
            return std::isfinite(value);
        }

        [[nodiscard]] bool IsFiniteNumberValue(const std::variant<std::int64_t, double> &value) noexcept {
            return std::visit([](const auto typed) noexcept {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<T, double>)
                    return IsFiniteNumber(typed);
                return true;
            }, value);
        }

        [[nodiscard]] double NumberValue(const std::variant<std::int64_t, double> &value) noexcept {
            return std::visit([](const auto typed) noexcept {
                return static_cast<double>(typed);
            }, value);
        }

        [[nodiscard]] bool IsIntegral(const double value) noexcept {
            return std::isfinite(value) && std::floor(value) == value;
        }

        [[nodiscard]] bool IsUniqueCanonical(const std::vector<std::string> &values, const std::size_t maximumBytes) noexcept {
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (!IsCanonicalIdentity(values[index], maximumBytes))
                    return false;
                if (std::ranges::find(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values[index]) !=
                    values.begin() + static_cast<std::ptrdiff_t>(index))
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<void> ValidateNodeInternal(const EditorUiNode &node, const EditorUiFormLimits &limits) {
            return std::visit([&](const auto &typed) -> Result<void> {
                using T = std::decay_t<decltype(typed)>;
                if (const Result<void> base = ValidateBase(typed.base, limits); base.HasError())
                    return base;
                const EditorUiNodeKind kind = EditorUiNodeKindOf(node);

                if constexpr (std::is_same_v<T, EditorUiLabelNode>) {
                    if (typed.base.focusPolicy != EditorUiFocusPolicy::Never || typed.base.readOnly ||
                        !IsValidText(typed.text, limits, true))
                        return Invalid("Label nodes must be non-focusable and carry valid text.");
                } else if constexpr (std::is_same_v<T, EditorUiTextNode>) {
                    if (typed.base.focusPolicy != EditorUiFocusPolicy::Never || typed.base.readOnly ||
                        !IsValidText(typed.text, limits, true))
                        return Invalid("Text nodes must be non-focusable and carry valid text.");
                } else if constexpr (std::is_same_v<T, EditorUiTextFieldNode>) {
                    if (!HasAccessibleName(typed.base) || !IsValidBinding(typed.binding, limits) || typed.maximumBytes == 0 ||
                        typed.maximumBytes > limits.maximumTextBytes ||
                        !IsValidText(typed.placeholder, limits, false, typed.maximumBytes) || typed.value.size() > typed.maximumBytes ||
                        !IsValidUtf8ScalarSequence(typed.value))
                        return Invalid("Text field binding, label, placeholder, or value is invalid.");
                } else if constexpr (std::is_same_v<T, EditorUiNumberNode>) {
                    if (!HasAccessibleName(typed.base) || !IsValidBinding(typed.binding, limits) || !IsKnownNumberKind(typed.numberKind) ||
                        !IsFiniteNumberValue(typed.value))
                        return Invalid("Number field binding, label, kind, or value is invalid.");
                    if ((typed.numberKind == EditorUiNumberKind::Integer && !std::holds_alternative<std::int64_t>(typed.value)) ||
                        (typed.numberKind == EditorUiNumberKind::Decimal && !std::holds_alternative<double>(typed.value)))
                        return Invalid("Number field value type does not match its declared kind.");
                    const auto validOptionalNumber = [&](const std::optional<double> &candidate, const bool integralRequired) {
                        return !candidate.has_value() || (IsFiniteNumber(*candidate) && (!integralRequired || IsIntegral(*candidate)));
                    };
                    if (!validOptionalNumber(typed.minimum, typed.numberKind == EditorUiNumberKind::Integer) ||
                        !validOptionalNumber(typed.maximum, typed.numberKind == EditorUiNumberKind::Integer) ||
                        !validOptionalNumber(typed.step, typed.numberKind == EditorUiNumberKind::Integer) ||
                        (typed.step.has_value() && *typed.step <= 0.0) ||
                        (typed.minimum.has_value() && typed.maximum.has_value() && *typed.minimum > *typed.maximum))
                        return Invalid("Number field range or step is invalid.");
                    const double value = NumberValue(typed.value);
                    if ((typed.minimum.has_value() && value < *typed.minimum) || (typed.maximum.has_value() && value > *typed.maximum))
                        return Invalid("Number field value is outside its declared range.");
                } else if constexpr (std::is_same_v<T, EditorUiBooleanNode>) {
                    if (!HasAccessibleName(typed.base) || !IsValidBinding(typed.binding, limits))
                        return Invalid("Boolean field binding or label is invalid.");
                } else if constexpr (std::is_same_v<T, EditorUiChoiceNode>) {
                    if (!HasAccessibleName(typed.base) || !IsValidBinding(typed.binding, limits) || typed.options.empty() ||
                        typed.options.size() > limits.maximumChoices)
                        return Invalid("Choice field binding, label, or option count is invalid.");
                    std::vector<std::string> optionIds;
                    optionIds.reserve(typed.options.size());
                    for (const auto &option : typed.options) {
                        if (option.id.size() > limits.maximumNodeIdentityBytes ||
                            !IsValidText(option.label, limits, true, limits.maximumChoiceLabelBytes))
                            return Invalid("Choice option identity or label is invalid.");
                        optionIds.push_back(option.id);
                    }
                    if (!IsUniqueCanonical(optionIds, limits.maximumNodeIdentityBytes))
                        return Invalid("Choice option identities must be unique and canonical.");
                    const auto selected = std::ranges::find(optionIds, typed.selected);
                    if (typed.selected.empty() ? !typed.allowEmpty : selected == optionIds.end())
                        return Invalid("Choice selection is not represented by its options.");
                    if (selected != optionIds.end() && !typed.options[static_cast<std::size_t>(selected - optionIds.begin())].enabled)
                        return Invalid("Choice selection cannot target a disabled option.");
                } else if constexpr (std::is_same_v<T, EditorUiPathNode>) {
                    if (!HasAccessibleName(typed.base) || !IsValidBinding(typed.binding, limits) || !IsKnownPathKind(typed.pathKind) ||
                        typed.value.size() > limits.maximumTextBytes || !IsValidUtf8ScalarSequence(typed.value))
                        return Invalid("Path field binding, label, kind, or value is invalid.");
                } else if constexpr (std::is_same_v<T, EditorUiColorNode>) {
                    if (!HasAccessibleName(typed.base) || !IsValidBinding(typed.binding, limits) || !typed.value.IsValid() ||
                        (!typed.allowAlpha && typed.value.alpha != 1.0F))
                        return Invalid("Color field binding, label, or value is invalid.");
                } else if constexpr (std::is_same_v<T, EditorUiVectorNode>) {
                    if (!HasAccessibleName(typed.base) || !IsValidBinding(typed.binding, limits) || !typed.value.IsValid())
                        return Invalid("Vector field binding, label, or value is invalid.");
                } else if constexpr (std::is_same_v<T, EditorUiActionNode>) {
                    if (!HasAccessibleName(typed.base) || !IsValidAction(typed.action, limits) || !IsKnownActionKind(typed.actionKind) ||
                        typed.base.readOnly)
                        return Invalid("Action identity, label, kind, or state is invalid.");
                } else if constexpr (std::is_same_v<T, EditorUiValidationNode>) {
                    if (typed.base.focusPolicy != EditorUiFocusPolicy::Never || typed.base.readOnly ||
                        !IsKnownValidationSeverity(typed.severity) || !IsCanonicalIdentity(typed.code, limits.maximumNodeIdentityBytes) ||
                        !IsValidText(typed.message, limits, true))
                        return Invalid("Validation identity, severity, or message is invalid.");
                } else if constexpr (std::is_same_v<T, EditorUiContainerNode>) {
                    if (typed.base.focusPolicy != EditorUiFocusPolicy::Never || typed.base.readOnly || !IsKnownLayoutKind(typed.layout) ||
                        typed.columns == 0 || (typed.layout == EditorUiLayoutKind::Grid && typed.columns > 16) ||
                        (typed.layout != EditorUiLayoutKind::Grid && typed.columns != 1))
                        return Invalid("Container layout, columns, or state is invalid.");
                    if (typed.layout == EditorUiLayoutKind::Group && !HasAccessibleName(typed.base))
                        return Invalid("Group containers require a localized or accessible title.");
                } else if constexpr (std::is_same_v<T, EditorUiHelpNode>) {
                    if (typed.base.focusPolicy != EditorUiFocusPolicy::Never || typed.base.readOnly ||
                        !IsValidText(typed.text, limits, true))
                        return Invalid("Help nodes must be non-focusable and carry valid text.");
                }

                static_cast<void>(kind);
                return Result<void>::Success();
            }, node.payload);
        }

        [[nodiscard]] const EditorUiNodeBase &BaseOf(const EditorUiNode &node) noexcept {
            return std::visit([](const auto &typed) -> const EditorUiNodeBase & {
                return typed.base;
            }, node.payload);
        }

        [[nodiscard]] std::size_t FindNodeIndex(const std::vector<EditorUiNode> &nodes, const EditorUiId &id) noexcept {
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                if (BaseOf(nodes[index]).id == id)
                    return index;
            }
            return nodes.size();
        }

        [[nodiscard]] bool ParentAllowsChild(const EditorUiNodeKind parent, const EditorUiNodeKind child) noexcept {
            if (IsContainer(parent))
                return true;
            return child == EditorUiNodeKind::Validation || child == EditorUiNodeKind::Help;
        }

        [[nodiscard]] std::size_t NodeDepth(const std::vector<EditorUiNode> &nodes, std::size_t index) noexcept {
            std::size_t depth = 0;
            EditorUiId parent = BaseOf(nodes[index]).parent;
            while (!parent.value.empty()) {
                const std::size_t parentIndex = FindNodeIndex(nodes, parent);
                if (parentIndex >= nodes.size() || parentIndex >= index)
                    return std::numeric_limits<std::size_t>::max();
                ++depth;
                parent = BaseOf(nodes[parentIndex]).parent;
            }
            return depth;
        }

        [[nodiscard]] EditorUiThemeToken ToneToken(const EditorUiSemanticTone tone) noexcept {
            switch (tone) {
                case EditorUiSemanticTone::Neutral:
                case EditorUiSemanticTone::Secondary:
                    return EditorUiThemeToken::Border;
                case EditorUiSemanticTone::Primary:
                    return EditorUiThemeToken::Accent;
                case EditorUiSemanticTone::Positive:
                    return EditorUiThemeToken::Positive;
                case EditorUiSemanticTone::Warning:
                    return EditorUiThemeToken::Warning;
                case EditorUiSemanticTone::Critical:
                    return EditorUiThemeToken::Critical;
            }
            return EditorUiThemeToken::None;
        }

        [[nodiscard]] EditorUiThemeToken ValidationToken(const EditorUiValidationSeverity severity) noexcept {
            switch (severity) {
                case EditorUiValidationSeverity::Info:
                    return EditorUiThemeToken::Accent;
                case EditorUiValidationSeverity::Warning:
                    return EditorUiThemeToken::Warning;
                case EditorUiValidationSeverity::Error:
                    return EditorUiThemeToken::Critical;
            }
            return EditorUiThemeToken::None;
        }

        [[nodiscard]] float ControlHeight(const EditorUiComponentSize size, const EditorUiThemeMetrics &metrics) noexcept {
            switch (size) {
                case EditorUiComponentSize::Small:
                    return metrics.smallControlHeight;
                case EditorUiComponentSize::Medium:
                    return metrics.mediumControlHeight;
                case EditorUiComponentSize::Large:
                    return metrics.largeControlHeight;
            }
            return metrics.mediumControlHeight;
        }

        [[nodiscard]] float NodeHeight(const EditorUiNodeKind kind, const EditorUiComponentSize size,
                                       const EditorUiThemeMetrics &metrics) noexcept {
            switch (kind) {
                case EditorUiNodeKind::TextField:
                case EditorUiNodeKind::Number:
                case EditorUiNodeKind::Boolean:
                case EditorUiNodeKind::Choice:
                case EditorUiNodeKind::Path:
                case EditorUiNodeKind::Color:
                case EditorUiNodeKind::Vector:
                case EditorUiNodeKind::Action:
                    return ControlHeight(size, metrics);
                case EditorUiNodeKind::Label:
                case EditorUiNodeKind::Text:
                case EditorUiNodeKind::Validation:
                case EditorUiNodeKind::Group:
                case EditorUiNodeKind::Stack:
                case EditorUiNodeKind::Row:
                case EditorUiNodeKind::Grid:
                case EditorUiNodeKind::Help:
                    return metrics.textLineHeight;
            }
            return metrics.textLineHeight;
        }

        [[nodiscard]] bool IsValidMetric(const float value) noexcept {
            return std::isfinite(value) && value > 0.0F;
        }

        [[nodiscard]] Result<void> ValidateTheme(const EditorUiThemeFrame &theme) {
            constexpr std::uint32_t knownTokenMask =
                EditorUiThemeTokenBit(EditorUiThemeToken::Critical) | (EditorUiThemeTokenBit(EditorUiThemeToken::Critical) - 1U);
            if (theme.schemaVersion != EditorUiFormSchemaVersion || theme.revision == 0 || !std::isfinite(theme.uiScale) ||
                theme.uiScale <= 0.0F || theme.uiScale > 8.0F || (theme.supportedTokenMask & ~knownTokenMask) != 0U ||
                !IsValidMetric(theme.metrics.smallControlHeight) || !IsValidMetric(theme.metrics.mediumControlHeight) ||
                !IsValidMetric(theme.metrics.largeControlHeight) || !IsValidMetric(theme.metrics.textLineHeight) ||
                !IsValidMetric(theme.metrics.rowGap) || !IsValidMetric(theme.metrics.defaultWidth))
                return ThemeInvalid("Editor UI theme frame has an unsupported schema, scale, token set, or metric.");
            if (theme.metrics.smallControlHeight > theme.metrics.mediumControlHeight ||
                theme.metrics.mediumControlHeight > theme.metrics.largeControlHeight)
                return ThemeInvalid("Editor UI theme control heights must be ordered from small to large.");
            return Result<void>::Success();
        }

        [[nodiscard]] EditorUiThemeToken ResolveToken(const EditorUiThemeToken requested, const EditorUiThemeFrame &frame) noexcept {
            if (requested == EditorUiThemeToken::None)
                return EditorUiThemeToken::None;
            if ((frame.supportedTokenMask & EditorUiThemeTokenBit(requested)) != 0U)
                return requested;

            const std::array<EditorUiThemeToken, 2> fallbacks = [&] {
                switch (requested) {
                    case EditorUiThemeToken::SurfaceSubtle:
                        return std::array{EditorUiThemeToken::Surface, EditorUiThemeToken::None};
                    case EditorUiThemeToken::TextDisabled:
                        return std::array{EditorUiThemeToken::TextSecondary, EditorUiThemeToken::TextPrimary};
                    case EditorUiThemeToken::Border:
                    case EditorUiThemeToken::Focus:
                        return std::array{EditorUiThemeToken::Accent, EditorUiThemeToken::TextSecondary};
                    case EditorUiThemeToken::Positive:
                    case EditorUiThemeToken::Warning:
                    case EditorUiThemeToken::Critical:
                    case EditorUiThemeToken::Accent:
                        return std::array{EditorUiThemeToken::Accent, EditorUiThemeToken::TextPrimary};
                    case EditorUiThemeToken::TextSecondary:
                        return std::array{EditorUiThemeToken::TextPrimary, EditorUiThemeToken::None};
                    case EditorUiThemeToken::Surface:
                    case EditorUiThemeToken::TextPrimary:
                    case EditorUiThemeToken::None:
                        return std::array{EditorUiThemeToken::None, EditorUiThemeToken::None};
                }
                return std::array{EditorUiThemeToken::None, EditorUiThemeToken::None};
            }();
            for (const auto fallback : fallbacks) {
                if (fallback != EditorUiThemeToken::None && (frame.supportedTokenMask & EditorUiThemeTokenBit(fallback)) != 0U)
                    return fallback;
            }
            return EditorUiThemeToken::None;
        }

        struct ResolvedStyle final {
            EditorUiThemeToken foreground{EditorUiThemeToken::TextPrimary};
            EditorUiThemeToken background{EditorUiThemeToken::None};
            EditorUiThemeToken border{EditorUiThemeToken::None};
        };

        [[nodiscard]] ResolvedStyle StyleForNode(const EditorUiNode &node, const EditorUiThemeFrame &theme) noexcept {
            const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
            const EditorUiNodeBase &base = BaseOf(node);
            ResolvedStyle style;
            if (kind == EditorUiNodeKind::Help)
                style.foreground = EditorUiThemeToken::TextSecondary;
            else if (kind == EditorUiNodeKind::Validation)
                style.foreground = std::get<EditorUiValidationNode>(node.payload).severity == EditorUiValidationSeverity::Info
                                       ? EditorUiThemeToken::TextSecondary
                                       : ValidationToken(std::get<EditorUiValidationNode>(node.payload).severity);
            else if (IsInteractive(kind)) {
                style.background = EditorUiThemeToken::Surface;
                if (kind == EditorUiNodeKind::Action) {
                    const auto actionKind = std::get<EditorUiActionNode>(node.payload).actionKind;
                    style.border =
                        actionKind == EditorUiActionKind::Primary
                            ? EditorUiThemeToken::Accent
                            : (actionKind == EditorUiActionKind::Destructive ? EditorUiThemeToken::Critical : EditorUiThemeToken::Border);
                } else {
                    style.border = EditorUiThemeToken::Border;
                }
            } else if (IsContainer(kind)) {
                style.background = kind == EditorUiNodeKind::Group ? EditorUiThemeToken::SurfaceSubtle : EditorUiThemeToken::None;
            }
            if (base.tone != EditorUiSemanticTone::Neutral && kind != EditorUiNodeKind::Validation)
                style.border = ToneToken(base.tone);
            if (!base.enabled) {
                style.foreground = EditorUiThemeToken::TextDisabled;
                style.background = EditorUiThemeToken::SurfaceSubtle;
            }
            style.foreground = ResolveToken(style.foreground, theme);
            style.background = ResolveToken(style.background, theme);
            style.border = ResolveToken(style.border, theme);
            return style;
        }

        [[nodiscard]] std::size_t SiblingIndex(const std::vector<EditorUiNode> &nodes, const std::size_t nodeIndex) noexcept {
            const EditorUiId parent = BaseOf(nodes[nodeIndex]).parent;
            std::size_t siblingIndex = 0;
            for (std::size_t index = 0; index < nodeIndex; ++index) {
                if (BaseOf(nodes[index]).parent == parent)
                    ++siblingIndex;
            }
            return siblingIndex;
        }

        [[nodiscard]] std::size_t SiblingCount(const std::vector<EditorUiNode> &nodes, const EditorUiId &parent) noexcept {
            return static_cast<std::size_t>(std::ranges::count_if(nodes, [&](const EditorUiNode &node) {
                return BaseOf(node).parent == parent;
            }));
        }

        [[nodiscard]] std::pair<float, float> ResponsiveWidthAndOffset(const std::vector<EditorUiNode> &nodes, const std::size_t nodeIndex,
                                                                       const float width) noexcept {
            const EditorUiId parent = BaseOf(nodes[nodeIndex]).parent;
            if (parent.value.empty())
                return {width, 0.0F};
            const std::size_t parentIndex = FindNodeIndex(nodes, parent);
            if (parentIndex >= nodes.size())
                return {width, 0.0F};
            const EditorUiNodeKind parentKind = EditorUiNodeKindOf(nodes[parentIndex]);
            const std::size_t siblings = std::max<std::size_t>(SiblingCount(nodes, parent), 1U);
            std::size_t columns = 1;
            if (parentKind == EditorUiNodeKind::Row)
                columns = siblings;
            else if (parentKind == EditorUiNodeKind::Grid)
                columns = std::max<std::size_t>(std::get<EditorUiContainerNode>(nodes[parentIndex].payload).columns, 1U);
            if (columns == 1)
                return {width, 0.0F};
            const float cellWidth = width / static_cast<float>(columns);
            const std::size_t column = SiblingIndex(nodes, nodeIndex) % columns;
            return {cellWidth, cellWidth * static_cast<float>(column)};
        }
    }  // namespace

    /** @copydoc EditorUiColorValue::IsValid */
    bool EditorUiColorValue::IsValid() const noexcept {
        return std::isfinite(red) && std::isfinite(green) && std::isfinite(blue) && std::isfinite(alpha) && red >= 0.0F && red <= 1.0F &&
               green >= 0.0F && green <= 1.0F && blue >= 0.0F && blue <= 1.0F && alpha >= 0.0F && alpha <= 1.0F;
    }

    /** @copydoc EditorUiVectorValue::IsValid */
    bool EditorUiVectorValue::IsValid() const noexcept {
        if (dimension < 2 || dimension > MaximumEditorUiVectorComponents)
            return false;
        for (std::size_t index = 0; index < dimension; ++index) {
            if (!std::isfinite(components[index]))
                return false;
        }
        return true;
    }

    /** @copydoc EditorUiNodeKindOf */
    EditorUiNodeKind EditorUiNodeKindOf(const EditorUiNode &node) noexcept {
        return std::visit([](const auto &typed) noexcept {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, EditorUiLabelNode>)
                return EditorUiNodeKind::Label;
            else if constexpr (std::is_same_v<T, EditorUiTextNode>)
                return EditorUiNodeKind::Text;
            else if constexpr (std::is_same_v<T, EditorUiTextFieldNode>)
                return EditorUiNodeKind::TextField;
            else if constexpr (std::is_same_v<T, EditorUiNumberNode>)
                return EditorUiNodeKind::Number;
            else if constexpr (std::is_same_v<T, EditorUiBooleanNode>)
                return EditorUiNodeKind::Boolean;
            else if constexpr (std::is_same_v<T, EditorUiChoiceNode>)
                return EditorUiNodeKind::Choice;
            else if constexpr (std::is_same_v<T, EditorUiPathNode>)
                return EditorUiNodeKind::Path;
            else if constexpr (std::is_same_v<T, EditorUiColorNode>)
                return EditorUiNodeKind::Color;
            else if constexpr (std::is_same_v<T, EditorUiVectorNode>)
                return EditorUiNodeKind::Vector;
            else if constexpr (std::is_same_v<T, EditorUiActionNode>)
                return EditorUiNodeKind::Action;
            else if constexpr (std::is_same_v<T, EditorUiValidationNode>)
                return EditorUiNodeKind::Validation;
            else if constexpr (std::is_same_v<T, EditorUiContainerNode>) {
                switch (typed.layout) {
                    case EditorUiLayoutKind::Group:
                        return EditorUiNodeKind::Group;
                    case EditorUiLayoutKind::Stack:
                        return EditorUiNodeKind::Stack;
                    case EditorUiLayoutKind::Row:
                        return EditorUiNodeKind::Row;
                    case EditorUiLayoutKind::Grid:
                        return EditorUiNodeKind::Grid;
                }
                return EditorUiNodeKind::Stack;
            } else
                return EditorUiNodeKind::Help;
        }, node.payload);
    }

    /** @copydoc EditorUiNodeBaseOf */
    const EditorUiNodeBase &EditorUiNodeBaseOf(const EditorUiNode &node) noexcept {
        return BaseOf(node);
    }

    /** @copydoc ValidateEditorUiNode */
    Result<void> ValidateEditorUiNode(const EditorUiNode &node, const EditorUiFormLimits &limits) {
        if (!IsValidLimits(limits))
            return Invalid("Editor UI form limits are zero, oversized, or otherwise unsupported.");
        return ValidateNodeInternal(node, limits);
    }

    /** @copydoc ValidateEditorUiForm */
    Result<void> ValidateEditorUiForm(const EditorUiForm &form, const EditorUiFormLimits &limits) {
        if (!IsValidLimits(limits))
            return Invalid("Editor UI form limits are zero, oversized, or otherwise unsupported.");
        if (form.schemaVersion != EditorUiFormSchemaVersion || !IsValidIdentity(form.id, limits.maximumFormIdentityBytes) ||
            !IsValidText(form.title, limits, true) || !IsValidText(form.description, limits, false))
            return Invalid("Editor UI form schema, identity, title, or description is invalid.");
        if (form.nodes.empty() || form.nodes.size() > limits.maximumNodes)
            return CapacityExceeded("Editor UI form node count is empty or exceeds its configured bound.");

        std::vector<std::string> nodeIds;
        std::vector<std::string> bindingIds;
        std::vector<std::string> actionIds;
        nodeIds.reserve(form.nodes.size());
        std::size_t validationCount = 0;
        for (std::size_t index = 0; index < form.nodes.size(); ++index) {
            const EditorUiNode &node = form.nodes[index];
            if (const Result<void> validation = ValidateNodeInternal(node, limits); validation.HasError())
                return validation;
            const EditorUiNodeBase &base = BaseOf(node);
            if (std::ranges::find(nodeIds, base.id.value) != nodeIds.end())
                return Invalid("Editor UI node identities must be unique within a form.");
            nodeIds.push_back(base.id.value);

            const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
            if (kind == EditorUiNodeKind::Validation) {
                ++validationCount;
                if (validationCount > limits.maximumValidationMessages)
                    return CapacityExceeded("Editor UI validation message count exceeds its configured bound.");
            }
            if (kind == EditorUiNodeKind::TextField)
                bindingIds.push_back(std::get<EditorUiTextFieldNode>(node.payload).binding.value);
            else if (kind == EditorUiNodeKind::Number)
                bindingIds.push_back(std::get<EditorUiNumberNode>(node.payload).binding.value);
            else if (kind == EditorUiNodeKind::Boolean)
                bindingIds.push_back(std::get<EditorUiBooleanNode>(node.payload).binding.value);
            else if (kind == EditorUiNodeKind::Choice)
                bindingIds.push_back(std::get<EditorUiChoiceNode>(node.payload).binding.value);
            else if (kind == EditorUiNodeKind::Path)
                bindingIds.push_back(std::get<EditorUiPathNode>(node.payload).binding.value);
            else if (kind == EditorUiNodeKind::Color)
                bindingIds.push_back(std::get<EditorUiColorNode>(node.payload).binding.value);
            else if (kind == EditorUiNodeKind::Vector)
                bindingIds.push_back(std::get<EditorUiVectorNode>(node.payload).binding.value);
            else if (kind == EditorUiNodeKind::Action)
                actionIds.push_back(std::get<EditorUiActionNode>(node.payload).action.value);

            if (!base.parent.value.empty()) {
                const std::size_t parentIndex = FindNodeIndex(form.nodes, base.parent);
                if (parentIndex >= index)
                    return Invalid("Editor UI parents must be declared before their children.");
                if (!ParentAllowsChild(EditorUiNodeKindOf(form.nodes[parentIndex]), kind))
                    return Invalid("Editor UI node parent is not a compatible container or annotation owner.");
                if (NodeDepth(form.nodes, index) > limits.maximumDepth)
                    return CapacityExceeded("Editor UI form nesting depth exceeds its configured bound.");
            }
        }
        if (!IsUniqueCanonical(bindingIds, limits.maximumBindingIdentityBytes))
            return Invalid("Editor UI value-binding identities must be unique and canonical.");
        if (!IsUniqueCanonical(actionIds, limits.maximumActionIdentityBytes))
            return Invalid("Editor UI action identities must be unique and canonical.");
        return Result<void>::Success();
    }

    /** @copydoc EditorUiFormBuilder::Create */
    Result<EditorUiFormBuilder> EditorUiFormBuilder::Create(EditorUiId id, EditorUiText title, const EditorUiFormLimits limits) {
        if (!IsValidLimits(limits))
            return Result<EditorUiFormBuilder>::Failure(
                MakeError(ExtensionErrors::EditorUiFormInvalid, "Editor UI form limits are zero, oversized, or otherwise unsupported."));
        EditorUiForm form{.schemaVersion = EditorUiFormSchemaVersion, .id = std::move(id), .title = std::move(title)};
        if (!IsValidIdentity(form.id, limits.maximumFormIdentityBytes) || !IsValidText(form.title, limits, true))
            return Result<EditorUiFormBuilder>::Failure(
                MakeError(ExtensionErrors::EditorUiFormInvalid, "Editor UI form identity or title is invalid."));
        return Result<EditorUiFormBuilder>::Success(EditorUiFormBuilder{std::move(form), limits});
    }

    /** @copydoc EditorUiFormBuilder::EditorUiFormBuilder */
    EditorUiFormBuilder::EditorUiFormBuilder(EditorUiForm form, const EditorUiFormLimits limits) noexcept
        : form_(std::move(form)), limits_(limits) {}

    /** @copydoc EditorUiFormBuilder::Add */
    Result<void> EditorUiFormBuilder::Add(EditorUiNode node) {
        if (form_.nodes.size() >= limits_.maximumNodes)
            return CapacityExceeded("Editor UI form node capacity is exhausted.");
        if (const Result<void> validation = ValidateNodeInternal(node, limits_); validation.HasError())
            return validation;
        const EditorUiNodeBase &base = BaseOf(node);
        if (std::ranges::find_if(form_.nodes, [&](const EditorUiNode &existing) {
            return BaseOf(existing).id == base.id;
        }) != form_.nodes.end())
            return Invalid("Editor UI node identities must be unique within a form.");
        try {
            form_.nodes.push_back(std::move(node));
        } catch (const std::bad_alloc &) {
            return CapacityExceeded("Editor UI form node storage could not be extended.");
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorUiFormBuilder::AddLabel */
    Result<void> EditorUiFormBuilder::AddLabel(EditorUiLabelNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddText */
    Result<void> EditorUiFormBuilder::AddText(EditorUiTextNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddTextField */
    Result<void> EditorUiFormBuilder::AddTextField(EditorUiTextFieldNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddNumber */
    Result<void> EditorUiFormBuilder::AddNumber(EditorUiNumberNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddBoolean */
    Result<void> EditorUiFormBuilder::AddBoolean(EditorUiBooleanNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddChoice */
    Result<void> EditorUiFormBuilder::AddChoice(EditorUiChoiceNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddPath */
    Result<void> EditorUiFormBuilder::AddPath(EditorUiPathNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddColor */
    Result<void> EditorUiFormBuilder::AddColor(EditorUiColorNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddVector */
    Result<void> EditorUiFormBuilder::AddVector(EditorUiVectorNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddAction */
    Result<void> EditorUiFormBuilder::AddAction(EditorUiActionNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddValidation */
    Result<void> EditorUiFormBuilder::AddValidation(EditorUiValidationNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddContainer */
    Result<void> EditorUiFormBuilder::AddContainer(EditorUiContainerNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddHelp */
    Result<void> EditorUiFormBuilder::AddHelp(EditorUiHelpNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::Build */
    Result<EditorUiForm> EditorUiFormBuilder::Build() && {
        if (const Result<void> validation = ValidateEditorUiForm(form_, limits_); validation.HasError())
            return Result<EditorUiForm>::Failure(std::move(validation).ErrorValue());
        return Result<EditorUiForm>::Success(std::move(form_));
    }

    /** @copydoc ResolveEditorUiThemeToken */
    EditorUiThemeToken ResolveEditorUiThemeToken(const EditorUiThemeToken requested, const EditorUiThemeFrame &frame) noexcept {
        return ResolveToken(requested, frame);
    }

    /** @copydoc BuildEditorUiRenderSnapshot */
    Result<EditorUiRenderSnapshot> BuildEditorUiRenderSnapshot(const EditorUiForm &form, const EditorUiThemeFrame &theme,
                                                               const float availableWidth) {
        if (const Result<void> validation = ValidateEditorUiForm(form); validation.HasError())
            return Result<EditorUiRenderSnapshot>::Failure(std::move(validation).ErrorValue());
        if (const Result<void> validation = ValidateTheme(theme); validation.HasError())
            return Result<EditorUiRenderSnapshot>::Failure(std::move(validation).ErrorValue());
        const float logicalWidth = availableWidth == 0.0F ? theme.metrics.defaultWidth : availableWidth;
        if (!std::isfinite(logicalWidth) || logicalWidth <= 0.0F || logicalWidth > 32'768.0F)
            return Result<EditorUiRenderSnapshot>::Failure(
                MakeError(ExtensionErrors::EditorUiThemeInvalid, "Editor UI available width is outside the finite adapter bound."));

        EditorUiRenderSnapshot snapshot{.schemaVersion = EditorUiFormSchemaVersion,
                                        .form = form.id,
                                        .themeRevision = theme.revision,
                                        .uiScale = theme.uiScale,
                                        .availableWidth = logicalWidth * theme.uiScale};
        try {
            snapshot.nodes.reserve(form.nodes.size());
        } catch (const std::bad_alloc &) {
            return Result<EditorUiRenderSnapshot>::Failure(
                MakeError(ExtensionErrors::EditorUiFormCapacityExceeded, "Editor UI render snapshot storage could not be reserved."));
        }

        float cursorY = 0.0F;
        std::uint32_t focusOrder = 0;
        for (std::size_t index = 0; index < form.nodes.size(); ++index) {
            const EditorUiNode &node = form.nodes[index];
            const EditorUiNodeBase &base = BaseOf(node);
            const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
            const auto [logicalNodeWidth, logicalNodeX] = ResponsiveWidthAndOffset(form.nodes, index, logicalWidth);
            const float logicalHeight = NodeHeight(kind, base.size, theme.metrics);
            const ResolvedStyle style = StyleForNode(node, theme);
            EditorUiRenderNode renderNode{.id = base.id,
                                          .parent = base.parent,
                                          .kind = kind,
                                          .size = base.size,
                                          .tone = base.tone,
                                          .foregroundToken = style.foreground,
                                          .backgroundToken = style.background,
                                          .borderToken = style.border,
                                          .enabled = base.enabled,
                                          .readOnly = base.readOnly,
                                          .focusable =
                                              base.enabled && base.focusPolicy == EditorUiFocusPolicy::Automatic && IsInteractive(kind),
                                          .focusOrder = 0,
                                          .x = logicalNodeX * theme.uiScale,
                                          .y = cursorY,
                                          .width = logicalNodeWidth * theme.uiScale,
                                          .height = logicalHeight * theme.uiScale};
            if (renderNode.focusable)
                renderNode.focusOrder = ++focusOrder;
            snapshot.nodes.push_back(std::move(renderNode));
            cursorY += (logicalHeight + theme.metrics.rowGap) * theme.uiScale;
        }
        return Result<EditorUiRenderSnapshot>::Success(std::move(snapshot));
    }
}  // namespace Horo::Extensions
