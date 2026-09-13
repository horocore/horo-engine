#include "Horo/Prefab/PrefabErrors.h"

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

    const ErrorCodeDescriptor ResolutionStale =
        Describe("prefab.resolution.stale", "A completed prefab resolution is stale.",
                 "Discard the candidate and resolve again from the current registry and document revisions.");

    const ErrorCodeDescriptor IdentityCollision =
        Describe("prefab.identity.collision", "A generated prefab scene identity collides with an existing identity.",
                 "Reject the complete expansion and report the colliding authored or expanded object provenance.");

    const ErrorCodeDescriptor ReferenceRewriteInvalid =
        Describe("prefab.reference_rewrite.invalid", "A prefab reference cannot be rewritten transactionally.",
                 "Provide one canonical typed target whose owner and referenced object belong to the complete candidate.");
}  // namespace Horo::Prefab::PrefabErrors
