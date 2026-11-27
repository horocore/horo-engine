#pragma once

#include "Horo/Application/ProjectMigrationErrors.h"
#include "Horo/Foundation/ErrorCode.h"
#include "ProjectVersionErrors.h"

namespace Horo::Application::ProjectErrors {
    extern const ErrorCodeDescriptor RegistryInvalid;
    extern const ErrorCodeDescriptor RegistryDuplicateRelease;
    extern const ErrorCodeDescriptor RegistryBaselineMismatch;
    extern const ErrorCodeDescriptor RegistryPatchContractDrift;
    extern const ErrorCodeDescriptor ProofRejected;
    extern const ErrorCodeDescriptor MetadataNotFound;
    extern const ErrorCodeDescriptor MetadataReadFailed;
    extern const ErrorCodeDescriptor MetadataSizeInvalid;
    extern const ErrorCodeDescriptor MetadataInvalidJson;
    extern const ErrorCodeDescriptor MetadataDuplicateKey;
    extern const ErrorCodeDescriptor MetadataLimitExceeded;
    extern const ErrorCodeDescriptor MetadataInvalidSchema;
    extern const ErrorCodeDescriptor MetadataInvalidValue;
    extern const ErrorCodeDescriptor MigrationCatalogInvalid;
    extern const ErrorCodeDescriptor MigrationDuplicateIdentity;
    extern const ErrorCodeDescriptor MigrationDuplicateEdge;
    extern const ErrorCodeDescriptor MigrationBackwardEdge;
    extern const ErrorCodeDescriptor MigrationCycle;
    extern const ErrorCodeDescriptor MigrationAmbiguous;
    extern const ErrorCodeDescriptor MigrationPathMissing;
    extern const ErrorCodeDescriptor MigrationContractMismatch;
    extern const ErrorCodeDescriptor MigrationProviderMissing;
    extern const ErrorCodeDescriptor MigrationPipelineInvalid;
    extern const ErrorCodeDescriptor MigrationInventoryInvalid;
    extern const ErrorCodeDescriptor MigrationInventoryLimit;
    extern const ErrorCodeDescriptor MigrationDocumentStale;
    extern const ErrorCodeDescriptor MigrationWriteConflict;
    extern const ErrorCodeDescriptor MigrationLocked;
    extern const ErrorCodeDescriptor MigrationInputChanged;
    extern const ErrorCodeDescriptor MigrationCapacityInsufficient;
    extern const ErrorCodeDescriptor MigrationPublishFailed;
    extern const ErrorCodeDescriptor MigrationRecoveryFailed;
}  // namespace Horo::Application::ProjectErrors
