#include "Horo/Prefab/PrefabErrors.h"

#include <array>

namespace Horo::Prefab::PrefabErrors {
    namespace {
        const ErrorDomainId Domain{"horo.prefab"};

        /** @brief Builds an immutable descriptor in the prefab error domain. */
        [[nodiscard]] ErrorCodeDescriptor Describe(const char *code, const char *summary, const char *remediationHint) {
            return {
                .domain = Domain,
                .code = ErrorCode{code},
                .defaultSeverity = ErrorSeverity::Error,
                .summary = summary,
                .remediationHint = remediationHint,
                .retryable = false,
                .userActionable = true,
            };
        }
    }  // namespace

    const ErrorCodeDescriptor IdentityInvalid = Describe("prefab.identity.invalid", "A prefab identity is invalid.",
                                                         "Provide the stable persisted identity required by the prefab contract.");

    const ErrorCodeDescriptor AddressInvalid = Describe("prefab.address.invalid", "A prefab-local address is invalid.",
                                                        "Use a bounded non-root placement scope and valid registered target identities.");

    const ErrorCodeDescriptor ReferenceInvalid = Describe("prefab.reference.invalid", "A prefab asset reference is invalid.",
                                                          "Use the non-zero AssetId from the Asset Registry sidecar.");

    const ErrorCodeDescriptor LimitProfileInvalid =
        Describe("prefab.limit_profile.invalid", "A prefab limit profile is invalid.",
                 "Use positive project limits within the engine safety ceilings and satisfy every cross-field constraint.");

    const ErrorCodeDescriptor WorkBudgetExceeded =
        Describe("prefab.work_budget.exceeded", "A prefab expansion exhausted its work budget.",
                 "Reduce the prefab expansion workload or lower the amount admitted into one candidate operation.");

    const ErrorCodeDescriptor DocumentInvalid = Describe("prefab.document.invalid", "A prefab authoring document is invalid.",
                                                         "Use a canonical project version, valid AssetId and exactly one document mode.");

    const ErrorCodeDescriptor HierarchyInvalid =
        Describe("prefab.hierarchy.invalid", "A prefab hierarchy is invalid.",
                 "Use one root followed by uniquely identified children whose parents precede them.");

    const ErrorCodeDescriptor ObjectCountExceeded = Describe("prefab.object_count.exceeded", "A prefab document contains too many objects.",
                                                             "Reduce the authored hierarchy to the declared object-count bound.");

    const ErrorCodeDescriptor NestedPlacementCountExceeded =
        Describe("prefab.nested_placement_count.exceeded", "A prefab document contains too many nested placements.",
                 "Reduce direct prefab composition edges to the declared count bound.");

    const ErrorCodeDescriptor ReferenceCountExceeded =
        Describe("prefab.reference_count.exceeded", "A prefab document contains too many asset references.",
                 "Reduce explicit Asset Registry dependencies to the declared count bound.");

    const ErrorCodeDescriptor HierarchyDepthExceeded = Describe("prefab.hierarchy_depth.exceeded", "A prefab hierarchy is too deep.",
                                                                "Flatten the hierarchy to the declared root-inclusive depth bound.");

    const ErrorCodeDescriptor ComponentCountExceeded =
        Describe("prefab.component_count.exceeded", "A prefab object contains too many components or behaviors.",
                 "Reduce the object's combined component and behavior count to the declared bound.");

    const ErrorCodeDescriptor PayloadTooLarge =
        Describe("prefab.payload.too_large", "A prefab document payload is too large.",
                 "Reduce names, component payloads, behavior fields or reference data to the declared byte bound.");

    const ErrorCodeDescriptor CompositionInvalid =
        Describe("prefab.composition.invalid", "Prefab composition data is invalid.",
                 "Use unique bounded placements for concrete prefabs or one exclusive variant parent.");

    const ErrorCodeDescriptor DependencyGraphInvalid =
        Describe("prefab.dependency_graph.invalid", "A prefab dependency graph candidate is invalid.",
                 "Provide each prefab source once with a canonical revision matching its document and registry record.");

    const ErrorCodeDescriptor DependencyUnavailable =
        Describe("prefab.dependency.unavailable", "A prefab dependency is unavailable.",
                 "Include the registered asset and every referenced prefab source in the pinned graph candidate.");

    const ErrorCodeDescriptor DependencyTypeMismatch =
        Describe("prefab.dependency.type_mismatch", "A prefab dependency has an incompatible asset type.",
                 "Register prefab graph sources and composition targets as core.prefab assets.");

    const ErrorCodeDescriptor DependencyRevisionMismatch =
        Describe("prefab.dependency.revision_mismatch", "A prefab dependency source revision does not match.",
                 "Reload the dependency graph from one coherent immutable source and Asset Registry snapshot.");
    const ErrorCodeDescriptor DependencyConflict =
        Describe("prefab.dependency.conflict", "Prefab dependency evidence conflicts for one asset identity.",
                 "Use one registry snapshot and one exact type and source revision for each asset identity.");
    const ErrorCodeDescriptor DependencyConflictPolicyUnsupported =
        Describe("prefab.dependency.conflict_policy_unsupported", "The prefab dependency conflict policy is unsupported.",
                 "Use the strict reject policy supported by this contract version.");
    const ErrorCodeDescriptor DependencyClosureCapacityExceeded =
        Describe("prefab.dependency.closure_capacity_exceeded", "The prefab dependency closure exceeds its capacity.",
                 "Raise the caller-owned bound within project policy or reduce the transitive asset closure.");
    const ErrorCodeDescriptor ResolutionStale =
        Describe("prefab.resolution.stale", "A completed prefab resolution is stale.",
                 "Discard the candidate and resolve again from the current registry and document revisions.");

    const ErrorCodeDescriptor IdentityCollision =
        Describe("prefab.identity.collision", "A generated prefab scene identity collides with an existing identity.",
                 "Reject the complete expansion and report the colliding authored or expanded object provenance.");

    const ErrorCodeDescriptor ReferenceRewriteInvalid =
        Describe("prefab.reference_rewrite.invalid", "A prefab reference cannot be rewritten transactionally.",
                 "Provide one canonical typed target whose owner and referenced object belong to the complete candidate.");

    const ErrorCodeDescriptor MissingPrefabSource =
        Describe("prefab.source.missing", "A referenced prefab source is unavailable.",
                 "Restore the source in the pinned project snapshot or remove the reference before expanding or cooking.");
    const ErrorCodeDescriptor SourceRevisionUnavailable =
        Describe("prefab.source.revision_unavailable", "A referenced prefab source revision is unavailable.",
                 "Resolve the reference against one coherent source and Asset Registry revision.");
    const ErrorCodeDescriptor UnsupportedPrefabSchema =
        Describe("prefab.source.schema_unsupported", "The prefab source schema is unsupported.",
                 "Run the supported project migration before validating, expanding, or cooking the prefab.");
    const ErrorCodeDescriptor InvalidPlacement =
        Describe("prefab.placement.invalid", "A prefab placement is invalid.",
                 "Provide one unique bounded placement identity with a valid containing object and source revision.");
    const ErrorCodeDescriptor MultipleVariantParents =
        Describe("prefab.composition.multiple_variant_parents", "A prefab declares multiple variant parents.",
                 "Keep exactly one immediate variant parent and preserve other references as ordinary dependencies.");
    const ErrorCodeDescriptor CyclicComposition = Describe("prefab.composition.cycle", "Prefab composition contains a cycle.",
                                                           "Remove the nested or variant edge that closes the dependency cycle.");
    const ErrorCodeDescriptor VariantDepthExceeded =
        Describe("prefab.variant.depth_exceeded", "Prefab variant inheritance exceeds its bound.",
                 "Reduce the variant chain to the configured maximum inheritance depth.");
    const ErrorCodeDescriptor CompositionDepthExceeded =
        Describe("prefab.composition.depth_exceeded", "Prefab nested composition exceeds its bound.",
                 "Flatten or reduce nested placements within the configured composition depth.");
    const ErrorCodeDescriptor CompositionEdgeLimitExceeded =
        Describe("prefab.composition.edge_limit_exceeded", "Prefab composition contains too many edges.",
                 "Reduce nested and variant composition edges admitted into one candidate.");

    const ErrorCodeDescriptor OverrideInvalid =
        Describe("prefab.override.invalid", "A prefab override record is invalid.",
                 "Use a supported typed operation and complete object, component, property, and source-revision identity.");
    const ErrorCodeDescriptor OverrideConflict =
        Describe("prefab.override.conflict", "A prefab override conflicts with another authored change.",
                 "Resolve the conflicting operation explicitly before applying or cooking the override set.");
    const ErrorCodeDescriptor OverrideOrphan = Describe("prefab.override.orphan", "A prefab override target cannot be resolved.",
                                                        "Preserve or repair the orphan against the current source and schema identities.");
    const ErrorCodeDescriptor OverrideSourceStale =
        Describe("prefab.override.source_stale", "A prefab override targets a stale source revision.",
                 "Rebase the override against the current source revision before applying it.");

    const ErrorCodeDescriptor CookInputInvalid =
        Describe("prefab.cook.input_invalid", "Prefab cook input is incomplete or inconsistent.",
                 "Supply a migrated, fully resolved candidate with its complete dependency and revision evidence.");
    const ErrorCodeDescriptor CookArtifactInvalid =
        Describe("prefab.cook.artifact_invalid", "A cooked prefab artifact is invalid.",
                 "Discard the candidate and rebuild the artifact from the authoritative resolved source.");
    const ErrorCodeDescriptor CookPayloadTooLarge =
        Describe("prefab.cook.payload_too_large", "A cooked prefab payload exceeds its bound.",
                 "Reduce the effective hierarchy or payload to the configured cooked representation limit.");

    const ErrorCodeDescriptor MigrationRequired =
        Describe("prefab.migration.required", "The prefab source requires migration.",
                 "Run the project migration pipeline before editing, expanding, or cooking this source.");
    const ErrorCodeDescriptor MigrationFailed =
        Describe("prefab.migration.failed", "Prefab source migration failed.",
                 "Retain the authoritative source and repair the reported migration failure before publication.");

    const ErrorCodeDescriptor AssetNotFound =
        Describe("prefab.runtime.asset_not_found", "The requested runtime prefab is not in the cooked catalog.",
                 "Cook or mount the prefab artifact before submitting the spawn request.");
    const ErrorCodeDescriptor AssetNotLoaded = Describe("prefab.runtime.asset_not_loaded", "The requested runtime prefab is not resident.",
                                                        "Use the admitted load-then-spawn path or wait for the required asset lease.");
    const ErrorCodeDescriptor UnsupportedCookedVersion =
        Describe("prefab.runtime.cooked_version_unsupported", "The cooked prefab format is unsupported.",
                 "Use a runtime-compatible cooked artifact; runtime never migrates or recooks cooked bytes.");
    const ErrorCodeDescriptor CorruptedPayload = Describe("prefab.runtime.payload_corrupted", "The cooked prefab payload is corrupt.",
                                                          "Discard the artifact and rebuild it from the authoritative source.");
    const ErrorCodeDescriptor ComponentTypeUnregistered =
        Describe("prefab.runtime.component_type_unregistered", "The cooked prefab references an unavailable component type.",
                 "Load the owning gameplay module or recook without the unavailable component.");
    const ErrorCodeDescriptor ComponentAllocationFailed =
        Describe("prefab.runtime.component_allocation_failed", "Runtime prefab component staging could not allocate storage.",
                 "Reduce the admitted spawn workload or retry after releasing bounded runtime capacity.");
    const ErrorCodeDescriptor SpawnRecursionDetected =
        Describe("prefab.runtime.spawn_recursion_detected", "Runtime prefab spawn recursion was detected.",
                 "Remove the repeated prefab from the inherited spawn lineage.");
    const ErrorCodeDescriptor SpawnDepthExceeded =
        Describe("prefab.runtime.spawn_depth_exceeded", "Runtime prefab spawn depth exceeds its bound.",
                 "Reduce lifecycle-created spawn depth to the configured runtime limit.");
    const ErrorCodeDescriptor AdmissionRejected =
        Describe("prefab.runtime.admission_rejected", "Runtime prefab spawn admission was rejected.",
                 "Retry after the bounded scene command or operation capacity becomes available.");
    const ErrorCodeDescriptor Cancelled =
        Describe("prefab.runtime.cancelled", "Runtime prefab spawn was cancelled before publication.",
                 "Retry from the still-active owning operation when cancellation is no longer requested.");
    const ErrorCodeDescriptor SceneUnavailable = Describe("prefab.runtime.scene_unavailable", "The target runtime scene is unavailable.",
                                                          "Submit the request against the current active scene generation.");
    const ErrorCodeDescriptor InvalidParent = Describe("prefab.runtime.invalid_parent", "The runtime prefab spawn parent is invalid.",
                                                       "Use a live parent in the target scene with the current entity generation.");
    const ErrorCodeDescriptor EntityAllocationExhausted =
        Describe("prefab.runtime.entity_allocation_exhausted", "Runtime entity identity capacity is exhausted.",
                 "Release or replace the owning scene runtime before admitting another spawn.");

    const ErrorCodeDescriptor DiagnosticInvalid =
        Describe("prefab.diagnostic.invalid", "Prefab diagnostic evidence is malformed.",
                 "Provide complete bounded identity, revision, operation, source, and dependency evidence.");
    const ErrorCodeDescriptor DiagnosticUnsupported =
        Describe("prefab.diagnostic.unsupported", "The prefab diagnostic source is outside the declared contract.",
                 "Use a declared prefab category and preserve only supported Asset, Scene, Gameplay, or migration causes.");
    const ErrorCodeDescriptor DiagnosticBudgetExceeded =
        Describe("prefab.diagnostic.budget_exceeded", "Prefab diagnostic evidence exceeds its explicit bound.",
                 "Reduce cause, dependency, message, or source evidence before publishing the diagnostic.");

    std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept {
        static const std::array descriptors{
            &IdentityInvalid,
            &AddressInvalid,
            &ReferenceInvalid,
            &LimitProfileInvalid,
            &WorkBudgetExceeded,
            &DocumentInvalid,
            &HierarchyInvalid,
            &ObjectCountExceeded,
            &NestedPlacementCountExceeded,
            &ReferenceCountExceeded,
            &HierarchyDepthExceeded,
            &ComponentCountExceeded,
            &PayloadTooLarge,
            &CompositionInvalid,
            &DependencyGraphInvalid,
            &DependencyUnavailable,
            &DependencyTypeMismatch,
            &DependencyRevisionMismatch,
            &DependencyConflict,
            &DependencyConflictPolicyUnsupported,
            &DependencyClosureCapacityExceeded,
            &ResolutionStale,
            &IdentityCollision,
            &ReferenceRewriteInvalid,
            &MissingPrefabSource,
            &SourceRevisionUnavailable,
            &UnsupportedPrefabSchema,
            &InvalidPlacement,
            &MultipleVariantParents,
            &CyclicComposition,
            &VariantDepthExceeded,
            &CompositionDepthExceeded,
            &CompositionEdgeLimitExceeded,
            &OverrideInvalid,
            &OverrideConflict,
            &OverrideOrphan,
            &OverrideSourceStale,
            &CookInputInvalid,
            &CookArtifactInvalid,
            &CookPayloadTooLarge,
            &MigrationRequired,
            &MigrationFailed,
            &AssetNotFound,
            &AssetNotLoaded,
            &UnsupportedCookedVersion,
            &CorruptedPayload,
            &ComponentTypeUnregistered,
            &ComponentAllocationFailed,
            &SpawnRecursionDetected,
            &SpawnDepthExceeded,
            &AdmissionRejected,
            &Cancelled,
            &SceneUnavailable,
            &InvalidParent,
            &EntityAllocationExhausted,
            &DiagnosticInvalid,
            &DiagnosticUnsupported,
            &DiagnosticBudgetExceeded,
        };
        return descriptors;
    }
}  // namespace Horo::Prefab::PrefabErrors
