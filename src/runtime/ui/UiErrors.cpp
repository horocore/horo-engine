#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc IdentityInvalid */
    const ErrorCodeDescriptor IdentityInvalid{UiDomain,
                                              ErrorCode{"runtime_ui.identity.invalid"},
                                              ErrorSeverity::Error,
                                              "The stable Runtime UI identity is invalid.",
                                              "Provide a non-zero 128-bit identity owned by the authoring document.",
                                              false,
                                              true};
    /** @copydoc OwnershipGenerationInvalid */
    const ErrorCodeDescriptor OwnershipGenerationInvalid{UiDomain,
                                                         ErrorCode{"runtime_ui.ownership_generation.invalid"},
                                                         ErrorSeverity::Error,
                                                         "The Runtime UI ownership generation is invalid.",
                                                         "Use the non-zero generation issued by the active RuntimeUiService owner.",
                                                         false,
                                                         false};
    /** @copydoc HandleMalformed */
    const ErrorCodeDescriptor HandleMalformed{UiDomain,
                                              ErrorCode{"runtime_ui.handle.malformed"},
                                              ErrorSeverity::Error,
                                              "The Runtime UI handle is malformed.",
                                              "Use a handle issued by the owning registry with non-zero owner, slot, and slot generation.",
                                              false,
                                              false};
    /** @copydoc HandleOwnerMismatch */
    const ErrorCodeDescriptor HandleOwnerMismatch{UiDomain,
                                                  ErrorCode{"runtime_ui.handle.owner_mismatch"},
                                                  ErrorSeverity::Error,
                                                  "The Runtime UI handle belongs to another owner generation.",
                                                  "Resolve the stable identity against the active owner scope again.",
                                                  false,
                                                  false};
    /** @copydoc HandleStale */
    const ErrorCodeDescriptor HandleStale{UiDomain,
                                          ErrorCode{"runtime_ui.handle.stale"},
                                          ErrorSeverity::Error,
                                          "The Runtime UI handle slot is absent or retired.",
                                          "Discard the transient handle and resolve the stable identity against the current tree.",
                                          false,
                                          false};
    /** @copydoc RevisionInvalid */
    const ErrorCodeDescriptor RevisionInvalid{UiDomain,
                                              ErrorCode{"runtime_ui.revision.invalid"},
                                              ErrorSeverity::Error,
                                              "The Runtime UI revision is invalid.",
                                              "Use a non-zero revision published by the owning Runtime UI object.",
                                              false,
                                              false};
    /** @copydoc RevisionStale */
    const ErrorCodeDescriptor RevisionStale{UiDomain,
                                            ErrorCode{"runtime_ui.revision.stale"},
                                            ErrorSeverity::Error,
                                            "The expected Runtime UI revision is stale.",
                                            "Reload the current owner-published revision and prepare the command again.",
                                            true,
                                            false};
    /** @copydoc GenerationExhausted */
    const ErrorCodeDescriptor GenerationExhausted{UiDomain,
                                                  ErrorCode{"runtime_ui.generation.exhausted"},
                                                  ErrorSeverity::Critical,
                                                  "The Runtime UI generation range is exhausted.",
                                                  "Close admission and replace the owning game runtime; never wrap the identity.",
                                                  false,
                                                  false};
    /** @copydoc DocumentInvalid */
    const ErrorCodeDescriptor DocumentInvalid{UiDomain,
                                              ErrorCode{"runtime_ui.document.invalid"},
                                              ErrorSeverity::Error,
                                              "The Runtime UI document is invalid.",
                                              "Provide valid document, revision, canvas, and root identities.",
                                              false,
                                              true};
    /** @copydoc DocumentDuplicateIdentity */
    const ErrorCodeDescriptor DocumentDuplicateIdentity{UiDomain,
                                                        ErrorCode{"runtime_ui.document.duplicate_identity"},
                                                        ErrorSeverity::Error,
                                                        "The Runtime UI document repeats a stable identity.",
                                                        "Assign unique canvas and root element identities.",
                                                        false,
                                                        true};
    /** @copydoc DependencyInvalid */
    const ErrorCodeDescriptor DependencyInvalid{UiDomain,
                                                ErrorCode{"runtime_ui.dependency.invalid"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI asset dependency is invalid or conflicting.",
                                                "Provide one valid expected asset type for each stable asset identity.",
                                                false,
                                                true};
    /** @copydoc CapacityExceeded */
    const ErrorCodeDescriptor CapacityExceeded{UiDomain,
                                               ErrorCode{"runtime_ui.capacity.exceeded"},
                                               ErrorSeverity::Error,
                                               "A bounded Runtime UI document limit was exceeded.",
                                               "Reduce canvas, dependency, or cooked payload size.",
                                               false,
                                               true};
    /** @copydoc PayloadInvalid */
    const ErrorCodeDescriptor PayloadInvalid{UiDomain,
                                             ErrorCode{"runtime_ui.payload.invalid"},
                                             ErrorSeverity::Error,
                                             "The cooked Runtime UI payload is invalid.",
                                             "Rebuild the asset with non-empty deterministic bytes within the declared bound.",
                                             false,
                                             true};
    /** @copydoc CanvasReferenceInvalid */
    const ErrorCodeDescriptor CanvasReferenceInvalid{UiDomain,
                                                     ErrorCode{"runtime_ui.canvas_reference.invalid"},
                                                     ErrorSeverity::Error,
                                                     "The Runtime UI canvas asset reference is invalid.",
                                                     "Provide complete asset, document, canvas, and minimum revision evidence.",
                                                     false,
                                                     true};
    /** @copydoc CanvasSpaceInvalid */
    const ErrorCodeDescriptor
        CanvasSpaceInvalid{UiDomain,
                           ErrorCode{"runtime_ui.canvas_space.invalid"},
                           ErrorSeverity::Error,
                           "The Runtime UI canvas-space input is invalid.",
                           "Provide known modes, bounded reference dimensions, a non-zero viewport, and valid caller-owned scale evidence.",
                           false,
                           true};
    /** @copydoc CanvasSpaceModeMismatch */
    const ErrorCodeDescriptor
        CanvasSpaceModeMismatch{UiDomain,
                                ErrorCode{"runtime_ui.canvas_space.mode_mismatch"},
                                ErrorSeverity::Error,
                                "The Runtime UI canvas resolver does not match the canvas projection mode.",
                                "Use screen resolution for overlay/camera canvases and logical world resolution for world-space canvases.",
                                false,
                                true};
    /** @copydoc CanvasSpaceOverflow */
    const ErrorCodeDescriptor CanvasSpaceOverflow{UiDomain,
                                                  ErrorCode{"runtime_ui.canvas_space.overflow"},
                                                  ErrorSeverity::Error,
                                                  "The resolved Runtime UI canvas extent exceeds the logical geometry domain.",
                                                  "Reduce the viewport extent or provide a larger valid pixels-per-DIP scale.",
                                                  false,
                                                  true};
    /** @copydoc InstanceStateInvalid */
    const ErrorCodeDescriptor InstanceStateInvalid{UiDomain,
                                                   ErrorCode{"runtime_ui.instance.state_invalid"},
                                                   ErrorSeverity::Error,
                                                   "The Runtime UI instance cannot perform this lifecycle transition.",
                                                   "Submit the transition only from its declared owner-thread lifecycle state.",
                                                   false,
                                                   false};
    /** @copydoc ElementTreeInvalid */
    const ErrorCodeDescriptor ElementTreeInvalid{UiDomain,
                                                 ErrorCode{"runtime_ui.element_tree.invalid"},
                                                 ErrorSeverity::Error,
                                                 "The retained Runtime UI element tree is invalid.",
                                                 "Provide one connected acyclic root tree within the declared depth and element bounds.",
                                                 false,
                                                 true};
    /** @copydoc ElementTreeIdentityConflict */
    const ErrorCodeDescriptor ElementTreeIdentityConflict{UiDomain,
                                                          ErrorCode{"runtime_ui.element_tree.identity_conflict"},
                                                          ErrorSeverity::Error,
                                                          "The retained Runtime UI element tree repeats a stable identity.",
                                                          "Assign one unique authored identity to every element in the tree.",
                                                          false,
                                                          true};
    /** @copydoc StructuralCommandInvalid */
    const ErrorCodeDescriptor
        StructuralCommandInvalid{UiDomain,
                                 ErrorCode{"runtime_ui.structural_command.invalid"},
                                 ErrorSeverity::Error,
                                 "The Runtime UI structural command is invalid.",
                                 "Use current handles, an owner safe point, and a child position valid after detachment.",
                                 false,
                                 false};
    /** @copydoc StructuralCommandConflict */
    const ErrorCodeDescriptor
        StructuralCommandConflict{UiDomain,
                                  ErrorCode{"runtime_ui.structural_command.conflict"},
                                  ErrorSeverity::Error,
                                  "The Runtime UI structural command conflicts with retained-tree invariants.",
                                  "Keep the root fixed and avoid self-parenting, descendant parenting, and competing topology changes.",
                                  false,
                                  false};
    /** @copydoc ElementTreeLifecycleUnavailable */
    const ErrorCodeDescriptor
        ElementTreeLifecycleUnavailable{UiDomain,
                                        ErrorCode{"runtime_ui.element_tree.lifecycle_unavailable"},
                                        ErrorSeverity::Error,
                                        "The retained Runtime UI element tree is unavailable in its current lifecycle state.",
                                        "Stop structural admission before retirement and query contents only until bounded shutdown "
                                        "completes.",
                                        false,
                                        false};
    /** @copydoc LayoutInvalid */
    const ErrorCodeDescriptor LayoutInvalid{UiDomain,
                                            ErrorCode{"runtime_ui.layout.invalid"},
                                            ErrorSeverity::Error,
                                            "The Runtime UI layout request or result is invalid.",
                                            "Provide finite bounded geometry, current handles, and one valid evaluator result per element.",
                                            false,
                                            false};
    /** @copydoc LayoutSourceStale */
    const ErrorCodeDescriptor LayoutSourceStale{UiDomain,
                                                ErrorCode{"runtime_ui.layout.source_stale"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI layout source evidence is stale or belongs to another owner.",
                                                "Rebuild the candidate from the active tree, canvas, and exact immutable source revisions.",
                                                true,
                                                false};
    /** @copydoc LayoutNonConvergent */
    const ErrorCodeDescriptor LayoutNonConvergent{UiDomain,
                                                  ErrorCode{"runtime_ui.layout.non_convergent"},
                                                  ErrorSeverity::Error,
                                                  "The Runtime UI layout did not converge within the bounded remeasure policy.",
                                                  "Remove the cyclic intrinsic/percentage dependency reported by the affected element.",
                                                  false,
                                                  true};
    /** @copydoc LayoutSnapshotStorageExhausted */
    const ErrorCodeDescriptor
        LayoutSnapshotStorageExhausted{UiDomain,
                                       ErrorCode{"runtime_ui.layout_snapshot.storage_exhausted"},
                                       ErrorSeverity::Error,
                                       "Every bounded Runtime UI layout snapshot slot is still leased.",
                                       "Retire an in-flight layout snapshot before retrying; never overwrite or allocate fallback storage.",
                                       true,
                                       false};
    /** @copydoc LayoutLifecycleUnavailable */
    const ErrorCodeDescriptor LayoutLifecycleUnavailable{UiDomain,
                                                         ErrorCode{"runtime_ui.layout.lifecycle_unavailable"},
                                                         ErrorSeverity::Error,
                                                         "The Runtime UI layout engine is closed.",
                                                         "Create a new engine for the active runtime canvas before submitting layout work.",
                                                         false,
                                                         false};
    /** @copydoc TextLayoutInputInvalid */
    const ErrorCodeDescriptor
        TextLayoutInputInvalid{UiDomain,
                               ErrorCode{"runtime_ui.text_layout.input_invalid"},
                               ErrorSeverity::Error,
                               "The Runtime UI text layout input or result is invalid.",
                               "Provide complete shaped cluster evidence, closed text policies, and finite logical bounds.",
                               false,
                               true};
    /** @copydoc TextLayoutSourceStale */
    const ErrorCodeDescriptor TextLayoutSourceStale{UiDomain,
                                                    ErrorCode{"runtime_ui.text_layout.source_stale"},
                                                    ErrorSeverity::Error,
                                                    "The Runtime UI text layout source belongs to another or older generation.",
                                                    "Prepare text from the active element, tree, content, intrinsic, and policy revisions.",
                                                    true,
                                                    false};
    /** @copydoc TextLayoutCapacityExceeded */
    const ErrorCodeDescriptor
        TextLayoutCapacityExceeded{UiDomain,
                                   ErrorCode{"runtime_ui.text_layout.capacity_exceeded"},
                                   ErrorSeverity::Error,
                                   "The Runtime UI text layout candidate exceeds its fixed capacity.",
                                   "Reduce source text, line, glyph, or face-run count within the declared owner limits.",
                                   true,
                                   false};
    /** @copydoc TextLayoutEllipsisInvalid */
    const ErrorCodeDescriptor
        TextLayoutEllipsisInvalid{UiDomain,
                                  ErrorCode{"runtime_ui.text_layout.ellipsis_invalid"},
                                  ErrorSeverity::Error,
                                  "The Runtime UI text layout needs a valid pre-shaped ellipsis view.",
                                  "Shape the declared ellipsis with the same font and content generation before layout.",
                                  false,
                                  true};
    /** @copydoc TextLayoutStorageExhausted */
    const ErrorCodeDescriptor
        TextLayoutStorageExhausted{UiDomain,
                                   ErrorCode{"runtime_ui.text_layout.storage_exhausted"},
                                   ErrorSeverity::Error,
                                   "Every bounded Runtime UI text layout result slot is still leased.",
                                   "Retire an in-flight text layout result before retrying; never overwrite or allocate fallback storage.",
                                   true,
                                   false};
    /** @copydoc TextLayoutLifecycleUnavailable */
    const ErrorCodeDescriptor
        TextLayoutLifecycleUnavailable{UiDomain,
                                       ErrorCode{"runtime_ui.text_layout.lifecycle_unavailable"},
                                       ErrorSeverity::Error,
                                       "The Runtime UI text layout engine is closed.",
                                       "Create a new text layout engine for the active element generation before submitting work.",
                                       false,
                                       false};
    /** @copydoc StyleInvalid */
    const ErrorCodeDescriptor StyleInvalid{UiDomain,
                                           ErrorCode{"runtime_ui.style.invalid"},
                                           ErrorSeverity::Error,
                                           "The Runtime UI style schema or request is invalid.",
                                           "Provide complete typed style identities, values, revisions, and bounded inputs.",
                                           false,
                                           true};
    /** @copydoc StyleIdentityConflict */
    const ErrorCodeDescriptor
        StyleIdentityConflict{UiDomain,
                              ErrorCode{"runtime_ui.style.identity_conflict"},
                              ErrorSeverity::Error,
                              "The Runtime UI style generation repeats a stable identity.",
                              "Use one unique asset, class, token, and property identity in the validated generation.",
                              false,
                              true};
    /** @copydoc StyleReferenceInvalid */
    const ErrorCodeDescriptor
        StyleReferenceInvalid{UiDomain,
                              ErrorCode{"runtime_ui.style.reference_invalid"},
                              ErrorSeverity::Error,
                              "The Runtime UI style reference is missing or foreign.",
                              "Resolve the exact qualified asset, class, token, or property identity before publication.",
                              false,
                              true};
    /** @copydoc StyleTypeMismatch */
    const ErrorCodeDescriptor StyleTypeMismatch{UiDomain,
                                                ErrorCode{"runtime_ui.style.type_mismatch"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI style value has an incompatible type or range.",
                                                "Use the exact registered property or token category and its declared bounds.",
                                                false,
                                                true};
    /** @copydoc StyleCycle */
    const ErrorCodeDescriptor StyleCycle{UiDomain,
                                         ErrorCode{"runtime_ui.style.cycle"},
                                         ErrorSeverity::Error,
                                         "The Runtime UI style dependency graph contains a cycle.",
                                         "Break the asset, class, or token cycle and prepare a complete candidate again.",
                                         false,
                                         true};
    /** @copydoc StyleStateInvalid */
    const ErrorCodeDescriptor StyleStateInvalid{UiDomain,
                                                ErrorCode{"runtime_ui.style.state_invalid"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI visual-state selector is invalid.",
                                                "Use known state bits, disjoint required/forbidden evidence, and a declared layer.",
                                                false,
                                                true};
    /** @copydoc StyleSourceStale */
    const ErrorCodeDescriptor StyleSourceStale{UiDomain,
                                               ErrorCode{"runtime_ui.style.source_stale"},
                                               ErrorSeverity::Error,
                                               "The Runtime UI style source belongs to another owner or generation.",
                                               "Prepare the candidate from the active tree, registry, and exact source revisions.",
                                               true,
                                               false};
    /** @copydoc StyleSnapshotStorageExhausted */
    const ErrorCodeDescriptor
        StyleSnapshotStorageExhausted{UiDomain,
                                      ErrorCode{"runtime_ui.style_snapshot.storage_exhausted"},
                                      ErrorSeverity::Error,
                                      "Every bounded Runtime UI computed-style snapshot slot is still leased.",
                                      "Retire an in-flight style snapshot before retrying; never overwrite or allocate fallback storage.",
                                      true,
                                      false};
    /** @copydoc StyleLifecycleUnavailable */
    const ErrorCodeDescriptor StyleLifecycleUnavailable{UiDomain,
                                                        ErrorCode{"runtime_ui.style.lifecycle_unavailable"},
                                                        ErrorSeverity::Error,
                                                        "The Runtime UI style owner is closed.",
                                                        "Create a new style registry or resolver for the active runtime generation.",
                                                        false,
                                                        false};
    /** @copydoc HitTestInvalid */
    const ErrorCodeDescriptor
        HitTestInvalid{UiDomain,
                       ErrorCode{"runtime_ui.hit_test.invalid"},
                       ErrorSeverity::Error,
                       "The Runtime UI hit-test request or projection is invalid.",
                       "Provide finite bounded geometry, exact element records, and a valid pointer or ray projection.",
                       false,
                       false};
    /** @copydoc HitTestSourceStale */
    const ErrorCodeDescriptor HitTestSourceStale{UiDomain,
                                                 ErrorCode{"runtime_ui.hit_test.source_stale"},
                                                 ErrorSeverity::Error,
                                                 "The Runtime UI hit-test source belongs to another owner or generation.",
                                                 "Publish from the exact active layout instance, canvas, document, and retained tree.",
                                                 true,
                                                 false};
    /** @copydoc HitTestNotPresented */
    const ErrorCodeDescriptor
        HitTestNotPresented{UiDomain,
                            ErrorCode{"runtime_ui.hit_test.not_presented"},
                            ErrorSeverity::Error,
                            "The Runtime UI interaction generation was not the last successfully presented generation.",
                            "Route input through the immutable generation adopted by successful presentation evidence.",
                            true,
                            false};
    /** @copydoc HitTestSnapshotStorageExhausted */
    const ErrorCodeDescriptor
        HitTestSnapshotStorageExhausted{UiDomain,
                                        ErrorCode{"runtime_ui.hit_test_snapshot.storage_exhausted"},
                                        ErrorSeverity::Error,
                                        "Every bounded Runtime UI hit-test snapshot slot is still leased.",
                                        "Retire an in-flight hit-test snapshot before retrying; never allocate fallback storage.",
                                        true,
                                        false};
    /** @copydoc HitTestLifecycleUnavailable */
    const ErrorCodeDescriptor
        HitTestLifecycleUnavailable{UiDomain,
                                    ErrorCode{"runtime_ui.hit_test.lifecycle_unavailable"},
                                    ErrorSeverity::Error,
                                    "The Runtime UI hit-test store is closed.",
                                    "Create a new store for the active runtime canvas before publishing interaction geometry.",
                                    false,
                                    false};
    /** @copydoc EventDispatchInvalid */
    const ErrorCodeDescriptor
        EventDispatchInvalid{UiDomain,
                             ErrorCode{"runtime_ui.event_dispatch.invalid"},
                             ErrorSeverity::Error,
                             "The Runtime UI routed event or route declaration is malformed.",
                             "Provide a known event kind, non-zero sequence, exact target, and coherent pointer payload.",
                             false,
                             false};
    /** @copydoc EventDispatchSourceStale */
    const ErrorCodeDescriptor EventDispatchSourceStale{UiDomain,
                                                       ErrorCode{"runtime_ui.event_dispatch.source_stale"},
                                                       ErrorSeverity::Error,
                                                       "The Runtime UI event route does not match the active retained-tree generation.",
                                                       "Retarget against the current presented interaction and retained-tree revision.",
                                                       true,
                                                       false};
    /** @copydoc EventDispatchModalBoundaryViolation */
    const ErrorCodeDescriptor
        EventDispatchModalBoundaryViolation{UiDomain,
                                            ErrorCode{"runtime_ui.event_dispatch.modal_boundary_violation"},
                                            ErrorSeverity::Error,
                                            "The Runtime UI event target is outside the active modal route boundary.",
                                            "Block the transition or target an element descended from the exact active modal root.",
                                            false,
                                            false};
    /** @copydoc EventDispatchCapacityExceeded */
    const ErrorCodeDescriptor EventDispatchCapacityExceeded{UiDomain,
                                                            ErrorCode{"runtime_ui.event_dispatch.capacity_exceeded"},
                                                            ErrorSeverity::Error,
                                                            "The Runtime UI event route exceeds the dispatcher's preallocated depth.",
                                                            "Use the retained tree's declared finite depth when creating the dispatcher.",
                                                            false,
                                                            false};
    /** @copydoc EventDispatchRouteInvalidated */
    const ErrorCodeDescriptor
        EventDispatchRouteInvalidated{UiDomain,
                                      ErrorCode{"runtime_ui.event_dispatch.route_invalidated"},
                                      ErrorSeverity::Warning,
                                      "A Runtime UI handler changed or destroyed the frozen routed-event path.",
                                      "Stop the current route and retarget later input against the next presented interaction revision.",
                                      false,
                                      false};
    /** @copydoc EventDispatchReentrant */
    const ErrorCodeDescriptor EventDispatchReentrant{UiDomain,
                                                     ErrorCode{"runtime_ui.event_dispatch.reentrant"},
                                                     ErrorSeverity::Error,
                                                     "A Runtime UI handler attempted nested dispatch through the active dispatcher.",
                                                     "Queue the nested event for the next owner dispatch turn.",
                                                     false,
                                                     false};
    /** @copydoc EventDispatchHandlerFailed */
    const ErrorCodeDescriptor EventDispatchHandlerFailed{UiDomain,
                                                         ErrorCode{"runtime_ui.event_dispatch.handler_failed"},
                                                         ErrorSeverity::Error,
                                                         "A Runtime UI routed handler threw across its callback boundary.",
                                                         "Return a typed failure from the handler and keep callback exceptions contained.",
                                                         false,
                                                         false};
    /** @copydoc EventDispatchLifecycleUnavailable */
    const ErrorCodeDescriptor
        EventDispatchLifecycleUnavailable{UiDomain,
                                          ErrorCode{"runtime_ui.event_dispatch.lifecycle_unavailable"},
                                          ErrorSeverity::Error,
                                          "The Runtime UI event dispatcher is closed or cannot change lifecycle during dispatch.",
                                          "Finish the active route or create a dispatcher for the current runtime canvas generation.",
                                          false,
                                          false};
    /** @copydoc ActionInvalid */
    const ErrorCodeDescriptor ActionInvalid{UiDomain,
                                            ErrorCode{"runtime_ui.action.invalid"},
                                            ErrorSeverity::Error,
                                            "The Runtime UI action owner context or request is invalid.",
                                            "Use exact active instance, canvas, document, revision, and element evidence.",
                                            false,
                                            false};
    /** @copydoc ActionPayloadInvalid */
    const ErrorCodeDescriptor ActionPayloadInvalid{UiDomain,
                                                   ErrorCode{"runtime_ui.action_payload.invalid"},
                                                   ErrorSeverity::Error,
                                                   "The Runtime UI action payload contains an invalid typed value.",
                                                   "Use finite scalar values, valid stable identities, and bounded UTF-8 text.",
                                                   false,
                                                   true};
    /** @copydoc ActionPayloadCapacityExceeded */
    const ErrorCodeDescriptor ActionPayloadCapacityExceeded{UiDomain,
                                                            ErrorCode{"runtime_ui.action_payload.capacity_exceeded"},
                                                            ErrorSeverity::Error,
                                                            "The Runtime UI action payload exceeds its fixed argument bound.",
                                                            "Reduce the command arguments to the declared bounded typed payload contract.",
                                                            false,
                                                            true};
    /** @copydoc ActionCommandInvalid */
    const ErrorCodeDescriptor ActionCommandInvalid{UiDomain,
                                                   ErrorCode{"runtime_ui.action_command.invalid"},
                                                   ErrorSeverity::Error,
                                                   "The Runtime UI typed action command is invalid.",
                                                   "Use a valid stable action identity and a compatible closed operation kind.",
                                                   false,
                                                   true};
    /** @copydoc ActionQueueCapacityExceeded */
    const ErrorCodeDescriptor
        ActionQueueCapacityExceeded{UiDomain,
                                    ErrorCode{"runtime_ui.action_queue.capacity_exceeded"},
                                    ErrorSeverity::Error,
                                    "The Runtime UI action queue is full.",
                                    "Retry at a later owner safe point or increase the finite admitted queue capacity.",
                                    true,
                                    false};
    /** @copydoc ActionSourceStale */
    const ErrorCodeDescriptor ActionSourceStale{UiDomain,
                                                ErrorCode{"runtime_ui.action.source_stale"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI action source belongs to another owner or revision.",
                                                "Retarget the action against the latest successfully presented interaction generation.",
                                                true,
                                                false};
    /** @copydoc ActionResultInvalid */
    const ErrorCodeDescriptor ActionResultInvalid{UiDomain,
                                                  ErrorCode{"runtime_ui.action_result.invalid"},
                                                  ErrorSeverity::Error,
                                                  "The Runtime UI action result has invalid state-specific fields.",
                                                  "Return exactly one closed action result state with matching operation evidence.",
                                                  false,
                                                  false};
    /** @copydoc ActionResultStale */
    const ErrorCodeDescriptor ActionResultStale{UiDomain,
                                                ErrorCode{"runtime_ui.action_result.stale"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI action result does not match its admitted request.",
                                                "Complete or reject the exact request identity supplied to the consumer.",
                                                true,
                                                false};
    /** @copydoc ActionHandlerFailed */
    const ErrorCodeDescriptor ActionHandlerFailed{UiDomain,
                                                  ErrorCode{"runtime_ui.action_handler.failed"},
                                                  ErrorSeverity::Error,
                                                  "A Runtime UI action consumer threw across its callback boundary.",
                                                  "Contain consumer failures and return a typed action result instead of throwing.",
                                                  false,
                                                  false};
    /** @copydoc ActionLifecycleUnavailable */
    const ErrorCodeDescriptor ActionLifecycleUnavailable{UiDomain,
                                                         ErrorCode{"runtime_ui.action.lifecycle_unavailable"},
                                                         ErrorSeverity::Error,
                                                         "The Runtime UI action router is closed or dispatching reentrantly.",
                                                         "Stop admitting actions during retirement and create a new router after reload.",
                                                         false,
                                                         false};
    /** @copydoc NavigationInvalid */
    const ErrorCodeDescriptor NavigationInvalid{UiDomain,
                                                ErrorCode{"runtime_ui.navigation.invalid"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI default navigation command or result is invalid.",
                                                "Use a known direction and exact presented focus handles.",
                                                false,
                                                false};
    /** @copydoc RenderSnapshotInvalid */
    const ErrorCodeDescriptor RenderSnapshotInvalid{UiDomain,
                                                    ErrorCode{"runtime_ui.render_snapshot.invalid"},
                                                    ErrorSeverity::Error,
                                                    "The immutable Runtime UI render snapshot is invalid.",
                                                    "Provide exact owner revisions and complete bounded logical projection tables.",
                                                    false,
                                                    false};
    /** @copydoc RenderCommandInvalid */
    const ErrorCodeDescriptor RenderCommandInvalid{UiDomain,
                                                   ErrorCode{"runtime_ui.render_command.invalid"},
                                                   ErrorSeverity::Error,
                                                   "A Runtime UI render command is invalid.",
                                                   "Use resident element handles, finite paint, and valid logical table references.",
                                                   false,
                                                   false};
    /** @copydoc RenderResourceReferenceInvalid */
    const ErrorCodeDescriptor
        RenderResourceReferenceInvalid{UiDomain,
                                       ErrorCode{"runtime_ui.render_resource_reference.invalid"},
                                       ErrorSeverity::Error,
                                       "A Runtime UI render resource reference is invalid.",
                                       "Provide a stable Horo asset with the exact nonzero revision and semantic role.",
                                       false,
                                       true};
    /** @copydoc RenderSnapshotStorageExhausted */
    const ErrorCodeDescriptor
        RenderSnapshotStorageExhausted{UiDomain,
                                       ErrorCode{"runtime_ui.render_snapshot.storage_exhausted"},
                                       ErrorSeverity::Error,
                                       "Every bounded Runtime UI render snapshot slot is still leased.",
                                       "Retire an in-flight snapshot before retrying; never overwrite or allocate fallback storage.",
                                       true,
                                       false};
    /** @copydoc RenderSnapshotLifecycleUnavailable */
    const ErrorCodeDescriptor
        RenderSnapshotLifecycleUnavailable{UiDomain,
                                           ErrorCode{"runtime_ui.render_snapshot.lifecycle_unavailable"},
                                           ErrorSeverity::Error,
                                           "The Runtime UI render extractor is closed.",
                                           "Create a new extractor for the active view generation before publishing another snapshot.",
                                           false,
                                           false};
    /** @copydoc RenderGeometryInvalid */
    const ErrorCodeDescriptor RenderGeometryInvalid{UiDomain,
                                                    ErrorCode{"runtime_ui.render_geometry.invalid"},
                                                    ErrorSeverity::Error,
                                                    "The generated Runtime UI geometry is invalid.",
                                                    "Provide finite logical paint data and a compatible bounded batch topology.",
                                                    false,
                                                    false};
    /** @copydoc RenderGeometryCapacityExceeded */
    const ErrorCodeDescriptor
        RenderGeometryCapacityExceeded{UiDomain,
                                       ErrorCode{"runtime_ui.render_geometry.capacity_exceeded"},
                                       ErrorSeverity::Error,
                                       "Generated Runtime UI geometry exceeds its bounded frame capacity.",
                                       "Increase the admitted geometry limits or reduce the UI draw, glyph, and batch count.",
                                       true,
                                       false};
    /** @copydoc RenderGeometryStorageExhausted */
    const ErrorCodeDescriptor
        RenderGeometryStorageExhausted{UiDomain,
                                       ErrorCode{"runtime_ui.render_geometry.storage_exhausted"},
                                       ErrorSeverity::Error,
                                       "Every bounded Runtime UI geometry plan slot is still leased.",
                                       "Retire an in-flight geometry plan before retrying; never overwrite or allocate fallback storage.",
                                       true,
                                       false};
    /** @copydoc RenderGeometryLifecycleUnavailable */
    const ErrorCodeDescriptor
        RenderGeometryLifecycleUnavailable{UiDomain,
                                           ErrorCode{"runtime_ui.render_geometry.lifecycle_unavailable"},
                                           ErrorSeverity::Error,
                                           "The Runtime UI geometry arena is closed.",
                                           "Create a new geometry arena for the active view generation before building another plan.",
                                           false,
                                           false};
    /** @copydoc RenderCompositionCapacityExceeded */
    const ErrorCodeDescriptor
        RenderCompositionCapacityExceeded{UiDomain,
                                          ErrorCode{"runtime_ui.render_composition.capacity_exceeded"},
                                          ErrorSeverity::Error,
                                          "The Runtime UI render composition request exceeds its bounded pass capacity.",
                                          "Split the view plan or increase its admitted capacity within the repository ceiling.",
                                          true,
                                          false};
    /** @copydoc RenderCompositionInvalid */
    const ErrorCodeDescriptor
        RenderCompositionInvalid{UiDomain,
                                 ErrorCode{"runtime_ui.render_composition.invalid"},
                                 ErrorSeverity::Error,
                                 "The Runtime UI render composition request is invalid.",
                                 "Use one exact view and graph with ordered compatible color, depth, space, and pass declarations.",
                                 false,
                                 false};
    /** @copydoc RenderPresentationInvalid */
    const ErrorCodeDescriptor
        RenderPresentationInvalid{UiDomain,
                                  ErrorCode{"runtime_ui.render_presentation.invalid"},
                                  ErrorSeverity::Error,
                                  "The Runtime UI render presentation result is invalid.",
                                  "Correlate a known terminal outcome and reason with valid view, canvas, and revision evidence.",
                                  false,
                                  false};
    /** @copydoc RenderPresentationStale */
    const ErrorCodeDescriptor
        RenderPresentationStale{UiDomain,
                                ErrorCode{"runtime_ui.render_presentation.stale"},
                                ErrorSeverity::Error,
                                "The Runtime UI render presentation result is stale.",
                                "Discard older completion evidence and retain the latest successfully presented interaction revision.",
                                false,
                                false};
    /** @copydoc DiagnosticInvalid */
    const ErrorCodeDescriptor
        DiagnosticInvalid{UiDomain,
                          ErrorCode{"runtime_ui.diagnostic.invalid"},
                          ErrorSeverity::Error,
                          "The Runtime UI diagnostic evidence is invalid.",
                          "Provide a known category, canonical Runtime UI error and bounded ordered correlation fields.",
                          false,
                          false};
    /** @copydoc DiagnosticUnsupported */
    const ErrorCodeDescriptor DiagnosticUnsupported{UiDomain,
                                                    ErrorCode{"runtime_ui.diagnostic.unsupported"},
                                                    ErrorSeverity::Error,
                                                    "The Runtime UI diagnostic source is unsupported.",
                                                    "Use a declared Runtime UI category and canonical horo.runtime_ui error descriptor.",
                                                    false,
                                                    false};
}  // namespace Horo::Runtime::Ui::UiErrors
