#include "Horo/Gameplay/GameplayErrors.h"

namespace Horo::Gameplay::GameplayErrors {
    namespace {
        const ErrorDomainId GameplayDomain{"horo.gameplay"};
    }

    const ErrorCodeDescriptor InvalidGameAssetTypeId{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.asset_type_id_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The game-owned asset type ID is invalid.",
        .remediationHint = "Use a lowercase game.<module>.<asset_type> identifier.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidGameAssetDescriptor{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.asset_descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The game-owned asset descriptor is incomplete or invalid.",
        .remediationHint = "Declare bounded editor metadata, source extensions, cook targets, and complete callbacks.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidSerializedGameAsset{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.serialized_asset_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The serialized game-owned asset envelope is invalid.",
        .remediationHint = "Repair its stable identity, schema version, encoding, or bounded opaque payload.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DuplicateGameAssetType{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.asset_type_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A game-owned asset type ID is duplicated.",
        .remediationHint = "Give every project-owned asset type one unique stable ID.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor GameAssetRegistryFrozen{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.asset_registry_frozen"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The game-owned asset registry is frozen.",
        .remediationHint = "Register complete asset metadata before module startup.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor GameAssetHandlerUnavailable{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.asset_handler_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The game-owned asset handler is unavailable.",
        .remediationHint = "Restore compatible project code before importing, editing, or cooking this asset.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidGameAssetProcessingInput{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.asset_processing_input_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The game-owned asset processing input is invalid or unsupported.",
        .remediationHint = "Use a declared source extension, current schema, supported target, and bounded input.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor GameAssetProcessingFailed{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.asset_processing_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A game-owned asset processing callback failed its boundary contract.",
        .remediationHint = "Fix the project callback and retry without replacing the preserved authored payload.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidComponentTypeId{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.component_type_id_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay component type ID is invalid.",
        .remediationHint = "Use a lowercase game.<module>.<component> identifier.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidComponentDescriptor{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.component_descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay component descriptor is invalid.",
        .remediationHint = "Provide a stable identity, current schema, and unique bounded property metadata.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidSerializedComponent{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.serialized_component_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The serialized gameplay component envelope is invalid.",
        .remediationHint = "Repair its stable identity, schema version, or bounded opaque payload.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidComponentMigration{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.component_migration_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay component migration metadata is invalid or ambiguous.",
        .remediationHint = "Declare unique forward-only migration edges ending at the current schema.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DuplicateComponentType{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.component_type_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A gameplay component type ID is duplicated.",
        .remediationHint = "Give every project-owned component one unique stable ID.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ComponentRegistryFrozen{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.component_registry_frozen"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay component registry is frozen.",
        .remediationHint = "Register complete component metadata before module startup.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor InvalidSystemId{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.system_id_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay system ID is invalid.",
        .remediationHint = "Use a lowercase game.<module>.<system> identifier.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidServiceId{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.service_id_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay service ID is invalid.",
        .remediationHint = "Use a lowercase game.<module>.<service> identifier.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidCapabilityId{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.capability_id_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay capability ID is invalid.",
        .remediationHint = "Use a lowercase identity with at least three namespace segments.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidSystemDescriptor{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.system_descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay system descriptor is incomplete or invalid.",
        .remediationHint = "Declare bounded typed access, dependencies, affinity, and a complete factory binding.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidServiceDescriptor{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.service_descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay service descriptor is incomplete or invalid.",
        .remediationHint = "Declare bounded dependencies, capabilities, lifecycle policy, and a complete factory binding.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DuplicateSystem{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.system_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A gameplay system ID is duplicated.",
        .remediationHint = "Give every registered project system one unique stable ID.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DuplicateService{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.service_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A gameplay service ID is duplicated.",
        .remediationHint = "Give every registered project service one unique stable ID.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SystemDependencyMissing{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.system_dependency_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A gameplay system scheduling or service dependency is unavailable.",
        .remediationHint = "Register every required system and service before freezing the transaction.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ServiceDependencyMissing{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.service_dependency_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A gameplay service dependency is unavailable.",
        .remediationHint = "Register every required service before freezing the transaction.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CapabilityMissing{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.capability_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required gameplay capability has no active provider.",
        .remediationHint = "Register or compose an explicit provider for every required capability.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SystemScheduleCycle{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.system_schedule_cycle"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Gameplay system scheduling dependencies contain a cycle.",
        .remediationHint = "Remove one after/before edge so the phase schedule is acyclic.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ServiceDependencyCycle{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.service_dependency_cycle"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Gameplay service dependencies contain a cycle.",
        .remediationHint = "Remove one dependency edge so providers can start before dependants.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SystemAccessConflict{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.system_access_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Two unordered gameplay systems have conflicting component access.",
        .remediationHint = "Declare an explicit dependency edge between same-phase systems that share writable state.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ServiceScopeViolation{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.service_scope_violation"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A gameplay service depends on a shorter-lived service.",
        .remediationHint = "Keep project services independent of scene-scoped service instances.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RegistrationRegistryFrozen{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.registration_registry_frozen"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay registration registry is frozen.",
        .remediationHint = "Register complete system and service metadata before module startup.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor GameplayFactoryFailed{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.factory_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A gameplay system or service factory failed to create an instance.",
        .remediationHint = "Fix the project factory and retry activation without changing the active generation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor GameplayRuntimeInactive{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.runtime_inactive"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay registration runtime is not active.",
        .remediationHint = "Create a new runtime generation before dispatching registered systems.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor GameplayCancelled{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.cancelled"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "The gameplay runtime generation was cancelled.",
        .remediationHint = "Do not publish late work into a stopped or replaced runtime generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor GameplayThreadAccessViolation{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.thread_access_violation"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A gameplay callback was invoked from an undeclared execution domain.",
        .remediationHint = "Dispatch the callback on its declared affinity or mark a genuinely thread-safe callback as Any.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor GameplayReloadRestartRequired{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.reload_restart_required"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The gameplay module cannot prove that native unload is safe.",
        .remediationHint = "Stop Play and restart the editor before activating the replacement module.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor GameplayReloadSnapshotInvalid{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.reload_snapshot_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The native gameplay reload snapshot is invalid or exceeds its bound.",
        .remediationHint = "Fix the module reload adapter and retry from the last working generation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor GameplayReloadRestoreFailed{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.reload_restore_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The replacement gameplay generation could not restore captured state.",
        .remediationHint = "Restore the previous generation or stop Play without discarding authored data.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidBehaviorTypeId{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.behavior_type_id_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The behavior type ID is invalid.",
        .remediationHint = "Use a lowercase game.<module>.<behavior> identifier.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidBehaviorInstanceId{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.behavior_instance_id_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The behavior instance ID is invalid.",
        .remediationHint = "Assign a non-zero stable attachment identity.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidBehaviorComponent{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.behavior_component_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The behavior component payload is invalid.",
        .remediationHint = "Repair the behavior schema version or bounded authoring fields.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DuplicateBehaviorInstance{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.behavior_instance_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A behavior instance ID is duplicated.",
        .remediationHint = "Regenerate one of the attachment identities.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DuplicateBehaviorType{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.behavior_type_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A behavior type ID is duplicated.",
        .remediationHint = "Give each registered behavior a unique stable ID.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RegistryFrozen{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.registry_frozen"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The behavior registry is frozen.",
        .remediationHint = "Register complete descriptors before activating a runtime scene.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BehaviorNotRegistered{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.behavior_not_registered"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The behavior type is not registered.",
        .remediationHint = "Build or restore the behavior implementation before entering Play Mode.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BehaviorMultiplicityViolation{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.behavior_multiplicity_violation"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The object contains too many instances of this behavior type.",
        .remediationHint = "Remove the duplicate attachment or enable allowMultiple.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor EventQueueFull{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.event_queue_full"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The scene gameplay event queue is full.",
        .remediationHint = "Reduce event traffic or increase the explicit scene event budget.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidEvent{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.event_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay event is invalid.",
        .remediationHint = "Use a registered event type, valid target, and bounded schema payload.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidGameModuleDescriptor{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.module_descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay module descriptor is malformed.",
        .remediationHint = "Rebuild the project gameplay module from valid generated inputs.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor IncompatibleGameModule{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.module_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay module does not match the requested build identity.",
        .remediationHint = "Rebuild the gameplay module with the active Horo SDK and project manifest.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidGeneratedDescriptorBundle{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.generated_descriptor_bundle_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The generated gameplay descriptor bundle is malformed or incomplete.",
        .remediationHint = "Regenerate the complete descriptor bundle and rebuild the gameplay module.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor GeneratedDescriptorDiagnosticsPresent{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.generated_descriptor_diagnostics_present"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The generated gameplay descriptor bundle contains blocking diagnostics.",
        .remediationHint = "Resolve every generated descriptor diagnostic before activating gameplay code.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidReplicationRegistration{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.replication_registration_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The native gameplay replication registration is invalid.",
        .remediationHint =
            "Declare an existing gameplay owner, deterministic owner-thread safe points, bounded access and an exact module-owned schema.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DuplicateReplicationSchema{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.replication_schema_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A native gameplay replication schema is registered more than once.",
        .remediationHint = "Give each schema one gameplay owner and one registration in the module transaction.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationOwnerMissing{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.replication_owner_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A replication registration names gameplay metadata that is not registered.",
        .remediationHint = "Register the component, behavior, service and every accessed component before freezing replication metadata.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationRegistryFrozen{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.replication_registry_frozen"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The native gameplay replication registry is frozen.",
        .remediationHint = "Register complete replication metadata before module startup.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::Gameplay::GameplayErrors
