#include "Horo/Runtime/Ui/UiAccessibility.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiAccessibilityPolicy.h"
#include "UiAccessibilityStorage.h"

#include <algorithm>
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

        using AccessibilityInternal::AllowsActivate;
        using AccessibilityInternal::AllowsChecked;
        using AccessibilityInternal::AllowsDismiss;
        using AccessibilityInternal::AllowsExpanded;
        using AccessibilityInternal::AllowsMultiSelectable;
        using AccessibilityInternal::AllowsPopup;
        using AccessibilityInternal::AllowsPressed;
        using AccessibilityInternal::AllowsRange;
        using AccessibilityInternal::AllowsReadOnly;
        using AccessibilityInternal::AllowsRequired;
        using AccessibilityInternal::AllowsScroll;
        using AccessibilityInternal::AllowsSelected;
        using AccessibilityInternal::AllowsSelection;
        using AccessibilityInternal::AllowsValue;
        using AccessibilityInternal::FindProjectionIndex;
        using AccessibilityInternal::IsKnownActionKind;
        using AccessibilityInternal::IsKnownActionValueKind;
        using AccessibilityInternal::IsKnownErrorKind;
        using AccessibilityInternal::IsKnownExposure;
        using AccessibilityInternal::IsKnownRelationKind;
        using AccessibilityInternal::IsKnownRole;
        using AccessibilityInternal::IsKnownSelectionMode;
        using AccessibilityInternal::IsKnownSource;
        using AccessibilityInternal::IsKnownTextSource;
        using AccessibilityInternal::IsKnownValueKind;
        using AccessibilityInternal::IsValueCompatible;
        using AccessibilityInternal::ProjectionLookup;
        using AccessibilityInternal::ProjectionLookupEntry;
        using AccessibilityInternal::RequiresName;

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
