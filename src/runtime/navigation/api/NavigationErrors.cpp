#include "Horo/Navigation/NavigationErrors.h"

#include <string>
#include <string_view>

namespace Horo::Navigation::NavigationErrors {
    namespace {
        const ErrorDomainId NavigationDomain{"horo.navigation"};

        [[nodiscard]] ErrorCodeDescriptor MakeNavMeshArtifactError(const std::string_view code, const ErrorSeverity severity,
                                                                   const std::string_view summary, const std::string_view remediationHint,
                                                                   const bool userActionable) {
            return {
                .domain = NavigationDomain,
                .code = ErrorCode{std::string{code}},
                .defaultSeverity = severity,
                .summary = summary,
                .remediationHint = remediationHint,
                .retryable = false,
                .userActionable = userActionable,
            };
        }
    }  // namespace

    const ErrorCodeDescriptor IdentityInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.identity.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation identity uses its reserved invalid representation.",
        .remediationHint = "Use a non-zero identity issued by the owning navigation or authoring boundary.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor InvalidHandle{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.handle.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation handle is malformed, foreign, or stale.",
        .remediationHint = "Resolve the stable binding again against the active navigation world and topology generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor GenerationExhausted{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.generation.exhausted"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "The navigation identity generation range is exhausted.",
        .remediationHint = "Retire the exhausted slot or world; never wrap or reuse an issued navigation generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CapabilityDescriptorInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.capability.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation capability evidence or bounded query requirement is invalid.",
        .remediationHint = "Use the supported contract version, typed identities, and finite positive query limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CapabilityStale{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.capability.stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Navigation capability evidence changed during query admission.",
        .remediationHint = "Capture the current provider snapshot and repeat complete query admission.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OperationUnsupported{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.operation.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The selected provider does not support the requested navigation operation and quality.",
        .remediationHint = "Select an explicitly supported query quality or compose a provider with the required capability.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CapabilityUnavailable{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.capability.unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required navigation capability is not currently available.",
        .remediationHint = "Restore the selected provider or wait for its owner to publish available capability evidence.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor QueryLimitExceeded{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.query.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation query exceeds a finite provider work, output, or distance limit.",
        .remediationHint = "Reduce the requested bounds or select a provider and project profile declaring sufficient capacity.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AdmissionRejected{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.query.admission_rejected"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Navigation admission rejected the request before creating a handle or job.",
        .remediationHint = "Retry within policy after queue or result-record capacity becomes available.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor QueryCancelled{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.query.cancelled"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "The navigation query was cancelled before publication.",
        .remediationHint = "Submit a new request only if the owning world and caller still require the result.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor StaleSnapshot{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.query.stale_snapshot"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "The navigation result was computed from an old topology generation.",
        .remediationHint = "Revalidate the request against the active topology and resubmit within its bounded policy.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NoNavigationData{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.data.unavailable"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The required navigation world or coverage is unavailable.",
        .remediationHint = "Activate or stream the required navigation data; do not treat missing coverage as a proven no-path result.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ProviderFailed{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.provider.failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The selected navigation provider failed while executing admitted work.",
        .remediationHint = "Inspect the normalized provider category and Horo diagnostics before retrying or replacing the provider.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OutcomeDescriptorInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.outcome.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Navigation terminal outcome evidence is invalid.",
        .remediationHint = "Publish outcomes only through typed factories with valid bounded provenance and coverage evidence.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor InvalidWorld{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.query.invalid_world"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "The accepted navigation request belongs to an invalid or replaced world.",
        .remediationHint = "Resolve the active navigation-world incarnation and submit a new request if still required.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CapacityExceeded{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.query.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Accepted navigation work exceeded a declared execution or publication capacity.",
        .remediationHint = "Reduce query work or retry under a profile with sufficient bounded scratch, result, or retry capacity.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AreaDescriptorInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.area.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A navigation area descriptor is invalid.",
        .remediationHint = "Use stable non-zero identities, typed source provenance, and a finite non-negative traversal cost.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor FilterDescriptorInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.filter.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A navigation query-filter descriptor is invalid.",
        .remediationHint = "Use stable identities, typed source provenance, and finite non-negative area cost overrides.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DescriptorConflict{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.registry.descriptor_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Navigation descriptors collide in one registry identity domain.",
        .remediationHint =
            "Assign unique stable area, filter, and per-filter override identities across project and package contributions.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AreaUnknown{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.area.unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested navigation area identity is not registered.",
        .remediationHint = "Restore the exact project or package area descriptor; do not substitute a default area.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor FilterUnknown{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.filter.unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested navigation query-filter identity is not registered.",
        .remediationHint = "Restore the exact project or package filter descriptor; do not substitute a default filter.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AgentProfileInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.agent_profile.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The grounded navigation agent profile is invalid.",
        .remediationHint = "Use a stable non-zero identity, a display name, and finite grounded dimensions and bake resolution values.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AgentProfileReferenced{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.agent_profile.referenced"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation agent profile still has authored references.",
        .remediationHint = "Choose an explicit surviving replacement profile before deleting the referenced profile.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SourceGeometryInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.source_geometry.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A navigation bake-source geometry snapshot is invalid.",
        .remediationHint = "Provide stable identities, explicit finite coordinate units, finite canonical transforms and vertices, valid "
                           "non-degenerate indexed triangles, and positive bounds.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SourceGeometryUnsupported{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.source_geometry.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation geometry producer kind is unsupported.",
        .remediationHint =
            "Use a canonical static collider, terrain, procedural-generation, or explicitly approved custom producer snapshot.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SourceGeometryCapacityExceeded{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.source_geometry.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Navigation source geometry exceeds a qualified capture bound.",
        .remediationHint = "Reduce the bake scope or use a qualified lower-detail canonical source without truncating geometry.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SourceGeometryStale{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.source_geometry.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Navigation source geometry changed after immutable capture.",
        .remediationHint = "Capture a new complete geometry snapshot before continuing or publishing the bake attempt.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BakeInputInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_input.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The canonical navigation bake input is invalid.",
        .remediationHint = "Use exact non-zero revisions, stable identities, finite non-degenerate bounds and coherent surface partitions.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BakeInputReferenceMissing{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_input.reference_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A canonical navigation bake input reference is missing.",
        .remediationHint = "Resolve every surface, profile, source contribution, filter and area against the same immutable capture.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BakeInputCapacityExceeded{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_input.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Canonical navigation bake input exceeds a qualified capture bound.",
        .remediationHint = "Reduce the bake scope or select a qualified lower-detail source without truncating accepted input.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BakeInputStale{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_input.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Navigation bake input changed before publication.",
        .remediationHint = "Capture the latest complete revision set and source observations before retrying the bake.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BakeInputCancelled{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_input.cancelled"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "Navigation bake input publication was cancelled.",
        .remediationHint = "Submit a new bake only if the owning operation still requires the output.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BakeInputFailed{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_input.failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation bake operation failed before publication.",
        .remediationHint = "Preserve the prior published generation and inspect the owning operation failure before retrying.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BakeInputShuttingDown{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_input.shutting_down"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "Navigation bake publication is closed for application shutdown.",
        .remediationHint = "Do not publish or restart bake work after the application begins shutdown.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BakeJobInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_job.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The staged navigation bake job descriptor is invalid.",
        .remediationHint = "Provide ordered non-empty stages, callbacks, positive work units, and finite positive resource bounds.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BakeJobBudgetExceeded{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_job.budget_exceeded"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The staged navigation bake exceeds a declared resource budget.",
        .remediationHint =
            "Reduce the bake scope or select a profile with sufficient concurrency, memory, temporary storage, item, and work bounds.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BakeJobAdmissionRejected{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.bake_job.admission_rejected"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The operation store or process scheduler rejected the staged navigation bake.",
        .remediationHint = "Retry after bounded operation or scheduler capacity becomes available.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ProjectProfileInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.project_profile.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation project profile or preview preference is invalid.",
        .remediationHint = "Use non-zero typed identities, known capabilities, and positive finite unit-labelled capacities.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProjectProfileStale{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.project_profile.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The navigation project profile changed before replacement or preview resolution.",
        .remediationHint = "Resolve the current project revision and rebuild the complete candidate transaction.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ProjectProfileCapacityExceeded{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.project_profile.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation profile, provider, or usage exceeds an authoritative finite capacity.",
        .remediationHint = "Reduce the requested capacity or select a qualified project profile and provider before activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NavMeshArtifactInvalid =
        MakeNavMeshArtifactError("navigation.navmesh_artifact.invalid", ErrorSeverity::Error,
                                 "The cooked NavMesh artifact schema is invalid.",
                                 "Reject the candidate and recook from validated navigation source data.", true);
    const ErrorCodeDescriptor NavMeshArtifactCorrupt =
        MakeNavMeshArtifactError("navigation.navmesh_artifact.corrupt", ErrorSeverity::Error,
                                 "The cooked NavMesh artifact failed integrity or table validation.",
                                 "Discard the corrupt generation and recook it from authoritative source data.", true);
    const ErrorCodeDescriptor NavMeshArtifactCapacityExceeded =
        MakeNavMeshArtifactError("navigation.navmesh_artifact.capacity_exceeded", ErrorSeverity::Error,
                                 "The cooked NavMesh artifact exceeds a qualified count or byte bound.",
                                 "Reduce the bake scope or select an explicitly qualified higher-capacity profile.", true);
    const ErrorCodeDescriptor UnsupportedCookedVersion =
        MakeNavMeshArtifactError("navigation.navmesh_artifact.unsupported_version", ErrorSeverity::Error,
                                 "The cooked NavMesh format is incompatible with this consumer.",
                                 "Recook with a compatible neutral and provider payload format.", true);
    const ErrorCodeDescriptor NavMeshTileUnknown =
        MakeNavMeshArtifactError("navigation.navmesh_artifact.tile_unknown", ErrorSeverity::Warning,
                                 "The requested NavMesh tile is absent from the artifact.",
                                 "Request an exact published tile coordinate and layer from the artifact manifest.", false);
    const ErrorCodeDescriptor NavMeshProviderPayloadUnavailable =
        MakeNavMeshArtifactError("navigation.navmesh_artifact.provider_payload_unavailable", ErrorSeverity::Warning,
                                 "No provider-private payload matches the requested provider fingerprint.",
                                 "Use the portable NavMesh data or recook an optional payload for the selected provider.", false);
    const ErrorCodeDescriptor NavMeshProviderPayloadIncompatible =
        MakeNavMeshArtifactError("navigation.navmesh_artifact.provider_payload_incompatible", ErrorSeverity::Error,
                                 "The provider-private NavMesh payload is incompatible with the selected provider.",
                                 "Discard the optional payload and use portable data, or recook it for the exact provider format.", true);
    const ErrorCodeDescriptor SourceEnvelopeInvalid =
        MakeNavMeshArtifactError("navigation.source.envelope_invalid", ErrorSeverity::Error,
                                 "The navigation source envelope framing or reserved fields are invalid.",
                                 "Regenerate the source with the canonical navigation serializer and preserve complete envelope bytes.",
                                 true);
    const ErrorCodeDescriptor SourceUnsupportedVersion =
        MakeNavMeshArtifactError("navigation.source.unsupported_version", ErrorSeverity::Error,
                                 "The navigation source schema version is not supported by this consumer.",
                                 "Use an explicit reviewed migration step or open the source with a compatible engine version.", true);
    const ErrorCodeDescriptor SourceDuplicateIdentity =
        MakeNavMeshArtifactError("navigation.source.duplicate_identity", ErrorSeverity::Error,
                                 "Navigation source records contain a duplicate stable identity.",
                                 "Assign one stable identity to each authored source record before serialization.", true);
    const ErrorCodeDescriptor SourceChecksumMismatch =
        MakeNavMeshArtifactError("navigation.source.checksum_mismatch", ErrorSeverity::Error,
                                 "The navigation source envelope checksum does not match its encoded bytes.",
                                 "Discard the corrupt candidate and reload or regenerate the complete source envelope.", true);
    const ErrorCodeDescriptor SourceSerializationCapacityExceeded =
        MakeNavMeshArtifactError("navigation.source.serialization_capacity_exceeded", ErrorSeverity::Error,
                                 "The navigation source envelope exceeds a qualified serialization bound.",
                                 "Reduce the source payload or use an explicitly qualified parser limit without truncating records.", true);
    const ErrorCodeDescriptor SourceUnknownAuthoredRecord =
        MakeNavMeshArtifactError("navigation.source.unknown_authored_record", ErrorSeverity::Error,
                                 "An authored navigation record is unknown or cannot be safely retained.",
                                 "Install the owning record provider or preserve only an optional opaque record under an explicit policy.",
                                 true);
    const ErrorCodeDescriptor GeneratedPayloadQuarantined =
        MakeNavMeshArtifactError("navigation.source.generated_payload_quarantined", ErrorSeverity::Warning,
                                 "A generated navigation payload was retained for inspection but quarantined from activation.",
                                 "Use a provider supporting the exact generated payload or regenerate derived navigation data.", true);
    const ErrorCodeDescriptor SourceMigrationMissing =
        MakeNavMeshArtifactError("navigation.source.migration_missing", ErrorSeverity::Error,
                                 "No explicit navigation source migration edge exists for the requested transition.",
                                 "Supply the reviewed version-to-version migration chain; schema ordering alone is not migration policy.",
                                 true);
    const ErrorCodeDescriptor SourceMigrationInvalid =
        MakeNavMeshArtifactError("navigation.source.migration_invalid", ErrorSeverity::Error,
                                 "The explicit navigation source migration catalog is invalid or ambiguous.",
                                 "Provide distinct forward migration edges with non-null transformations and bounded output records.",
                                 true);
}  // namespace Horo::Navigation::NavigationErrors
