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
            std::size_t maximum = limits.maximumTextBytes;
            if (text.kind == EditorUiTextKind::LocalizationKey)
                maximum = limits.maximumLocalizationKeyBytes;
            else if (technicalMaximum != 0)
                maximum = technicalMaximum;
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

        [[nodiscard]] bool IsContainer(const EditorUiNodeKind kind) noexcept {
            using enum EditorUiNodeKind;
            return kind == Group || kind == Stack || kind == Row || kind == Grid;
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
            return std::visit([]<typename T>(const T typed) noexcept {
                if constexpr (std::is_same_v<std::decay_t<T>, double>)
                    return IsFiniteNumber(typed);
                return true;
            }, value);
        }

        [[nodiscard]] double NumberValue(const std::variant<std::int64_t, double> &value) noexcept {
            return std::visit([]<typename T>(const T typed) noexcept {
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

        [[nodiscard]] Result<void> ValidatePassiveTextNode(const EditorUiNodeBase &base, const EditorUiText &text,
                                                           const EditorUiFormLimits &limits, const std::string_view reason) {
            if (base.focusPolicy != EditorUiFocusPolicy::Never || base.readOnly || !IsValidText(text, limits, true))
                return Invalid(reason);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateTextFieldNode(const EditorUiTextFieldNode &node, const EditorUiFormLimits &limits) {
            if (!HasAccessibleName(node.base) || !IsValidBinding(node.binding, limits) || node.maximumBytes == 0 ||
                node.maximumBytes > limits.maximumTextBytes || !IsValidText(node.placeholder, limits, false, node.maximumBytes) ||
                node.value.size() > node.maximumBytes || !IsValidUtf8ScalarSequence(node.value))
                return Invalid("Text field binding, label, placeholder, or value is invalid.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNumberNode(const EditorUiNumberNode &node, const EditorUiFormLimits &limits) {
            if (!HasAccessibleName(node.base) || !IsValidBinding(node.binding, limits) || !IsKnownNumberKind(node.numberKind) ||
                !IsFiniteNumberValue(node.value))
                return Invalid("Number field binding, label, kind, or value is invalid.");
            if ((node.numberKind == EditorUiNumberKind::Integer && !std::holds_alternative<std::int64_t>(node.value)) ||
                (node.numberKind == EditorUiNumberKind::Decimal && !std::holds_alternative<double>(node.value)))
                return Invalid("Number field value type does not match its declared kind.");
            const auto validOptionalNumber = [&](const std::optional<double> &candidate, const bool integralRequired) {
                return !candidate.has_value() || (IsFiniteNumber(*candidate) && (!integralRequired || IsIntegral(*candidate)));
            };
            if (!validOptionalNumber(node.minimum, node.numberKind == EditorUiNumberKind::Integer) ||
                !validOptionalNumber(node.maximum, node.numberKind == EditorUiNumberKind::Integer) ||
                !validOptionalNumber(node.step, node.numberKind == EditorUiNumberKind::Integer) ||
                (node.step.has_value() && *node.step <= 0.0) ||
                (node.minimum.has_value() && node.maximum.has_value() && *node.minimum > *node.maximum))
                return Invalid("Number field range or step is invalid.");
            const double value = NumberValue(node.value);
            if ((node.minimum.has_value() && value < *node.minimum) || (node.maximum.has_value() && value > *node.maximum))
                return Invalid("Number field value is outside its declared range.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateChoiceNode(const EditorUiChoiceNode &node, const EditorUiFormLimits &limits) {
            if (!HasAccessibleName(node.base) || !IsValidBinding(node.binding, limits) || node.options.empty() ||
                node.options.size() > limits.maximumChoices)
                return Invalid("Choice field binding, label, or option count is invalid.");
            std::vector<std::string> optionIds;
            optionIds.reserve(node.options.size());
            for (const auto &option : node.options) {
                if (option.id.size() > limits.maximumNodeIdentityBytes ||
                    !IsValidText(option.label, limits, true, limits.maximumChoiceLabelBytes))
                    return Invalid("Choice option identity or label is invalid.");
                optionIds.push_back(option.id);
            }
            if (!IsUniqueCanonical(optionIds, limits.maximumNodeIdentityBytes))
                return Invalid("Choice option identities must be unique and canonical.");
            const auto selected = std::ranges::find(optionIds, node.selected);
            if (node.selected.empty() ? !node.allowEmpty : selected == optionIds.end())
                return Invalid("Choice selection is not represented by its options.");
            if (selected != optionIds.end() && !node.options[static_cast<std::size_t>(selected - optionIds.begin())].enabled)
                return Invalid("Choice selection cannot target a disabled option.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePathNode(const EditorUiPathNode &node, const EditorUiFormLimits &limits) {
            if (!HasAccessibleName(node.base) || !IsValidBinding(node.binding, limits) || !IsKnownPathKind(node.pathKind) ||
                node.value.size() > limits.maximumTextBytes || !IsValidUtf8ScalarSequence(node.value))
                return Invalid("Path field binding, label, kind, or value is invalid.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateColorNode(const EditorUiColorNode &node, const EditorUiFormLimits &limits) {
            if (!HasAccessibleName(node.base) || !IsValidBinding(node.binding, limits) || !node.value.IsValid() ||
                (!node.allowAlpha && node.value.alpha != 1.0F))
                return Invalid("Color field binding, label, or value is invalid.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateVectorNode(const EditorUiVectorNode &node, const EditorUiFormLimits &limits) {
            if (!HasAccessibleName(node.base) || !IsValidBinding(node.binding, limits) || !node.value.IsValid())
                return Invalid("Vector field binding, label, or value is invalid.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateActionNode(const EditorUiActionNode &node, const EditorUiFormLimits &limits) {
            if (!HasAccessibleName(node.base) || !IsValidAction(node.action, limits) || !IsKnownActionKind(node.actionKind) ||
                node.base.readOnly)
                return Invalid("Action identity, label, kind, or state is invalid.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateValidationNode(const EditorUiValidationNode &node, const EditorUiFormLimits &limits) {
            if (node.base.focusPolicy != EditorUiFocusPolicy::Never || node.base.readOnly || !IsKnownValidationSeverity(node.severity) ||
                !IsCanonicalIdentity(node.code, limits.maximumNodeIdentityBytes) || !IsValidText(node.message, limits, true))
                return Invalid("Validation identity, severity, or message is invalid.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateContainerNode(const EditorUiContainerNode &node) {
            if (node.base.focusPolicy != EditorUiFocusPolicy::Never || node.base.readOnly || !IsKnownLayoutKind(node.layout) ||
                node.columns == 0 || (node.layout == EditorUiLayoutKind::Grid && node.columns > 16) ||
                (node.layout != EditorUiLayoutKind::Grid && node.columns != 1))
                return Invalid("Container layout, columns, or state is invalid.");
            if (node.layout == EditorUiLayoutKind::Group && !HasAccessibleName(node.base))
                return Invalid("Group containers require a localized or accessible title.");
            return Result<void>::Success();
        }

        template <typename T> [[nodiscard]] Result<void> ValidateNodePayload(const T &node, const EditorUiFormLimits &limits) {
            using Node = std::decay_t<T>;
            if constexpr (std::is_same_v<Node, EditorUiLabelNode>)
                return ValidatePassiveTextNode(node.base, node.text, limits, "Label nodes must be non-focusable and carry valid text.");
            else if constexpr (std::is_same_v<Node, EditorUiTextNode>)
                return ValidatePassiveTextNode(node.base, node.text, limits, "Text nodes must be non-focusable and carry valid text.");
            else if constexpr (std::is_same_v<Node, EditorUiTextFieldNode>)
                return ValidateTextFieldNode(node, limits);
            else if constexpr (std::is_same_v<Node, EditorUiNumberNode>)
                return ValidateNumberNode(node, limits);
            else if constexpr (std::is_same_v<Node, EditorUiBooleanNode>)
                return HasAccessibleName(node.base) && IsValidBinding(node.binding, limits)
                           ? Result<void>::Success()
                           : Invalid("Boolean field binding or label is invalid.");
            else if constexpr (std::is_same_v<Node, EditorUiChoiceNode>)
                return ValidateChoiceNode(node, limits);
            else if constexpr (std::is_same_v<Node, EditorUiPathNode>)
                return ValidatePathNode(node, limits);
            else if constexpr (std::is_same_v<Node, EditorUiColorNode>)
                return ValidateColorNode(node, limits);
            else if constexpr (std::is_same_v<Node, EditorUiVectorNode>)
                return ValidateVectorNode(node, limits);
            else if constexpr (std::is_same_v<Node, EditorUiActionNode>)
                return ValidateActionNode(node, limits);
            else if constexpr (std::is_same_v<Node, EditorUiValidationNode>)
                return ValidateValidationNode(node, limits);
            else if constexpr (std::is_same_v<Node, EditorUiContainerNode>)
                return ValidateContainerNode(node);
            else
                return ValidatePassiveTextNode(node.base, node.text, limits, "Help nodes must be non-focusable and carry valid text.");
        }

        [[nodiscard]] Result<void> ValidateNodeInternal(const EditorUiNode &node, const EditorUiFormLimits &limits) {
            return std::visit([&]<typename T>(const T &typed) {
                if (const Result<void> base = ValidateBase(typed.base, limits); base.HasError())
                    return base;
                return ValidateNodePayload(typed, limits);
            }, node.payload);
        }

        [[nodiscard]] const EditorUiNodeBase &BaseOf(const EditorUiNode &node) noexcept {
            return std::visit([]<typename T>(const T &typed) -> const EditorUiNodeBase & {
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

        [[nodiscard]] Result<void> CollectNodeReferences(const EditorUiNode &node, const EditorUiFormLimits &limits,
                                                         std::vector<std::string> &nodeIds, std::vector<std::string> &bindingIds,
                                                         std::vector<std::string> &actionIds, std::size_t &validationCount) {
            const EditorUiNodeBase &base = BaseOf(node);
            if (std::ranges::find(nodeIds, base.id.value) != nodeIds.end())
                return Invalid("Editor UI node identities must be unique within a form.");
            nodeIds.push_back(base.id.value);

            const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
            if (kind == EditorUiNodeKind::Validation && ++validationCount > limits.maximumValidationMessages)
                return CapacityExceeded("Editor UI validation message count exceeds its configured bound.");
            std::visit([&]<typename T>(const T &typed) {
                using Node = std::decay_t<T>;
                if constexpr (requires { typed.binding.value; })
                    bindingIds.push_back(typed.binding.value);
                else if constexpr (std::is_same_v<Node, EditorUiActionNode>)
                    actionIds.push_back(typed.action.value);
            }, node.payload);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNodeParent(const std::vector<EditorUiNode> &nodes, const std::size_t index,
                                                      const EditorUiFormLimits &limits) {
            const EditorUiNodeBase &base = BaseOf(nodes[index]);
            if (base.parent.value.empty())
                return Result<void>::Success();
            const std::size_t parentIndex = FindNodeIndex(nodes, base.parent);
            if (parentIndex >= index)
                return Invalid("Editor UI parents must be declared before their children.");
            if (!ParentAllowsChild(EditorUiNodeKindOf(nodes[parentIndex]), EditorUiNodeKindOf(nodes[index])))
                return Invalid("Editor UI node parent is not a compatible container or annotation owner.");
            if (NodeDepth(nodes, index) > limits.maximumDepth)
                return CapacityExceeded("Editor UI form nesting depth exceeds its configured bound.");
            return Result<void>::Success();
        }

        template <typename T> [[nodiscard]] EditorUiNodeKind NodeKindOfPayload(const T &typed) noexcept {
            using Node = std::decay_t<T>;
            using enum EditorUiNodeKind;
            if constexpr (std::is_same_v<Node, EditorUiLabelNode>)
                return Label;
            else if constexpr (std::is_same_v<Node, EditorUiTextNode>)
                return Text;
            else if constexpr (std::is_same_v<Node, EditorUiTextFieldNode>)
                return TextField;
            else if constexpr (std::is_same_v<Node, EditorUiNumberNode>)
                return Number;
            else if constexpr (std::is_same_v<Node, EditorUiBooleanNode>)
                return Boolean;
            else if constexpr (std::is_same_v<Node, EditorUiChoiceNode>)
                return Choice;
            else if constexpr (std::is_same_v<Node, EditorUiPathNode>)
                return Path;
            else if constexpr (std::is_same_v<Node, EditorUiColorNode>)
                return Color;
            else if constexpr (std::is_same_v<Node, EditorUiVectorNode>)
                return Vector;
            else if constexpr (std::is_same_v<Node, EditorUiActionNode>)
                return Action;
            else if constexpr (std::is_same_v<Node, EditorUiValidationNode>)
                return Validation;
            else if constexpr (std::is_same_v<Node, EditorUiContainerNode>) {
                switch (typed.layout) {
                    case EditorUiLayoutKind::Group:
                        return Group;
                    case EditorUiLayoutKind::Stack:
                        return Stack;
                    case EditorUiLayoutKind::Row:
                        return Row;
                    case EditorUiLayoutKind::Grid:
                        return Grid;
                    default:
                        return Stack;
                }
            } else
                return Help;
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
        return std::visit([]<typename T>(const T &typed) noexcept {
            return NodeKindOfPayload(typed);
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
            if (const Result<void> references = CollectNodeReferences(node, limits, nodeIds, bindingIds, actionIds, validationCount);
                references.HasError())
                return references;
            if (const Result<void> parent = ValidateNodeParent(form.nodes, index, limits); parent.HasError())
                return parent;
        }
        if (!IsUniqueCanonical(bindingIds, limits.maximumBindingIdentityBytes))
            return Invalid("Editor UI value-binding identities must be unique and canonical.");
        if (!IsUniqueCanonical(actionIds, limits.maximumActionIdentityBytes))
            return Invalid("Editor UI action identities must be unique and canonical.");
        return Result<void>::Success();
    }

    /** @copydoc EditorUiFormBuilder::Create */
    Result<EditorUiFormBuilder> EditorUiFormBuilder::Create(EditorUiId id, EditorUiText title, const EditorUiFormLimits &limits) {
        if (!IsValidLimits(limits))
            return Result<EditorUiFormBuilder>::Failure(
                MakeError(ExtensionErrors::EditorUiFormInvalid, "Editor UI form limits are zero, oversized, or otherwise unsupported."));
        EditorUiForm form{.schemaVersion = EditorUiFormSchemaVersion, .id = std::move(id), .title = std::move(title)};
        if (!IsValidIdentity(form.id, limits.maximumFormIdentityBytes) || !IsValidText(form.title, limits, true))
            return Result<EditorUiFormBuilder>::Failure(
                MakeError(ExtensionErrors::EditorUiFormInvalid, "Editor UI form identity or title is invalid."));
        return Result<EditorUiFormBuilder>::Success(EditorUiFormBuilder{std::move(form), limits});
    }

}  // namespace Horo::Extensions
