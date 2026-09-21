#include "Horo/Runtime/Ui/UiAccessibility.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnownRole(const UiAccessibilityRole role) noexcept {
            switch (role) {
                case UiAccessibilityRole::Application:
                case UiAccessibilityRole::Window:
                case UiAccessibilityRole::Screen:
                case UiAccessibilityRole::Dialog:
                case UiAccessibilityRole::Alert:
                case UiAccessibilityRole::Group:
                case UiAccessibilityRole::Heading:
                case UiAccessibilityRole::StaticText:
                case UiAccessibilityRole::Button:
                case UiAccessibilityRole::Toggle:
                case UiAccessibilityRole::Checkbox:
                case UiAccessibilityRole::Radio:
                case UiAccessibilityRole::Slider:
                case UiAccessibilityRole::TextField:
                case UiAccessibilityRole::Link:
                case UiAccessibilityRole::Image:
                case UiAccessibilityRole::Progress:
                case UiAccessibilityRole::List:
                case UiAccessibilityRole::ListItem:
                case UiAccessibilityRole::Menu:
                case UiAccessibilityRole::MenuItem:
                case UiAccessibilityRole::Tab:
                case UiAccessibilityRole::TabItem:
                case UiAccessibilityRole::Tree:
                case UiAccessibilityRole::TreeItem:
                case UiAccessibilityRole::Table:
                case UiAccessibilityRole::Row:
                case UiAccessibilityRole::Cell:
                case UiAccessibilityRole::ScrollView:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnownSource(const UiAccessibilityControlSource source) noexcept {
            return source == UiAccessibilityControlSource::Core || source == UiAccessibilityControlSource::Contributed;
        }

        [[nodiscard]] bool IsKnownTextSource(const UiAccessibilityTextSource source) noexcept {
            return source == UiAccessibilityTextSource::ResolvedMessage || source == UiAccessibilityTextSource::UserContent;
        }

        [[nodiscard]] bool IsKnownExposure(const UiAccessibilityExposure exposure) noexcept {
            switch (exposure) {
                case UiAccessibilityExposure::Visible:
                case UiAccessibilityExposure::Offscreen:
                case UiAccessibilityExposure::Hidden:
                case UiAccessibilityExposure::Covered:
                case UiAccessibilityExposure::Suppressed:
                case UiAccessibilityExposure::Suspended:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnownValueKind(const UiAccessibilityValueKind kind) noexcept {
            switch (kind) {
                case UiAccessibilityValueKind::None:
                case UiAccessibilityValueKind::Boolean:
                case UiAccessibilityValueKind::Integer:
                case UiAccessibilityValueKind::Number:
                case UiAccessibilityValueKind::Text:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnownSelectionMode(const UiAccessibilitySelectionMode mode) noexcept {
            return mode == UiAccessibilitySelectionMode::None || mode == UiAccessibilitySelectionMode::Single ||
                   mode == UiAccessibilitySelectionMode::Multiple;
        }

        [[nodiscard]] bool IsKnownErrorKind(const UiAccessibilityErrorKind kind) noexcept {
            switch (kind) {
                case UiAccessibilityErrorKind::None:
                case UiAccessibilityErrorKind::Invalid:
                case UiAccessibilityErrorKind::Required:
                case UiAccessibilityErrorKind::Range:
                case UiAccessibilityErrorKind::Pattern:
                case UiAccessibilityErrorKind::Custom:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnownRelationKind(const UiAccessibilityRelationKind kind) noexcept {
            switch (kind) {
                case UiAccessibilityRelationKind::LabelledBy:
                case UiAccessibilityRelationKind::DescribedBy:
                case UiAccessibilityRelationKind::Controls:
                case UiAccessibilityRelationKind::Owns:
                case UiAccessibilityRelationKind::ActiveDescendant:
                case UiAccessibilityRelationKind::ErrorMessage:
                case UiAccessibilityRelationKind::FlowTo:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnownActionKind(const UiAccessibilityActionKind kind) noexcept {
            switch (kind) {
                case UiAccessibilityActionKind::Focus:
                case UiAccessibilityActionKind::Activate:
                case UiAccessibilityActionKind::Increment:
                case UiAccessibilityActionKind::Decrement:
                case UiAccessibilityActionKind::SetValue:
                case UiAccessibilityActionKind::SetText:
                case UiAccessibilityActionKind::ScrollForward:
                case UiAccessibilityActionKind::ScrollBackward:
                case UiAccessibilityActionKind::ScrollTo:
                case UiAccessibilityActionKind::Expand:
                case UiAccessibilityActionKind::Collapse:
                case UiAccessibilityActionKind::Select:
                case UiAccessibilityActionKind::ClearSelection:
                case UiAccessibilityActionKind::Dismiss:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnownActionValueKind(const UiAccessibilityActionValueKind kind) noexcept {
            switch (kind) {
                case UiAccessibilityActionValueKind::None:
                case UiAccessibilityActionValueKind::Boolean:
                case UiAccessibilityActionValueKind::Integer:
                case UiAccessibilityActionValueKind::Number:
                case UiAccessibilityActionValueKind::Text:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool RequiresName(const UiAccessibilityRole role) noexcept {
            switch (role) {
                case UiAccessibilityRole::Group:
                case UiAccessibilityRole::List:
                case UiAccessibilityRole::Menu:
                case UiAccessibilityRole::Tab:
                case UiAccessibilityRole::Tree:
                case UiAccessibilityRole::ScrollView:
                    return false;
                default:
                    return true;
            }
        }

        [[nodiscard]] bool AllowsRange(const UiAccessibilityRole role) noexcept {
            return role == UiAccessibilityRole::Slider || role == UiAccessibilityRole::Progress;
        }

        [[nodiscard]] bool AllowsSelection(const UiAccessibilityRole role) noexcept {
            switch (role) {
                case UiAccessibilityRole::Radio:
                case UiAccessibilityRole::ListItem:
                case UiAccessibilityRole::MenuItem:
                case UiAccessibilityRole::TabItem:
                case UiAccessibilityRole::TreeItem:
                case UiAccessibilityRole::Row:
                case UiAccessibilityRole::Cell:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool AllowsExpanded(const UiAccessibilityRole role) noexcept {
            switch (role) {
                case UiAccessibilityRole::Group:
                case UiAccessibilityRole::List:
                case UiAccessibilityRole::Menu:
                case UiAccessibilityRole::Tab:
                case UiAccessibilityRole::Tree:
                case UiAccessibilityRole::TreeItem:
                case UiAccessibilityRole::Row:
                case UiAccessibilityRole::Cell:
                case UiAccessibilityRole::ScrollView:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool AllowsScroll(const UiAccessibilityRole role) noexcept {
            return role == UiAccessibilityRole::ScrollView || role == UiAccessibilityRole::List || role == UiAccessibilityRole::Tree ||
                   role == UiAccessibilityRole::Table;
        }

        [[nodiscard]] bool AllowsActivate(const UiAccessibilityRole role) noexcept {
            switch (role) {
                case UiAccessibilityRole::Button:
                case UiAccessibilityRole::Toggle:
                case UiAccessibilityRole::Checkbox:
                case UiAccessibilityRole::Radio:
                case UiAccessibilityRole::Link:
                case UiAccessibilityRole::ListItem:
                case UiAccessibilityRole::MenuItem:
                case UiAccessibilityRole::TabItem:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool AllowsDismiss(const UiAccessibilityRole role) noexcept {
            return role == UiAccessibilityRole::Window || role == UiAccessibilityRole::Screen || role == UiAccessibilityRole::Dialog ||
                   role == UiAccessibilityRole::Alert;
        }

        [[nodiscard]] bool AllowsChecked(const UiAccessibilityRole role) noexcept {
            return role == UiAccessibilityRole::Toggle || role == UiAccessibilityRole::Checkbox || role == UiAccessibilityRole::Radio;
        }

        [[nodiscard]] bool AllowsPressed(const UiAccessibilityRole role) noexcept {
            return role == UiAccessibilityRole::Button || role == UiAccessibilityRole::Toggle;
        }

        [[nodiscard]] bool AllowsSelected(const UiAccessibilityRole role) noexcept {
            return AllowsSelection(role);
        }

        [[nodiscard]] bool AllowsValue(const UiAccessibilityRole role) noexcept {
            return role == UiAccessibilityRole::Toggle || role == UiAccessibilityRole::Checkbox || role == UiAccessibilityRole::Radio ||
                   role == UiAccessibilityRole::Slider || role == UiAccessibilityRole::TextField || role == UiAccessibilityRole::Progress;
        }

        [[nodiscard]] bool AllowsReadOnly(const UiAccessibilityRole role) noexcept {
            return AllowsValue(role);
        }

        [[nodiscard]] bool AllowsRequired(const UiAccessibilityRole role) noexcept {
            return role == UiAccessibilityRole::TextField || role == UiAccessibilityRole::Slider;
        }

        [[nodiscard]] bool AllowsMultiSelectable(const UiAccessibilityRole role) noexcept {
            return role == UiAccessibilityRole::List || role == UiAccessibilityRole::Tree || role == UiAccessibilityRole::Table;
        }

        [[nodiscard]] bool AllowsPopup(const UiAccessibilityRole role) noexcept {
            return role == UiAccessibilityRole::Button || role == UiAccessibilityRole::Toggle || role == UiAccessibilityRole::MenuItem;
        }

        [[nodiscard]] bool IsValueCompatible(const UiAccessibilityRole role, const UiAccessibilityValueKind kind) noexcept {
            if (kind == UiAccessibilityValueKind::None)
                return true;
            switch (role) {
                case UiAccessibilityRole::Toggle:
                case UiAccessibilityRole::Checkbox:
                case UiAccessibilityRole::Radio:
                    return kind == UiAccessibilityValueKind::Boolean;
                case UiAccessibilityRole::Slider:
                case UiAccessibilityRole::Progress:
                    return kind == UiAccessibilityValueKind::Integer || kind == UiAccessibilityValueKind::Number;
                case UiAccessibilityRole::TextField:
                    return kind == UiAccessibilityValueKind::Text;
                default:
                    return false;
            }
        }

        struct ProjectionLookupEntry final {
            UiElementId element;
            std::uint32_t index{};
        };

        using ProjectionLookup = std::span<const ProjectionLookupEntry>;

        [[nodiscard]] Result<void> BuildProjectionLookup(const std::span<const UiAccessibilityNodeInput> nodes,
                                                         std::vector<ProjectionLookupEntry> &lookup) {
            lookup.clear();
            for (std::size_t index = 0; index < nodes.size(); ++index)
                lookup.push_back({nodes[index].element, static_cast<std::uint32_t>(index)});
            std::ranges::sort(lookup, {}, &ProjectionLookupEntry::element);
            if (std::ranges::adjacent_find(lookup, [](const ProjectionLookupEntry &left, const ProjectionLookupEntry &right) {
                return left.element == right.element;
            }) != lookup.end())
                return Failure<void>(UiErrors::AccessibilitySchemaInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] std::size_t FindProjectionIndex(const ProjectionLookup lookup, const UiElementId element) noexcept {
            const auto found = std::lower_bound(lookup.begin(), lookup.end(), element,
                                                [](const ProjectionLookupEntry &entry, const UiElementId candidate) {
                return entry.element < candidate;
            });
            return found != lookup.end() && found->element == element ? found->index : std::numeric_limits<std::size_t>::max();
        }

        [[nodiscard]] bool HasLabelRelation(const UiAccessibilityNodeInput &node) noexcept {
            return std::ranges::any_of(node.relations, [](const UiAccessibilityRelationInput &relation) {
                return relation.kind == UiAccessibilityRelationKind::LabelledBy;
            });
        }

        [[nodiscard]] Result<void> ValidateText(const UiAccessibilityTextInput &text, const bool allowEmpty = true) {
            if (!IsKnownTextSource(text.source) || (!allowEmpty && text.text.empty()) ||
                text.text.size() > MaximumUiAccessibilityTextBytes || !IsValidUtf8ScalarSequence(text.text))
                return Failure<void>(UiErrors::AccessibilityTextInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateValue(const UiAccessibilityValueInput &value) {
            if (!IsKnownValueKind(value.kind))
                return Failure<void>(UiErrors::AccessibilityValueInvalid);
            switch (value.kind) {
                case UiAccessibilityValueKind::None:
                    return Result<void>::Success();
                case UiAccessibilityValueKind::Boolean:
                case UiAccessibilityValueKind::Integer:
                    return Result<void>::Success();
                case UiAccessibilityValueKind::Number:
                    return std::isfinite(value.number) ? Result<void>::Success() : Failure<void>(UiErrors::AccessibilityValueInvalid);
                case UiAccessibilityValueKind::Text:
                    return ValidateText(value.text);
            }
            return Failure<void>(UiErrors::AccessibilityValueInvalid);
        }

        [[nodiscard]] Result<void> ValidateError(const UiAccessibilityErrorInput &error) {
            if (!IsKnownErrorKind(error.kind))
                return Failure<void>(UiErrors::AccessibilitySchemaInvalid);
            return ValidateText(error.message, error.kind == UiAccessibilityErrorKind::None);
        }

        [[nodiscard]] Result<void> ValidateActionArgument(const UiAccessibilityActionInput &action) {
            using ActionKind = UiAccessibilityActionKind;
            using ValueKind = UiAccessibilityActionValueKind;
            bool argumentValid{};
            switch (action.kind) {
                case ActionKind::SetValue:
                    argumentValid = action.argumentKind == ValueKind::Integer || action.argumentKind == ValueKind::Number;
                    break;
                case ActionKind::SetText:
                    argumentValid = action.argumentKind == ValueKind::Text;
                    break;
                case ActionKind::ScrollTo:
                    argumentValid = action.argumentKind == ValueKind::Integer || action.argumentKind == ValueKind::Number;
                    break;
                default:
                    argumentValid = action.argumentKind == ValueKind::None;
                    break;
            }
            if (!argumentValid)
                return Failure<void>(UiErrors::AccessibilityActionInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateActionAdmission(const UiAccessibilityActionInput &action, const UiAccessibilityNodeInput &node) {
            using ActionKind = UiAccessibilityActionKind;
            bool admitted = false;
            switch (action.kind) {
                case ActionKind::Focus:
                    admitted = node.state.Has(UiAccessibilityStateFlag::Focusable);
                    break;
                case ActionKind::Activate:
                    admitted = AllowsActivate(node.role);
                    break;
                case ActionKind::Increment:
                case ActionKind::Decrement:
                    admitted = node.role == UiAccessibilityRole::Slider && node.hasRange;
                    break;
                case ActionKind::SetValue:
                    admitted = node.role == UiAccessibilityRole::Slider && node.hasRange;
                    break;
                case ActionKind::SetText:
                    admitted = node.role == UiAccessibilityRole::TextField && node.state.Has(UiAccessibilityStateFlag::Editable);
                    break;
                case ActionKind::ScrollForward:
                case ActionKind::ScrollBackward:
                    admitted = AllowsScroll(node.role);
                    break;
                case ActionKind::ScrollTo:
                    admitted = AllowsScroll(node.role);
                    break;
                case ActionKind::Expand:
                case ActionKind::Collapse:
                    admitted = AllowsExpanded(node.role);
                    break;
                case ActionKind::Select:
                case ActionKind::ClearSelection:
                    admitted = node.hasSelection && AllowsSelection(node.role);
                    break;
                case ActionKind::Dismiss:
                    admitted = AllowsDismiss(node.role);
                    break;
            }
            return admitted ? Result<void>::Success() : Failure<void>(UiErrors::AccessibilityActionInvalid);
        }

        [[nodiscard]] Result<void> ValidateAction(const UiAccessibilityActionInput &action, const UiAccessibilityNodeInput &node) {
            if (!action.id.IsValid() || !IsKnownActionKind(action.kind) || !IsKnownActionValueKind(action.argumentKind))
                return Failure<void>(UiErrors::AccessibilityActionInvalid);
            if (const auto name = ValidateText(action.name); name.HasError())
                return name;
            if (const auto argument = ValidateActionArgument(action); argument.HasError())
                return argument;
            return ValidateActionAdmission(action, node);
        }

        [[nodiscard]] Result<void> ValidateNodeStateCompatibility(const UiAccessibilityNodeInput &node) {
            if (node.state.Has(UiAccessibilityStateFlag::Checked) && !AllowsChecked(node.role))
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::Pressed) && !AllowsPressed(node.role))
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::Selected) && !AllowsSelected(node.role))
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::Selected) && !node.hasSelection)
                return Failure<void>(UiErrors::AccessibilitySelectionInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::Expanded) && !AllowsExpanded(node.role))
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::Editable) && node.role != UiAccessibilityRole::TextField)
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::ReadOnly) && !AllowsReadOnly(node.role))
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::Required) && !AllowsRequired(node.role))
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::MultiSelectable) && !AllowsMultiSelectable(node.role))
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::HasPopup) && !AllowsPopup(node.role))
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::Focused) && !node.state.Has(UiAccessibilityStateFlag::Focusable))
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.state.Has(UiAccessibilityStateFlag::Invalid) && node.error.kind == UiAccessibilityErrorKind::None)
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNode(const UiAccessibilityNodeInput &node) {
            if (!node.element.IsValid() || !IsKnownSource(node.source) || !IsKnownExposure(node.exposure) || !node.bounds.IsValid())
                return Failure<void>(UiErrors::AccessibilitySchemaInvalid);
            if (!IsKnownRole(node.role))
                return Failure<void>(UiErrors::AccessibilityRoleInvalid);
            if (node.source == UiAccessibilityControlSource::Core ? node.contributor.IsValid() : !node.contributor.IsValid())
                return Failure<void>(UiErrors::AccessibilityContributorInvalid);
            if (const auto text = ValidateText(node.name); text.HasError())
                return text;
            if (const auto text = ValidateText(node.description); text.HasError())
                return text;
            if (const auto value = ValidateValue(node.value); value.HasError())
                return value;
            if (const auto error = ValidateError(node.error); error.HasError())
                return error;
            if (!node.state.IsValid())
                return Failure<void>(UiErrors::AccessibilityStateInvalid);
            if (node.hasRange && (!AllowsRange(node.role) || !node.range.IsValid()))
                return Failure<void>(UiErrors::AccessibilityRangeInvalid);
            if (node.role == UiAccessibilityRole::Slider && !node.hasRange)
                return Failure<void>(UiErrors::AccessibilityRangeInvalid);
            if (node.hasSelection && (!AllowsSelection(node.role) || !node.selection.IsValid()))
                return Failure<void>(UiErrors::AccessibilitySelectionInvalid);
            if (const auto state = ValidateNodeStateCompatibility(node); state.HasError())
                return state;
            if (node.value.kind != UiAccessibilityValueKind::None &&
                (!AllowsValue(node.role) || !IsValueCompatible(node.role, node.value.kind)))
                return Failure<void>(UiErrors::AccessibilityValueInvalid);
            if (RequiresName(node.role) && node.name.text.empty() && !HasLabelRelation(node))
                return Failure<void>(UiErrors::AccessibilityNameMissing);
            for (const auto &action : node.actions)
                if (const auto validation = ValidateAction(action, node); validation.HasError())
                    return validation;
            for (std::size_t index = 0; index < node.actions.size(); ++index)
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (node.actions[index].id == node.actions[previous].id)
                        return Failure<void>(UiErrors::AccessibilityActionInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRelations(const std::span<const UiAccessibilityNodeInput> nodes, const ProjectionLookup lookup) {
            for (const auto &node : nodes) {
                for (const auto &relation : node.relations) {
                    if (!IsKnownRelationKind(relation.kind) || !relation.target.IsValid() ||
                        FindProjectionIndex(lookup, relation.target) == std::numeric_limits<std::size_t>::max())
                        return Failure<void>(UiErrors::AccessibilityRelationInvalid);
                }
                for (std::size_t index = 0; index < node.relations.size(); ++index)
                    for (std::size_t previous = 0; previous < index; ++previous)
                        if (node.relations[index].kind == node.relations[previous].kind &&
                            node.relations[index].target == node.relations[previous].target)
                            return Failure<void>(UiErrors::AccessibilityRelationInvalid);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool HasRelationCycle(const std::size_t nodeIndex, const std::span<const UiAccessibilityNodeInput> nodes,
                                            const ProjectionLookup lookup, std::vector<std::uint8_t> &colors) {
            if (colors[nodeIndex] == 1)
                return true;
            if (colors[nodeIndex] == 2)
                return false;
            colors[nodeIndex] = 1;
            for (const auto &relation : nodes[nodeIndex].relations) {
                const auto target = FindProjectionIndex(lookup, relation.target);
                if (target != std::numeric_limits<std::size_t>::max() && HasRelationCycle(target, nodes, lookup, colors))
                    return true;
            }
            colors[nodeIndex] = 2;
            return false;
        }

        struct ProjectionTotals final {
            std::size_t relations{};
            std::size_t actions{};
            std::size_t textBytes{};
            std::size_t focused{};
        };

        [[nodiscard]] Result<ProjectionTotals> ValidateAndMeasureNodes(const UiElementTree &tree,
                                                                       const UiAccessibilityProjection &projection) {
            ProjectionTotals totals;
            for (const auto &node : projection.nodes) {
                if (const auto validation = ValidateNode(node); validation.HasError())
                    return Result<ProjectionTotals>::Failure(validation.ErrorValue());
                if (tree.Find(node.element).HasError())
                    return Result<ProjectionTotals>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
                totals.relations += node.relations.size();
                totals.actions += node.actions.size();
                totals.textBytes += node.name.text.size() + node.description.text.size() + node.error.message.text.size();
                if (node.value.kind == UiAccessibilityValueKind::Text)
                    totals.textBytes += node.value.text.text.size();
                for (const auto &action : node.actions)
                    totals.textBytes += action.name.text.size();
                if (node.state.Has(UiAccessibilityStateFlag::Focused))
                    ++totals.focused;
            }
            return Result<ProjectionTotals>::Success(totals);
        }

        [[nodiscard]] Result<void> ValidateProjection(const UiElementTree &tree, const UiAccessibilitySnapshotDescriptor &descriptor,
                                                      const UiAccessibilityProjection &projection,
                                                      const UiAccessibilityExtractorDescriptor &owner,
                                                      const UiAccessibilitySemanticRevision lastRevision,
                                                      std::vector<std::uint8_t> &cycleScratch,
                                                      std::vector<ProjectionLookupEntry> &lookupScratch) {
            if (!descriptor.IsValid() || descriptor.limits.nodes > owner.limits.nodes ||
                descriptor.limits.relations > owner.limits.relations || descriptor.limits.actions > owner.limits.actions ||
                descriptor.limits.textBytes > owner.limits.textBytes)
                return Failure<void>(UiErrors::AccessibilitySnapshotInvalid);
            if (descriptor.instance != owner.instance || descriptor.canvas != owner.canvas || descriptor.document != owner.document)
                return Failure<void>(UiErrors::AccessibilitySnapshotSourceStale);
            if (tree.State() != UiElementTreeState::Active || tree.Instance() != descriptor.instance ||
                tree.Canvas() != descriptor.canvas || tree.SourceDocument() != descriptor.document ||
                tree.SourceDocumentRevision() != descriptor.documentRevision || tree.Revision() != descriptor.treeRevision)
                return Failure<void>(UiErrors::AccessibilitySnapshotSourceStale);
            if (lastRevision.IsValid() && descriptor.semanticRevision.Compare(lastRevision) != UiRevisionRelation::Newer)
                return Failure<void>(UiErrors::AccessibilitySnapshotSourceStale);
            if (projection.nodes.size() > descriptor.limits.nodes || projection.nodes.size() > MaximumUiAccessibilityNodes)
                return Failure<void>(UiErrors::CapacityExceeded);

            if (const auto lookup = BuildProjectionLookup(projection.nodes, lookupScratch); lookup.HasError())
                return lookup;
            const auto totalsResult = ValidateAndMeasureNodes(tree, projection);
            if (totalsResult.HasError())
                return Result<void>::Failure(totalsResult.ErrorValue());
            const auto &totals = totalsResult.Value();
            if (totals.relations > descriptor.limits.relations || totals.relations > MaximumUiAccessibilityRelations ||
                totals.actions > descriptor.limits.actions || totals.actions > MaximumUiAccessibilityActions ||
                totals.textBytes > descriptor.limits.textBytes || totals.textBytes > MaximumUiAccessibilityTextBytes)
                return Failure<void>(UiErrors::CapacityExceeded);
            if (totals.focused > 1)
                return Failure<void>(UiErrors::AccessibilityFocusConflict);
            if (const auto relations = ValidateRelations(projection.nodes, lookupScratch); relations.HasError())
                return relations;
            cycleScratch.assign(projection.nodes.size(), 0);
            for (std::size_t index = 0; index < projection.nodes.size(); ++index)
                if (HasRelationCycle(index, projection.nodes, lookupScratch, cycleScratch))
                    return Failure<void>(UiErrors::AccessibilityRelationInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] UiAccessibilityNodeId MakeNodeId(const UiElementHandle handle) noexcept {
            return {handle.ownership, handle.slot, handle.generation};
        }

        [[nodiscard]] UiAccessibilityTextRef CopyText(std::vector<char> &destination, const UiAccessibilityTextInput source) {
            if (source.text.empty())
                return {};
            const auto offset = static_cast<std::uint32_t>(destination.size());
            destination.insert(destination.end(), source.text.begin(), source.text.end());
            return {offset, static_cast<std::uint32_t>(source.text.size()), source.source};
        }

        [[nodiscard]] UiAccessibilityValue CopyValue(std::vector<char> &text, const UiAccessibilityValueInput source) {
            UiAccessibilityValue value;
            value.kind = source.kind;
            value.boolean = source.boolean;
            value.integer = source.integer;
            value.number = source.number;
            if (source.kind == UiAccessibilityValueKind::Text)
                value.text = CopyText(text, source.text);
            return value;
        }

        [[nodiscard]] UiAccessibilityActionValueKind ActionArgumentKind(const UiAccessibilityValueKind kind) noexcept {
            switch (kind) {
                case UiAccessibilityValueKind::None:
                    return UiAccessibilityActionValueKind::None;
                case UiAccessibilityValueKind::Boolean:
                    return UiAccessibilityActionValueKind::Boolean;
                case UiAccessibilityValueKind::Integer:
                    return UiAccessibilityActionValueKind::Integer;
                case UiAccessibilityValueKind::Number:
                    return UiAccessibilityActionValueKind::Number;
                case UiAccessibilityValueKind::Text:
                    return UiAccessibilityActionValueKind::Text;
            }
            return UiAccessibilityActionValueKind::None;
        }
    }  // namespace

    /** @copydoc UiAccessibilityActionId::Create */
    Result<UiAccessibilityActionId> UiAccessibilityActionId::Create(const std::uint32_t value) {
        return value == 0 ? Failure<UiAccessibilityActionId>(UiErrors::AccessibilityActionInvalid)
                          : Result<UiAccessibilityActionId>::Success(UiAccessibilityActionId{value});
    }

    /** @copydoc UiAccessibilityContributorId::Create */
    Result<UiAccessibilityContributorId> UiAccessibilityContributorId::Create(const std::uint64_t value) {
        return value == 0 ? Failure<UiAccessibilityContributorId>(UiErrors::AccessibilityContributorInvalid)
                          : Result<UiAccessibilityContributorId>::Success(UiAccessibilityContributorId{value});
    }

    /** @copydoc UiAccessibilityValue::IsValid */
    bool UiAccessibilityValue::IsValid() const noexcept {
        if (!IsKnownValueKind(kind))
            return false;
        if (kind == UiAccessibilityValueKind::Number)
            return std::isfinite(number);
        if (kind == UiAccessibilityValueKind::Text)
            return text.IsPresent();
        return true;
    }

    /** @copydoc UiAccessibilityRange::IsValid */
    bool UiAccessibilityRange::IsValid() const noexcept {
        return std::isfinite(minimum) && std::isfinite(maximum) && std::isfinite(current) && std::isfinite(step) && minimum <= maximum &&
               current >= minimum && current <= maximum && step >= 0.0;
    }

    /** @copydoc UiAccessibilitySelection::IsValid */
    bool UiAccessibilitySelection::IsValid() const noexcept {
        if (!IsKnownSelectionMode(mode))
            return false;
        if (mode == UiAccessibilitySelectionMode::None)
            return !selected && index == 0 && count == 0;
        return count != 0 && index < count;
    }

    /** @copydoc UiAccessibilityLimits::IsValid */
    bool UiAccessibilityLimits::IsValid() const noexcept {
        return nodes != 0 && nodes <= MaximumUiAccessibilityNodes && relations <= MaximumUiAccessibilityRelations &&
               actions <= MaximumUiAccessibilityActions && textBytes <= MaximumUiAccessibilityTextBytes;
    }

    /** @copydoc UiAccessibilitySnapshotDescriptor::IsValid */
    bool UiAccessibilitySnapshotDescriptor::IsValid() const noexcept {
        return schemaVersion == CurrentUiAccessibilitySchemaVersion && instance.IsValid() && canvas.IsValid() && document.IsValid() &&
               documentRevision.IsValid() && treeRevision.IsValid() && interactionRevision.IsValid() && semanticRevision.IsValid() &&
               limits.IsValid() && instance.ownership == canvas.ownership;
    }

    /** @copydoc UiAccessibilityExtractorDescriptor::IsValid */
    bool UiAccessibilityExtractorDescriptor::IsValid() const noexcept {
        return instance.IsValid() && canvas.IsValid() && document.IsValid() && instance.ownership == canvas.ownership && limits.IsValid() &&
               concurrentSnapshots != 0 && concurrentSnapshots <= MaximumUiAccessibilitySnapshotsInFlight;
    }

    /** @brief Preallocated immutable semantic slot. */
    struct UiAccessibilitySnapshot::Storage final {
        mutable std::atomic<std::uint64_t> leases{};
        UiAccessibilitySnapshotDescriptor descriptor;
        std::vector<UiAccessibilityNode> nodes;
        std::vector<UiAccessibilityRelation> relations;
        std::vector<UiAccessibilityAction> actions;
        std::vector<char> text;

        explicit Storage(const UiAccessibilityLimits &limits) {
            nodes.reserve(limits.nodes);
            relations.reserve(limits.relations);
            actions.reserve(limits.actions);
            text.reserve(limits.textBytes);
        }

        void Publish(const UiElementTree &tree, const UiAccessibilitySnapshotDescriptor &sourceDescriptor,
                     const UiAccessibilityProjection &projection, const ProjectionLookup lookup) {
            descriptor = sourceDescriptor;
            nodes.clear();
            relations.clear();
            actions.clear();
            text.clear();
            for (const auto &input : projection.nodes) {
                const auto elementResult = tree.Find(input.element);
                const auto element = elementResult.Value();
                UiAccessibilityNode node;
                node.id = MakeNodeId(element);
                node.element = element;
                node.elementId = input.element;
                node.role = input.role;
                node.source = input.source;
                node.contributor = input.contributor;
                node.name = CopyText(text, input.name);
                node.description = CopyText(text, input.description);
                node.value = CopyValue(text, input.value);
                node.state = input.state;
                node.hasRange = input.hasRange;
                node.range = input.range;
                node.hasSelection = input.hasSelection;
                node.selection = input.selection;
                node.error.kind = input.error.kind;
                node.error.message = CopyText(text, input.error.message);
                node.exposure = input.exposure;
                node.bounds = input.bounds;
                node.firstRelation = static_cast<std::uint32_t>(relations.size());
                for (const auto &relation : input.relations) {
                    const auto target = tree.Find(relation.target).Value();
                    relations.push_back({relation.kind, MakeNodeId(target)});
                }
                node.relationCount = static_cast<std::uint32_t>(input.relations.size());
                node.firstAction = static_cast<std::uint32_t>(actions.size());
                for (const auto &action : input.actions)
                    actions.push_back({action.id, action.kind, action.argumentKind, CopyText(text, action.name)});
                node.actionCount = static_cast<std::uint32_t>(input.actions.size());
                nodes.push_back(node);
            }
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                auto parent = tree.Get(nodes[index].element).Value().parent;
                while (parent.IsValid()) {
                    const auto parentRecord = tree.Get(parent).Value();
                    const auto parentIndex = FindProjectionIndex(lookup, parentRecord.id);
                    if (parentIndex != std::numeric_limits<std::size_t>::max()) {
                        nodes[index].parent = nodes[parentIndex].id;
                        break;
                    }
                    parent = parentRecord.parent;
                }
            }
        }
    };

    /** @brief Preallocated semantic slots and owner-thread admission state. */
    struct UiAccessibilityExtractor::Storage final {
        UiAccessibilityExtractorDescriptor descriptor;
        UiAccessibilityExtractorState lifecycle{UiAccessibilityExtractorState::Active};
        std::vector<std::shared_ptr<UiAccessibilitySnapshot::Storage>> slots;
        std::size_t nextSlot{};
        UiAccessibilitySemanticRevision lastRevision;
        std::vector<std::uint8_t> cycleScratch;
        std::vector<ProjectionLookupEntry> lookupScratch;

        explicit Storage(const UiAccessibilityExtractorDescriptor &source) : descriptor(source), cycleScratch(source.limits.nodes) {
            slots.reserve(source.concurrentSnapshots);
            lookupScratch.reserve(source.limits.nodes);
            for (std::uint32_t index = 0; index < source.concurrentSnapshots; ++index)
                slots.push_back(std::make_shared<UiAccessibilitySnapshot::Storage>(source.limits));
        }

        std::shared_ptr<UiAccessibilitySnapshot::Storage> TryAcquire() noexcept {
            for (std::size_t offset = 0; offset < slots.size(); ++offset) {
                const auto index = (nextSlot + offset) % slots.size();
                std::uint64_t expected{};
                if (slots[index]->leases.compare_exchange_strong(expected, 1)) {
                    nextSlot = (index + 1) % slots.size();
                    return slots[index];
                }
            }
            return {};
        }

        [[nodiscard]] bool IsDrained() const noexcept {
            for (const auto &slot : slots)
                if (slot->leases.load() != 0)
                    return false;
            return true;
        }
    };

    /** @copydoc UiAccessibilitySnapshot::UiAccessibilitySnapshot(std::shared_ptr<const Storage>) */
    UiAccessibilitySnapshot::UiAccessibilitySnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiAccessibilitySnapshot::~UiAccessibilitySnapshot */
    UiAccessibilitySnapshot::~UiAccessibilitySnapshot() {
        Release();
    }

    /** @copydoc UiAccessibilitySnapshot::UiAccessibilitySnapshot(const UiAccessibilitySnapshot &) */
    UiAccessibilitySnapshot::UiAccessibilitySnapshot(const UiAccessibilitySnapshot &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiAccessibilitySnapshot::operator=(const UiAccessibilitySnapshot &) */
    UiAccessibilitySnapshot &UiAccessibilitySnapshot::operator=(const UiAccessibilitySnapshot &other) noexcept {
        if (this != &other) {
            UiAccessibilitySnapshot replacement{other};
            *this = std::move(replacement);
        }
        return *this;
    }

    /** @copydoc UiAccessibilitySnapshot::UiAccessibilitySnapshot(UiAccessibilitySnapshot &&) */
    UiAccessibilitySnapshot::UiAccessibilitySnapshot(UiAccessibilitySnapshot &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiAccessibilitySnapshot::operator=(UiAccessibilitySnapshot &&) */
    UiAccessibilitySnapshot &UiAccessibilitySnapshot::operator=(UiAccessibilitySnapshot &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiAccessibilitySnapshot::Retain */
    void UiAccessibilitySnapshot::Retain() const noexcept {
        if (!storage_)
            return;
        auto current = storage_->leases.load();
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (storage_->leases.compare_exchange_weak(current, current + 1))
                return;
        }
        std::terminate();
    }

    /** @copydoc UiAccessibilitySnapshot::Release */
    void UiAccessibilitySnapshot::Release() noexcept {
        if (!storage_)
            return;
        storage_->leases.fetch_sub(1);
        storage_.reset();
    }

    /** @copydoc UiAccessibilitySnapshot::Descriptor */
    const UiAccessibilitySnapshotDescriptor &UiAccessibilitySnapshot::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiAccessibilitySnapshot::Nodes */
    std::span<const UiAccessibilityNode> UiAccessibilitySnapshot::Nodes() const noexcept {
        return storage_->nodes;
    }

    /** @copydoc UiAccessibilitySnapshot::Relations */
    std::span<const UiAccessibilityRelation> UiAccessibilitySnapshot::Relations() const noexcept {
        return storage_->relations;
    }

    /** @copydoc UiAccessibilitySnapshot::Actions */
    std::span<const UiAccessibilityAction> UiAccessibilitySnapshot::Actions() const noexcept {
        return storage_->actions;
    }

    /** @copydoc UiAccessibilitySnapshot::Text */
    std::string_view UiAccessibilitySnapshot::Text(const UiAccessibilityTextRef text) const noexcept {
        if (!text.IsPresent() || text.offset > storage_->text.size() || text.size > storage_->text.size() - text.offset)
            return {};
        return {storage_->text.data() + text.offset, text.size};
    }

    /** @copydoc UiAccessibilitySnapshot::Find */
    Result<UiAccessibilityNodeId> UiAccessibilitySnapshot::Find(const UiElementId element) const {
        if (!element.IsValid())
            return Failure<UiAccessibilityNodeId>(UiErrors::HandleMalformed);
        for (const auto &node : storage_->nodes)
            if (node.elementId == element)
                return Result<UiAccessibilityNodeId>::Success(node.id);
        return Failure<UiAccessibilityNodeId>(UiErrors::HandleStale);
    }

    /** @copydoc UiAccessibilitySnapshot::Get */
    Result<UiAccessibilityNode> UiAccessibilitySnapshot::Get(const UiAccessibilityNodeId node) const {
        if (!node.IsValid())
            return Failure<UiAccessibilityNode>(UiErrors::HandleMalformed);
        if (node.ownership != storage_->descriptor.instance.ownership)
            return Failure<UiAccessibilityNode>(UiErrors::HandleOwnerMismatch);
        for (const auto &record : storage_->nodes)
            if (record.id == node)
                return Result<UiAccessibilityNode>::Success(record);
        return Failure<UiAccessibilityNode>(UiErrors::HandleStale);
    }

    /** @copydoc UiAccessibilitySnapshot::Actions(const UiAccessibilityNode &) */
    std::span<const UiAccessibilityAction> UiAccessibilitySnapshot::Actions(const UiAccessibilityNode &node) const noexcept {
        if (node.firstAction > storage_->actions.size() || node.actionCount > storage_->actions.size() - node.firstAction)
            return {};
        return {storage_->actions.data() + node.firstAction, node.actionCount};
    }

    /** @copydoc UiAccessibilitySnapshot::Relations(const UiAccessibilityNode &) */
    std::span<const UiAccessibilityRelation> UiAccessibilitySnapshot::Relations(const UiAccessibilityNode &node) const noexcept {
        if (node.firstRelation > storage_->relations.size() || node.relationCount > storage_->relations.size() - node.firstRelation)
            return {};
        return {storage_->relations.data() + node.firstRelation, node.relationCount};
    }

    /** @copydoc UiAccessibilityExtractor::Create */
    Result<UiAccessibilityExtractor> UiAccessibilityExtractor::Create(const UiAccessibilityExtractorDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiAccessibilityExtractor>(UiErrors::AccessibilitySnapshotInvalid);
        try {
            return Result<UiAccessibilityExtractor>::Success(UiAccessibilityExtractor{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiAccessibilityExtractor>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiAccessibilityExtractor::UiAccessibilityExtractor(std::unique_ptr<Storage>) */
    UiAccessibilityExtractor::UiAccessibilityExtractor(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiAccessibilityExtractor::~UiAccessibilityExtractor */
    UiAccessibilityExtractor::~UiAccessibilityExtractor() {
        Close();
    }

    /** @copydoc UiAccessibilityExtractor::UiAccessibilityExtractor(UiAccessibilityExtractor &&) */
    UiAccessibilityExtractor::UiAccessibilityExtractor(UiAccessibilityExtractor &&) noexcept = default;

    /** @copydoc UiAccessibilityExtractor::operator=(UiAccessibilityExtractor &&) */
    UiAccessibilityExtractor &UiAccessibilityExtractor::operator=(UiAccessibilityExtractor &&) noexcept = default;

    /** @copydoc UiAccessibilityExtractor::Extract */
    Result<UiAccessibilitySnapshot> UiAccessibilityExtractor::Extract(const UiElementTree &tree,
                                                                      const UiAccessibilitySnapshotDescriptor &descriptor,
                                                                      const UiAccessibilityProjection &projection) {
        if (!storage_ || storage_->lifecycle != UiAccessibilityExtractorState::Active)
            return Failure<UiAccessibilitySnapshot>(UiErrors::AccessibilityLifecycleUnavailable);
        if (const auto validation = ValidateProjection(tree, descriptor, projection, storage_->descriptor, storage_->lastRevision,
                                                       storage_->cycleScratch, storage_->lookupScratch);
            validation.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(validation.ErrorValue());
        auto slot = storage_->TryAcquire();
        if (!slot)
            return Failure<UiAccessibilitySnapshot>(UiErrors::AccessibilitySnapshotStorageExhausted);

        struct PublishLease final {
            UiAccessibilitySnapshot::Storage *storage{};

            ~PublishLease() {
                if (storage)
                    storage->leases.store(0);
            }

            void Commit() noexcept {
                storage = nullptr;
            }
        } lease{slot.get()};

        try {
            slot->Publish(tree, descriptor, projection, storage_->lookupScratch);
        } catch (const std::bad_alloc &) {
            return Failure<UiAccessibilitySnapshot>(UiErrors::CapacityExceeded);
        }
        lease.Commit();
        storage_->lastRevision = descriptor.semanticRevision;
        return Result<UiAccessibilitySnapshot>::Success(UiAccessibilitySnapshot{std::move(slot)});
    }

    /** @copydoc UiAccessibilityExtractor::Close */
    void UiAccessibilityExtractor::Close() noexcept {
        if (storage_)
            storage_->lifecycle = UiAccessibilityExtractorState::Closed;
    }

    /** @copydoc UiAccessibilityExtractor::IsDrained */
    bool UiAccessibilityExtractor::IsDrained() const noexcept {
        return !storage_ || storage_->IsDrained();
    }

    /** @copydoc UiAccessibilityExtractor::State */
    UiAccessibilityExtractorState UiAccessibilityExtractor::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiAccessibilityExtractorState::Closed;
    }

    /** @copydoc ValidateUiAccessibilityActionRequest */
    Result<void> ValidateUiAccessibilityActionRequest(const UiAccessibilitySnapshot &snapshot,
                                                      const UiAccessibilityActionRequest &request) {
        const auto &descriptor = snapshot.Descriptor();
        if (request.instance != descriptor.instance || request.canvas != descriptor.canvas || request.document != descriptor.document ||
            request.documentRevision != descriptor.documentRevision || request.treeRevision != descriptor.treeRevision ||
            request.interactionRevision != descriptor.interactionRevision || request.semanticRevision != descriptor.semanticRevision)
            return Failure<void>(UiErrors::AccessibilityActionStale);
        if (!request.action.IsValid())
            return Failure<void>(UiErrors::AccessibilityActionInvalid);
        const auto nodeResult = snapshot.Get(request.node);
        if (nodeResult.HasError())
            return Failure<void>(UiErrors::AccessibilityActionStale);
        const auto &node = nodeResult.Value();
        if (node.state.Has(UiAccessibilityStateFlag::Disabled) ||
            (node.exposure != UiAccessibilityExposure::Visible && node.exposure != UiAccessibilityExposure::Offscreen))
            return Failure<void>(UiErrors::AccessibilityActionRejected);
        const auto actions = snapshot.Actions(node);
        const auto found = std::ranges::find_if(actions, [&request](const UiAccessibilityAction &action) {
            return action.id == request.action;
        });
        if (found == actions.end())
            return Failure<void>(UiErrors::AccessibilityActionStale);
        if (ActionArgumentKind(request.argument.kind) != found->argumentKind)
            return Failure<void>(UiErrors::AccessibilityActionRejected);
        if (const auto value = ValidateValue(request.argument); value.HasError())
            return value;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
