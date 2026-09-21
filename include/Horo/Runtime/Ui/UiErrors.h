#pragma once

/**
 * @file UiErrors.h
 * @brief Stable errors for backend-neutral Runtime UI contracts.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Runtime::Ui::UiErrors {
    /** @brief A stable authored identity is the reserved all-zero value. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief A runtime ownership generation is the reserved zero value. */
    extern const ErrorCodeDescriptor OwnershipGenerationInvalid;
    /** @brief A runtime handle has an invalid owner, slot, or slot generation. */
    extern const ErrorCodeDescriptor HandleMalformed;
    /** @brief A runtime handle belongs to another service, scope, or instance incarnation. */
    extern const ErrorCodeDescriptor HandleOwnerMismatch;
    /** @brief A runtime handle names an absent, retired, or replaced slot generation. */
    extern const ErrorCodeDescriptor HandleStale;
    /** @brief A revision is the reserved zero value. */
    extern const ErrorCodeDescriptor RevisionInvalid;
    /** @brief An expected revision no longer matches the owner-published revision. */
    extern const ErrorCodeDescriptor RevisionStale;
    /** @brief A generation or revision cannot advance without wrapping. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief Authored document or canvas metadata is invalid. */
    extern const ErrorCodeDescriptor DocumentInvalid;
    /** @brief A document repeats a stable canvas or root-element identity. */
    extern const ErrorCodeDescriptor DocumentDuplicateIdentity;
    /** @brief An asset dependency is malformed or conflicts with an earlier requirement. */
    extern const ErrorCodeDescriptor DependencyInvalid;
    /** @brief A bounded document or cooked payload limit was exceeded. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief Cooked bytes are empty or exceed the declared representation contract. */
    extern const ErrorCodeDescriptor PayloadInvalid;
    /** @brief A scene/component canvas reference lacks stable identity or revision evidence. */
    extern const ErrorCodeDescriptor CanvasReferenceInvalid;
    /** @brief A canvas descriptor, viewport extent, or caller-supplied scale is malformed. */
    extern const ErrorCodeDescriptor CanvasSpaceInvalid;
    /** @brief A screen/world resolver was used with the wrong semantic canvas mode. */
    extern const ErrorCodeDescriptor CanvasSpaceModeMismatch;
    /** @brief Canvas resolution cannot be represented safely in the canonical logical domain. */
    extern const ErrorCodeDescriptor CanvasSpaceOverflow;
    /** @brief A runtime instance cannot admit the requested lifecycle transition. */
    extern const ErrorCodeDescriptor InstanceStateInvalid;
    /** @brief A retained element tree is malformed, disconnected, cyclic, or exceeds its declared depth. */
    extern const ErrorCodeDescriptor ElementTreeInvalid;
    /** @brief A retained element tree repeats a stable authored identity. */
    extern const ErrorCodeDescriptor ElementTreeIdentityConflict;
    /** @brief A structural command has an invalid safe point, identity, or child position. */
    extern const ErrorCodeDescriptor StructuralCommandInvalid;
    /** @brief A structural command would remove the root or create a cyclic/conflicting topology. */
    extern const ErrorCodeDescriptor StructuralCommandConflict;
    /** @brief A retained element tree is retiring, stopped, or otherwise unavailable for the request. */
    extern const ErrorCodeDescriptor ElementTreeLifecycleUnavailable;
    /** @brief A layout request, geometry value, evaluator result, or dirty target is malformed. */
    extern const ErrorCodeDescriptor LayoutInvalid;
    /** @brief Layout source identity or revision evidence does not match the active tree/canvas. */
    extern const ErrorCodeDescriptor LayoutSourceStale;
    /** @brief Arrange-time dependency resolution changed more than the one bounded remeasure permits. */
    extern const ErrorCodeDescriptor LayoutNonConvergent;
    /** @brief Every preallocated immutable layout snapshot slot remains leased. */
    extern const ErrorCodeDescriptor LayoutSnapshotStorageExhausted;
    /** @brief The layout engine is retiring or stopped and rejects new work. */
    extern const ErrorCodeDescriptor LayoutLifecycleUnavailable;
    /** @brief Hit-test projection, geometry, pointer, ray, or canvas evidence is malformed. */
    extern const ErrorCodeDescriptor HitTestInvalid;
    /** @brief Hit-test ownership, tree, canvas, or interaction evidence is stale or mismatched. */
    extern const ErrorCodeDescriptor HitTestSourceStale;
    /** @brief The queried interaction generation is not the last successfully presented generation. */
    extern const ErrorCodeDescriptor HitTestNotPresented;
    /** @brief Every preallocated immutable hit-test snapshot slot remains leased. */
    extern const ErrorCodeDescriptor HitTestSnapshotStorageExhausted;
    /** @brief The hit-test store is retiring or stopped and rejects publication. */
    extern const ErrorCodeDescriptor HitTestLifecycleUnavailable;
    /** @brief Routed-event identities, kind, sequence, or modal handle are malformed. */
    extern const ErrorCodeDescriptor EventDispatchInvalid;
    /** @brief Routed-event owner, tree, document, target, or interaction evidence is stale. */
    extern const ErrorCodeDescriptor EventDispatchSourceStale;
    /** @brief The targeted element is outside the active inclusive modal root. */
    extern const ErrorCodeDescriptor EventDispatchModalBoundaryViolation;
    /** @brief The frozen route exceeded its preallocated root-inclusive depth. */
    extern const ErrorCodeDescriptor EventDispatchCapacityExceeded;
    /** @brief A handler structurally changed or destroyed the frozen event route. */
    extern const ErrorCodeDescriptor EventDispatchRouteInvalidated;
    /** @brief A handler attempted nested dispatch through the same dispatcher. */
    extern const ErrorCodeDescriptor EventDispatchReentrant;
    /** @brief A routed handler or default action threw across the callback boundary. */
    extern const ErrorCodeDescriptor EventDispatchHandlerFailed;
    /** @brief The event dispatcher is retiring, stopped, or changing lifecycle during dispatch. */
    extern const ErrorCodeDescriptor EventDispatchLifecycleUnavailable;
    /** @brief A typed Runtime UI action or owner context is malformed. */
    extern const ErrorCodeDescriptor ActionInvalid;
    /** @brief A typed Runtime UI action payload contains an invalid value. */
    extern const ErrorCodeDescriptor ActionPayloadInvalid;
    /** @brief A typed Runtime UI action payload exceeded its fixed argument bound. */
    extern const ErrorCodeDescriptor ActionPayloadCapacityExceeded;
    /** @brief A typed Runtime UI command is malformed or uses an incompatible operation. */
    extern const ErrorCodeDescriptor ActionCommandInvalid;
    /** @brief The preallocated Runtime UI action queue is full. */
    extern const ErrorCodeDescriptor ActionQueueCapacityExceeded;
    /** @brief An action source belongs to another owner or published revision. */
    extern const ErrorCodeDescriptor ActionSourceStale;
    /** @brief A typed action result has an invalid state-specific field combination. */
    extern const ErrorCodeDescriptor ActionResultInvalid;
    /** @brief A typed action result does not correlate to the admitted request. */
    extern const ErrorCodeDescriptor ActionResultStale;
    /** @brief A Runtime UI action consumer threw across the borrowed callback boundary. */
    extern const ErrorCodeDescriptor ActionHandlerFailed;
    /** @brief The Runtime UI action router is retiring, stopped, or dispatching reentrantly. */
    extern const ErrorCodeDescriptor ActionLifecycleUnavailable;
    /** @brief A default navigation command or result has invalid focus evidence. */
    extern const ErrorCodeDescriptor NavigationInvalid;
    /** @brief A Runtime UI focus graph, node, scope, or transition is malformed. */
    extern const ErrorCodeDescriptor FocusInvalid;
    /** @brief Focus owner, tree, interaction, or handle evidence belongs to another generation. */
    extern const ErrorCodeDescriptor FocusSourceStale;
    /** @brief A requested focus target is absent, disabled, hidden, or otherwise unavailable. */
    extern const ErrorCodeDescriptor FocusTargetUnavailable;
    /** @brief A focus transition would escape the active inclusive modal boundary. */
    extern const ErrorCodeDescriptor FocusModalBoundaryViolation;
    /** @brief The bounded focus graph node capacity was exceeded or could not be allocated. */
    extern const ErrorCodeDescriptor FocusCapacityExceeded;
    /** @brief The bounded modal or restoration stack cannot admit another scope. */
    extern const ErrorCodeDescriptor FocusModalCapacityExceeded;
    /** @brief A modal close identity is stale or is not the current top modal. */
    extern const ErrorCodeDescriptor FocusModalStale;
    /** @brief A reload attempted to change the player, presentation layer, or owner scope. */
    extern const ErrorCodeDescriptor FocusScopeMismatch;
    /** @brief The focus graph is retiring or stopped and rejects the request. */
    extern const ErrorCodeDescriptor FocusLifecycleUnavailable;
    /** @brief Immutable Runtime UI render snapshot evidence or table topology is malformed. */
    extern const ErrorCodeDescriptor RenderSnapshotInvalid;
    /** @brief A Runtime UI draw command contains invalid geometry, paint, or table references. */
    extern const ErrorCodeDescriptor RenderCommandInvalid;
    /** @brief A Runtime UI render resource identity, role, or revision is invalid. */
    extern const ErrorCodeDescriptor RenderResourceReferenceInvalid;
    /** @brief Every preallocated Runtime UI render snapshot slot is still leased by an in-flight frame. */
    extern const ErrorCodeDescriptor RenderSnapshotStorageExhausted;
    /** @brief A Runtime UI render extractor is closed and no longer accepts snapshots. */
    extern const ErrorCodeDescriptor RenderSnapshotLifecycleUnavailable;
    /** @brief A Runtime UI render composition request exceeds its declared bounded pass capacity. */
    extern const ErrorCodeDescriptor RenderCompositionCapacityExceeded;
    /** @brief A Runtime UI render composition pass has invalid view, graph, target, ordering, or space policy. */
    extern const ErrorCodeDescriptor RenderCompositionInvalid;
    /** @brief Runtime UI render-completion evidence has an invalid outcome or reason. */
    extern const ErrorCodeDescriptor RenderPresentationInvalid;
    /** @brief Runtime UI render-completion evidence is older than the last observed or presented revision. */
    extern const ErrorCodeDescriptor RenderPresentationStale;
    /** @brief Runtime UI diagnostic evidence is malformed or exceeds its fixed bounds. */
    extern const ErrorCodeDescriptor DiagnosticInvalid;
    /** @brief A Runtime UI diagnostic category or source error is not part of the declared contract. */
    extern const ErrorCodeDescriptor DiagnosticUnsupported;
}  // namespace Horo::Runtime::Ui::UiErrors
