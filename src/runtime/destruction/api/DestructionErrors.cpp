#include "Horo/Destruction/DestructionErrors.h"

namespace Horo::Destruction::DestructionErrors {
    namespace {
        const ErrorDomainId DestructionDomain{"horo.destruction"};
    }

    const ErrorCodeDescriptor IdentityInvalid{.domain = DestructionDomain,
                                              .code = ErrorCode{"destruction.identity.invalid"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The destruction identity uses a reserved or incomplete representation.",
                                              .remediationHint = "Use identities issued by the owning destruction or asset boundary."};
    const ErrorCodeDescriptor IdentityUnknown{.domain = DestructionDomain,
                                              .code = ErrorCode{"destruction.identity.unknown"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "The destruction identity belongs to a different logical owner.",
                                              .remediationHint =
                                                  "Resolve the identity only against its exact asset, world, and destructible owner."};
    const ErrorCodeDescriptor StaleGeneration{.domain = DestructionDomain,
                                              .code = ErrorCode{"destruction.identity.stale_generation"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "The destruction identity belongs to a retired runtime generation.",
                                              .remediationHint = "Discard stale work and resolve the current destructible generation."};
    const ErrorCodeDescriptor StaleContent{.domain = DestructionDomain,
                                           .code = ErrorCode{"destruction.identity.stale_content"},
                                           .defaultSeverity = ErrorSeverity::Warning,
                                           .summary = "The destruction identity belongs to replaced fracture content.",
                                           .remediationHint =
                                               "Resolve the current artifact content and migrate stable chunk identities explicitly."};
    const ErrorCodeDescriptor StaleRevision{.domain = DestructionDomain,
                                            .code = ErrorCode{"destruction.identity.stale_revision"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "The destruction identity belongs to a retired semantic revision.",
                                            .remediationHint = "Re-query the current immutable destruction snapshot before continuing."};
    const ErrorCodeDescriptor GenerationExhausted{.domain = DestructionDomain,
                                                  .code = ErrorCode{"destruction.identity.generation_exhausted"},
                                                  .defaultSeverity = ErrorSeverity::Critical,
                                                  .summary = "The destruction generation cannot advance without wrapping.",
                                                  .remediationHint = "Retire the exhausted owner permanently; never reuse its generation."};
    const ErrorCodeDescriptor RevisionExhausted{.domain = DestructionDomain,
                                                .code = ErrorCode{"destruction.identity.revision_exhausted"},
                                                .defaultSeverity = ErrorSeverity::Critical,
                                                .summary = "The destruction revision cannot advance without wrapping.",
                                                .remediationHint = "Reject the transition and replace the owning generation explicitly."};
    const ErrorCodeDescriptor SerializedIdentityInvalid{.domain = DestructionDomain,
                                                        .code = ErrorCode{"destruction.identity.serialized_invalid"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Serialized destruction identity bytes are malformed.",
                                                        .remediationHint = "Reject or recook the malformed identity payload."};
    const ErrorCodeDescriptor DescriptorInvalid{.domain = DestructionDomain,
                                                .code = ErrorCode{"destruction.descriptor.invalid"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "The destructible descriptor contains malformed or contradictory policy.",
                                                .remediationHint = "Correct the typed policy and exact identity fields."};
    const ErrorCodeDescriptor TierInvalid{.domain = DestructionDomain,
                                          .code = ErrorCode{"destruction.tier.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The destruction feature tier is unknown.",
                                          .remediationHint = "Select an exact provider-neutral tier defined by the current contract."};
    const ErrorCodeDescriptor FeatureUnsatisfied{.domain = DestructionDomain,
                                                 .code = ErrorCode{"destruction.feature.unsatisfied"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The exact selected tier does not satisfy a required destruction feature.",
                                                 .remediationHint =
                                                     "Select an explicitly allowed compatible profile or revise the requirement."};
    const ErrorCodeDescriptor
        RuntimeGeometryUnsupported{.domain = DestructionDomain,
                                   .code = ErrorCode{"destruction.feature.runtime_geometry_unsupported"},
                                   .defaultSeverity = ErrorSeverity::Error,
                                   .summary = "Runtime geometry generation is unavailable in the core pre-cooked destruction contract.",
                                   .remediationHint =
                                       "Provide compatible pre-cooked fracture content; do not request runtime cutting as fallback."};
    const ErrorCodeDescriptor LimitProfileInvalid{.domain = DestructionDomain,
                                                  .code = ErrorCode{"destruction.limits.profile_invalid"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "The destruction limit profile is empty, contradictory, or above its ceiling.",
                                                  .remediationHint = "Use positive limits no wider than the exact selected tier."};
    const ErrorCodeDescriptor LimitExceeded{.domain = DestructionDomain,
                                            .code = ErrorCode{"destruction.limits.exceeded"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "The destruction artifact or peak work exceeds the admitted finite limits.",
                                            .remediationHint =
                                                "Reject or recook the content for a compatible explicitly selected profile."};
    const ErrorCodeDescriptor StaleConfiguration{.domain = DestructionDomain,
                                                 .code = ErrorCode{"destruction.configuration.stale"},
                                                 .defaultSeverity = ErrorSeverity::Warning,
                                                 .summary = "The destructible descriptor belongs to a replaced configuration revision.",
                                                 .remediationHint = "Resolve the current immutable descriptor before admitting work."};
    const ErrorCodeDescriptor StateInvalid{.domain = DestructionDomain,
                                           .code = ErrorCode{"destruction.state.invalid"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The destruction state candidate violates canonical state-machine invariants.",
                                           .remediationHint =
                                               "Discard the candidate and prepare it again from the current immutable snapshot."};
    const ErrorCodeDescriptor InvalidDamage{.domain = DestructionDomain,
                                            .code = ErrorCode{"destruction.command.invalid_damage"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "The destruction damage command is zero, negative, or non-finite.",
                                            .remediationHint = "Submit finite positive canonical health units."};
    const ErrorCodeDescriptor DamageCooldownActive{.domain = DestructionDomain,
                                                   .code = ErrorCode{"destruction.damage.cooldown_active"},
                                                   .defaultSeverity = ErrorSeverity::Warning,
                                                   .summary = "The destructible has not reached its next admitted damage tick.",
                                                   .remediationHint = "Retry at or after the configured fixed-tick interval."};
    const ErrorCodeDescriptor DuplicateCommand{.domain = DestructionDomain,
                                               .code = ErrorCode{"destruction.command.duplicate_conflict"},
                                               .defaultSeverity = ErrorSeverity::Warning,
                                               .summary = "A destruction command identity was reused with conflicting semantics.",
                                               .remediationHint =
                                                   "Retry the exact original command or issue a new identity for changed work."};
    const ErrorCodeDescriptor StateTerminal{.domain = DestructionDomain,
                                            .code = ErrorCode{"destruction.state.terminal"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "The destruction generation is already in its terminal Destroyed state.",
                                            .remediationHint =
                                                "Replace the destructible generation explicitly instead of mutating terminal state."};
    const ErrorCodeDescriptor
        CancelledBeforeCommit{.domain = DestructionDomain,
                              .code = ErrorCode{"destruction.transition.cancelled_before_commit"},
                              .defaultSeverity = ErrorSeverity::Info,
                              .summary = "Detached destruction work was cancelled before owner-safe commit.",
                              .remediationHint = "Discard its candidate resources; prepare new work from the current snapshot if needed."};
    const ErrorCodeDescriptor ShutdownInProgress{.domain = DestructionDomain,
                                                 .code = ErrorCode{"destruction.lifecycle.shutdown_in_progress"},
                                                 .defaultSeverity = ErrorSeverity::Warning,
                                                 .summary = "The destruction owner has closed mutation admission for shutdown.",
                                                 .remediationHint = "Stop submitting work and allow exact-generation readers to drain."};
    const ErrorCodeDescriptor CommandInvalid{.domain = DestructionDomain,
                                             .code = ErrorCode{"destruction.command.invalid"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The destruction command contains malformed typed input.",
                                             .remediationHint =
                                                 "Submit a current schema with valid identities, finite vectors, and a non-zero tick."};
    const ErrorCodeDescriptor CommandLimitExceeded{.domain = DestructionDomain,
                                                   .code = ErrorCode{"destruction.command.limit_exceeded"},
                                                   .defaultSeverity = ErrorSeverity::Warning,
                                                   .summary = "The destruction command exceeds an admitted finite input limit.",
                                                   .remediationHint = "Clamp or split the request according to the active product limits."};
    const ErrorCodeDescriptor CommandAuthorityDenied{.domain = DestructionDomain,
                                                     .code = ErrorCode{"destruction.command.authority_denied"},
                                                     .defaultSeverity = ErrorSeverity::Warning,
                                                     .summary = "The destruction authority grant cannot issue this command.",
                                                     .remediationHint =
                                                         "Resolve the current grant and request only explicitly granted capabilities."};
    const ErrorCodeDescriptor CommandUnsupported{.domain = DestructionDomain,
                                                 .code = ErrorCode{"destruction.command.unsupported"},
                                                 .defaultSeverity = ErrorSeverity::Warning,
                                                 .summary = "The current destruction policy does not support this command kind.",
                                                 .remediationHint =
                                                     "Use a command admitted by the exact descriptor and capability snapshot."};
    const ErrorCodeDescriptor CommandResultInvalid{.domain = DestructionDomain,
                                                   .code = ErrorCode{"destruction.command.result_invalid"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The destruction command terminal result is internally inconsistent.",
                                                   .remediationHint =
                                                       "Discard the result and terminate the exact command with a valid typed outcome."};
    const ErrorCodeDescriptor RegistryInvalid{.domain = DestructionDomain,
                                              .code = ErrorCode{"destruction.registry.invalid"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The destruction registry input or bounded query is malformed.",
                                              .remediationHint =
                                                  "Use valid immutable records and non-zero limits within the registry ceilings."};
    const ErrorCodeDescriptor RegistryDuplicate{.domain = DestructionDomain,
                                                .code = ErrorCode{"destruction.registry.duplicate"},
                                                .defaultSeverity = ErrorSeverity::Warning,
                                                .summary = "The destruction registry already contains this target or authored owner.",
                                                .remediationHint =
                                                    "Update the owner publication or use explicit next-generation replacement."};
    const ErrorCodeDescriptor RegistryCapacityExceeded{.domain = DestructionDomain,
                                                       .code = ErrorCode{"destruction.registry.capacity_exceeded"},
                                                       .defaultSeverity = ErrorSeverity::Warning,
                                                       .summary = "The destruction registry operation exceeds an explicit finite bound.",
                                                       .remediationHint =
                                                           "Reduce the registration or query count; never truncate implicitly."};
    const ErrorCodeDescriptor
        CompositionInvalid{.domain = DestructionDomain,
                           .code = ErrorCode{"destruction.composition.invalid"},
                           .defaultSeverity = ErrorSeverity::Error,
                           .summary = "The destruction product composition contains malformed explicit evidence.",
                           .remediationHint = "Supply one canonical fact per capability and a known profile, revision, and lifecycle."};
    const ErrorCodeDescriptor CompositionCapabilityUnavailable{.domain = DestructionDomain,
                                                               .code = ErrorCode{"destruction.composition.capability_unavailable"},
                                                               .defaultSeverity = ErrorSeverity::Error,
                                                               .summary =
                                                                   "A capability required by the exact destruction profile is unavailable.",
                                                               .remediationHint = "Install the capability explicitly or select a different "
                                                                                  "product profile; no fallback occurs."};
    const ErrorCodeDescriptor CompositionStale{.domain = DestructionDomain,
                                               .code = ErrorCode{"destruction.composition.stale"},
                                               .defaultSeverity = ErrorSeverity::Warning,
                                               .summary = "The destruction composition revision was replaced.",
                                               .remediationHint = "Discard captured work and resolve the current immutable composition."};
}  // namespace Horo::Destruction::DestructionErrors
