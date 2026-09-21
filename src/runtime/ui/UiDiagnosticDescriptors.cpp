#include "UiDiagnosticDescriptors.h"

#include <algorithm>
#include <array>

namespace Horo::Runtime::Ui::DiagnosticsInternal {
    namespace {
        const std::array core{
            &UiErrors::IdentityInvalid,
            &UiErrors::OwnershipGenerationInvalid,
            &UiErrors::HandleMalformed,
            &UiErrors::HandleOwnerMismatch,
            &UiErrors::HandleStale,
            &UiErrors::RevisionInvalid,
            &UiErrors::RevisionStale,
            &UiErrors::GenerationExhausted,
            &UiErrors::DocumentInvalid,
            &UiErrors::DocumentDuplicateIdentity,
            &UiErrors::DependencyInvalid,
            &UiErrors::CapacityExceeded,
            &UiErrors::PayloadInvalid,
            &UiErrors::CanvasReferenceInvalid,
            &UiErrors::CanvasSpaceInvalid,
            &UiErrors::CanvasSpaceModeMismatch,
            &UiErrors::CanvasSpaceOverflow,
            &UiErrors::InstanceStateInvalid,
            &UiErrors::ElementTreeInvalid,
            &UiErrors::ElementTreeIdentityConflict,
            &UiErrors::StructuralCommandInvalid,
            &UiErrors::StructuralCommandConflict,
            &UiErrors::ElementTreeLifecycleUnavailable,
            &UiErrors::LayoutInvalid,
            &UiErrors::LayoutSourceStale,
            &UiErrors::LayoutNonConvergent,
            &UiErrors::LayoutSnapshotStorageExhausted,
            &UiErrors::LayoutLifecycleUnavailable,
            &UiErrors::HitTestInvalid,
            &UiErrors::HitTestSourceStale,
            &UiErrors::HitTestNotPresented,
            &UiErrors::HitTestSnapshotStorageExhausted,
            &UiErrors::HitTestLifecycleUnavailable,
            &UiErrors::RenderSnapshotInvalid,
            &UiErrors::RenderCommandInvalid,
            &UiErrors::RenderResourceReferenceInvalid,
            &UiErrors::RenderSnapshotStorageExhausted,
            &UiErrors::RenderSnapshotLifecycleUnavailable,
            &UiErrors::RenderCompositionCapacityExceeded,
            &UiErrors::RenderCompositionInvalid,
            &UiErrors::RenderPresentationInvalid,
            &UiErrors::RenderPresentationStale,
            &UiErrors::EventDispatchInvalid,
            &UiErrors::EventDispatchSourceStale,
            &UiErrors::EventDispatchModalBoundaryViolation,
            &UiErrors::EventDispatchCapacityExceeded,
            &UiErrors::EventDispatchRouteInvalidated,
            &UiErrors::EventDispatchReentrant,
            &UiErrors::EventDispatchHandlerFailed,
            &UiErrors::EventDispatchLifecycleUnavailable,
            &UiErrors::DiagnosticInvalid,
            &UiErrors::DiagnosticUnsupported,
        };
        const std::array accessibility{
            &UiErrors::AccessibilitySchemaInvalid,        &UiErrors::AccessibilityRoleInvalid,
            &UiErrors::AccessibilityStateInvalid,         &UiErrors::AccessibilityValueInvalid,
            &UiErrors::AccessibilityRangeInvalid,         &UiErrors::AccessibilitySelectionInvalid,
            &UiErrors::AccessibilityTextInvalid,          &UiErrors::AccessibilityNameMissing,
            &UiErrors::AccessibilityRelationInvalid,      &UiErrors::AccessibilityActionInvalid,
            &UiErrors::AccessibilityContributorInvalid,   &UiErrors::AccessibilitySnapshotInvalid,
            &UiErrors::AccessibilitySnapshotSourceStale,  &UiErrors::AccessibilitySnapshotStorageExhausted,
            &UiErrors::AccessibilityLifecycleUnavailable, &UiErrors::AccessibilityActionStale,
            &UiErrors::AccessibilityActionRejected,       &UiErrors::AccessibilityFocusConflict,
        };
        const auto descriptors = [] {
            std::array<const ErrorCodeDescriptor *, core.size() + accessibility.size()> combined{};
            auto output = std::ranges::copy(core, combined.begin()).out;
            std::ranges::copy(accessibility, output);
            return combined;
        }();
    }  // namespace

    std::span<const ErrorCodeDescriptor *const> ErrorDescriptors() noexcept {
        return descriptors;
    }
}  // namespace Horo::Runtime::Ui::DiagnosticsInternal
