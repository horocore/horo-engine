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
    /** @brief A durable document schema version is not supported by this Runtime UI build. */
    extern const ErrorCodeDescriptor DocumentSchemaUnsupported;
    /** @brief A serialized UI document is malformed or has an invalid typed value. */
    extern const ErrorCodeDescriptor DocumentSerializationInvalid;
    /** @brief A serialized UI document exceeds a parser or semantic content bound. */
    extern const ErrorCodeDescriptor DocumentPayloadTooLarge;
    /** @brief A durable UI element repeats a property key. */
    extern const ErrorCodeDescriptor DocumentDuplicateProperty;
    /** @brief A durable UI reference is malformed or targets an absent authored identity. */
    extern const ErrorCodeDescriptor DocumentReferenceInvalid;
    /** @brief A durable UI hierarchy is disconnected or contains a cycle. */
    extern const ErrorCodeDescriptor DocumentHierarchyInvalid;
    /** @brief A durable UI route definition is malformed or duplicated. */
    extern const ErrorCodeDescriptor DocumentRouteInvalid;
    /** @brief No explicit migration chain reaches the requested UI document schema. */
    extern const ErrorCodeDescriptor DocumentMigrationMissing;
    /** @brief The supplied UI document migration graph is malformed or ambiguous. */
    extern const ErrorCodeDescriptor DocumentMigrationInvalid;
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
    /** @brief A pointer-capture context, pointer, button, or cancellation reason is malformed. */
    extern const ErrorCodeDescriptor PointerCaptureInvalid;
    /** @brief Pointer-capture owner, tree, target, or route evidence is stale or foreign. */
    extern const ErrorCodeDescriptor PointerCaptureSourceStale;
    /** @brief Pointer capture was requested against an interaction generation that was not presented. */
    extern const ErrorCodeDescriptor PointerCaptureInteractionStale;
    /** @brief The requested pointer is already captured by the same Runtime UI context. */
    extern const ErrorCodeDescriptor PointerCaptureBusy;
    /** @brief Every preallocated pointer-capture slot is occupied or retired. */
    extern const ErrorCodeDescriptor PointerCaptureCapacityExceeded;
    /** @brief The pointer-capture store is retiring, stopped, or otherwise unavailable. */
    extern const ErrorCodeDescriptor PointerCaptureLifecycleUnavailable;
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
    /** @brief A route-stack descriptor or operation request is malformed. */
    extern const ErrorCodeDescriptor RouteStackInvalid;
    /** @brief A route operation has invalid fields or targets an unsupported route. */
    extern const ErrorCodeDescriptor RouteOperationInvalid;
    /** @brief A route operation guard no longer matches the committed stack generation. */
    extern const ErrorCodeDescriptor RouteOperationStale;
    /** @brief A route operation was attempted while another transaction is preparing or committing. */
    extern const ErrorCodeDescriptor RouteOperationReentrant;
    /** @brief A route transaction was used after its terminal result was produced. */
    extern const ErrorCodeDescriptor RouteOperationAlreadyCompleted;
    /** @brief The route stack is retiring, stopped, or cannot admit another operation. */
    extern const ErrorCodeDescriptor RouteOperationLifecycleUnavailable;
    /** @brief A typed interactive-control descriptor is malformed or incompatible with its control kind. */
    extern const ErrorCodeDescriptor ControlDescriptorInvalid;
    /** @brief A normalized control input has invalid kind, source, sequence, tick, or payload evidence. */
    extern const ErrorCodeDescriptor ControlInputInvalid;
    /** @brief A control input belongs to another owner, element, or presented revision. */
    extern const ErrorCodeDescriptor ControlSourceStale;
    /** @brief A control input arrived before the staged default action was resolved. */
    extern const ErrorCodeDescriptor ControlDefaultPending;
    /** @brief A staged control default action could not be represented by its typed bounded payload. */
    extern const ErrorCodeDescriptor ControlDefaultInvalid;
    /** @brief A text input or repeat policy exceeded its fixed representation bound. */
    extern const ErrorCodeDescriptor ControlCapacityExceeded;
    /** @brief A control tick or event sequence moved backwards or exhausted its finite domain. */
    extern const ErrorCodeDescriptor ControlSequenceInvalid;
    /** @brief The interactive-control state machine is retiring, stopped, or changing lifecycle during input. */
    extern const ErrorCodeDescriptor ControlLifecycleUnavailable;
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
    /** @brief Generated Runtime UI geometry or its batch topology is malformed. */
    extern const ErrorCodeDescriptor RenderGeometryInvalid;
    /** @brief Generated Runtime UI geometry exceeds its admitted vertex, index, or batch capacity. */
    extern const ErrorCodeDescriptor RenderGeometryCapacityExceeded;
    /** @brief Every preallocated Runtime UI geometry plan slot remains leased by an in-flight frame. */
    extern const ErrorCodeDescriptor RenderGeometryStorageExhausted;
    /** @brief A Runtime UI geometry arena is closed and no longer accepts plans. */
    extern const ErrorCodeDescriptor RenderGeometryLifecycleUnavailable;
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
    /** @brief An accessibility semantic schema value is malformed or unsupported. */
    extern const ErrorCodeDescriptor AccessibilitySchemaInvalid;
    /** @brief An accessibility node role is unknown or incompatible with its control data. */
    extern const ErrorCodeDescriptor AccessibilityRoleInvalid;
    /** @brief An accessibility state is unknown or incompatible with its node role. */
    extern const ErrorCodeDescriptor AccessibilityStateInvalid;
    /** @brief An accessibility typed value is malformed or incompatible with its node role. */
    extern const ErrorCodeDescriptor AccessibilityValueInvalid;
    /** @brief Accessibility range metadata is malformed or not admitted for the node role. */
    extern const ErrorCodeDescriptor AccessibilityRangeInvalid;
    /** @brief Accessibility selection metadata is malformed or not admitted for the node role. */
    extern const ErrorCodeDescriptor AccessibilitySelectionInvalid;
    /** @brief Accessibility text is invalid, oversized, or not valid UTF-8. */
    extern const ErrorCodeDescriptor AccessibilityTextInvalid;
    /** @brief An interactive accessibility node has neither a name nor a valid label relation. */
    extern const ErrorCodeDescriptor AccessibilityNameMissing;
    /** @brief An accessibility relation is dangling, duplicated, cyclic, or unsupported. */
    extern const ErrorCodeDescriptor AccessibilityRelationInvalid;
    /** @brief An accessibility action is unknown, duplicated, or incompatible with its node. */
    extern const ErrorCodeDescriptor AccessibilityActionInvalid;
    /** @brief A contributed accessibility node has invalid contributor ownership evidence. */
    extern const ErrorCodeDescriptor AccessibilityContributorInvalid;
    /** @brief A complete accessibility snapshot candidate is malformed. */
    extern const ErrorCodeDescriptor AccessibilitySnapshotInvalid;
    /** @brief An accessibility snapshot source or semantic revision is stale. */
    extern const ErrorCodeDescriptor AccessibilitySnapshotSourceStale;
    /** @brief Every preallocated accessibility snapshot slot remains leased. */
    extern const ErrorCodeDescriptor AccessibilitySnapshotStorageExhausted;
    /** @brief The accessibility snapshot store is closed and rejects new publication. */
    extern const ErrorCodeDescriptor AccessibilityLifecycleUnavailable;
    /** @brief An accessibility action request targets an older or absent semantic generation. */
    extern const ErrorCodeDescriptor AccessibilityActionStale;
    /** @brief An accessibility action is currently disallowed by visibility, state, or argument policy. */
    extern const ErrorCodeDescriptor AccessibilityActionRejected;
    /** @brief More than one accessibility node claims semantic focus. */
    extern const ErrorCodeDescriptor AccessibilityFocusConflict;
}  // namespace Horo::Runtime::Ui::UiErrors
