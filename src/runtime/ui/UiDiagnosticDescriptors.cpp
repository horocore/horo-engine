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
        const std::array textLayout{
            &UiErrors::TextLayoutInputInvalid,    &UiErrors::TextLayoutSourceStale,      &UiErrors::TextLayoutCapacityExceeded,
            &UiErrors::TextLayoutEllipsisInvalid, &UiErrors::TextLayoutStorageExhausted, &UiErrors::TextLayoutLifecycleUnavailable,
        };
        const std::array glyphAtlas{
            &UiErrors::GlyphAtlasInputInvalid,
            &UiErrors::GlyphAtlasSourceStale,
            &UiErrors::GlyphAtlasCapacityExceeded,
            &UiErrors::GlyphAtlasPressure,
            &UiErrors::GlyphAtlasFallbackUnavailable,
            &UiErrors::GlyphAtlasUploadInvalid,
            &UiErrors::GlyphAtlasUploadCapacityExceeded,
            &UiErrors::GlyphAtlasUploadStale,
            &UiErrors::GlyphAtlasUploadInvalidTransition,
            &UiErrors::GlyphAtlasFrameInvalid,
            &UiErrors::GlyphAtlasFrameCapacityExceeded,
            &UiErrors::GlyphAtlasFrameInFlight,
            &UiErrors::GlyphAtlasEvictionInvalid,
            &UiErrors::GlyphAtlasResetInvalid,
            &UiErrors::GlyphAtlasResetBusy,
            &UiErrors::GlyphAtlasLifecycleUnavailable,
            &UiErrors::GlyphAtlasUploadInFlight,
        };
        const std::array actions{
            &UiErrors::ActionInvalid,
            &UiErrors::ActionPayloadInvalid,
            &UiErrors::ActionPayloadCapacityExceeded,
            &UiErrors::ActionCommandInvalid,
            &UiErrors::ActionQueueCapacityExceeded,
            &UiErrors::ActionSourceStale,
            &UiErrors::ActionResultInvalid,
            &UiErrors::ActionResultStale,
            &UiErrors::ActionHandlerFailed,
            &UiErrors::ActionLifecycleUnavailable,
            &UiErrors::NavigationInvalid,
        };
        const std::array bindings{
            &UiErrors::BindingDescriptorInvalid,  &UiErrors::BindingSchemaInvalid,   &UiErrors::BindingSchemaIncompatible,
            &UiErrors::BindingProviderUnknown,    &UiErrors::BindingPropertyUnknown, &UiErrors::BindingPropertySignatureMismatch,
            &UiErrors::BindingDescriptorConflict, &UiErrors::BindingAccessInvalid,   &UiErrors::BindingTypeMismatch,
            &UiErrors::BindingConverterInvalid,   &UiErrors::BindingFallbackInvalid, &UiErrors::BindingUpdatePolicyInvalid,
            &UiErrors::BindingCapacityExceeded,
        };
        const std::array pointerCapture{
            &UiErrors::PointerCaptureInvalid, &UiErrors::PointerCaptureSourceStale,      &UiErrors::PointerCaptureInteractionStale,
            &UiErrors::PointerCaptureBusy,    &UiErrors::PointerCaptureCapacityExceeded, &UiErrors::PointerCaptureLifecycleUnavailable,
        };
        const std::array controls{
            &UiErrors::ControlDescriptorInvalid, &UiErrors::ControlInputInvalid,         &UiErrors::ControlSourceStale,
            &UiErrors::ControlDefaultPending,    &UiErrors::ControlDefaultInvalid,       &UiErrors::ControlCapacityExceeded,
            &UiErrors::ControlSequenceInvalid,   &UiErrors::ControlLifecycleUnavailable,
        };
        const std::array focus{
            &UiErrors::FocusInvalid,
            &UiErrors::FocusSourceStale,
            &UiErrors::FocusTargetUnavailable,
            &UiErrors::FocusModalBoundaryViolation,
            &UiErrors::FocusCapacityExceeded,
            &UiErrors::FocusModalCapacityExceeded,
            &UiErrors::FocusModalStale,
            &UiErrors::FocusScopeMismatch,
            &UiErrors::FocusLifecycleUnavailable,
        };
        const std::array renderGeometry{
            &UiErrors::RenderGeometryInvalid,
            &UiErrors::RenderGeometryCapacityExceeded,
            &UiErrors::RenderGeometryStorageExhausted,
            &UiErrors::RenderGeometryLifecycleUnavailable,
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
            std::array<const ErrorCodeDescriptor *, core.size() + textLayout.size() + glyphAtlas.size() + pointerCapture.size() + actions.size() +
                                                        bindings.size() + controls.size() + focus.size() + renderGeometry.size() +
                                                        accessibility.size()>
                combined{};
            auto output = std::ranges::copy(core, combined.begin()).out;
            output = std::ranges::copy(textLayout, output).out;
            output = std::ranges::copy(glyphAtlas, output).out;
            output = std::ranges::copy(pointerCapture, output).out;
            output = std::ranges::copy(actions, output).out;
            output = std::ranges::copy(bindings, output).out;
            output = std::ranges::copy(controls, output).out;
            output = std::ranges::copy(focus, output).out;
            output = std::ranges::copy(renderGeometry, output).out;
            std::ranges::copy(accessibility, output);
            return combined;
        }();
    }  // namespace

    std::span<const ErrorCodeDescriptor *const> ErrorDescriptors() noexcept {
        return descriptors;
    }
}  // namespace Horo::Runtime::Ui::DiagnosticsInternal
