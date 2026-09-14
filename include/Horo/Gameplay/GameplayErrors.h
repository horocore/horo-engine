#pragma once

/**
 * @file GameplayErrors.h
 * @brief Typed errors shared by gameplay authoring and runtime validation.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Gameplay::GameplayErrors {
    extern const ErrorCodeDescriptor InvalidGameAssetTypeId;
    extern const ErrorCodeDescriptor InvalidGameAssetDescriptor;
    extern const ErrorCodeDescriptor InvalidSerializedGameAsset;
    extern const ErrorCodeDescriptor DuplicateGameAssetType;
    extern const ErrorCodeDescriptor GameAssetRegistryFrozen;
    extern const ErrorCodeDescriptor GameAssetHandlerUnavailable;
    extern const ErrorCodeDescriptor InvalidGameAssetProcessingInput;
    extern const ErrorCodeDescriptor GameAssetProcessingFailed;
    extern const ErrorCodeDescriptor InvalidComponentTypeId;
    extern const ErrorCodeDescriptor InvalidComponentDescriptor;
    extern const ErrorCodeDescriptor InvalidSerializedComponent;
    extern const ErrorCodeDescriptor InvalidComponentMigration;
    extern const ErrorCodeDescriptor DuplicateComponentType;
    extern const ErrorCodeDescriptor ComponentRegistryFrozen;
    extern const ErrorCodeDescriptor InvalidSystemId;
    extern const ErrorCodeDescriptor InvalidServiceId;
    extern const ErrorCodeDescriptor InvalidCapabilityId;
    extern const ErrorCodeDescriptor InvalidSystemDescriptor;
    extern const ErrorCodeDescriptor InvalidServiceDescriptor;
    extern const ErrorCodeDescriptor DuplicateSystem;
    extern const ErrorCodeDescriptor DuplicateService;
    extern const ErrorCodeDescriptor SystemDependencyMissing;
    extern const ErrorCodeDescriptor ServiceDependencyMissing;
    extern const ErrorCodeDescriptor CapabilityMissing;
    extern const ErrorCodeDescriptor SystemScheduleCycle;
    extern const ErrorCodeDescriptor ServiceDependencyCycle;
    extern const ErrorCodeDescriptor SystemAccessConflict;
    extern const ErrorCodeDescriptor ServiceScopeViolation;
    extern const ErrorCodeDescriptor RegistrationRegistryFrozen;
    extern const ErrorCodeDescriptor GameplayFactoryFailed;
    extern const ErrorCodeDescriptor GameplayRuntimeInactive;
    extern const ErrorCodeDescriptor GameplayCancelled;
    extern const ErrorCodeDescriptor GameplayThreadAccessViolation;
    extern const ErrorCodeDescriptor GameplayReloadRestartRequired;
    extern const ErrorCodeDescriptor GameplayReloadSnapshotInvalid;
    extern const ErrorCodeDescriptor GameplayReloadRestoreFailed;
    extern const ErrorCodeDescriptor InvalidBehaviorTypeId;
    extern const ErrorCodeDescriptor InvalidBehaviorInstanceId;
    extern const ErrorCodeDescriptor InvalidBehaviorComponent;
    extern const ErrorCodeDescriptor DuplicateBehaviorInstance;
    extern const ErrorCodeDescriptor DuplicateBehaviorType;
    extern const ErrorCodeDescriptor RegistryFrozen;
    extern const ErrorCodeDescriptor BehaviorNotRegistered;
    extern const ErrorCodeDescriptor BehaviorMultiplicityViolation;
    extern const ErrorCodeDescriptor EventQueueFull;
    extern const ErrorCodeDescriptor InvalidEvent;
    extern const ErrorCodeDescriptor InvalidGameModuleDescriptor;
    extern const ErrorCodeDescriptor IncompatibleGameModule;
    extern const ErrorCodeDescriptor InvalidGeneratedDescriptorBundle;
    extern const ErrorCodeDescriptor GeneratedDescriptorDiagnosticsPresent;
    extern const ErrorCodeDescriptor InvalidReplicationRegistration;
    extern const ErrorCodeDescriptor DuplicateReplicationSchema;
    extern const ErrorCodeDescriptor ReplicationOwnerMissing;
    extern const ErrorCodeDescriptor ReplicationRegistryFrozen;
}  // namespace Horo::Gameplay::GameplayErrors
