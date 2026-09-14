#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Runtime::SceneErrors {
    extern const ErrorCodeDescriptor InvalidDefinition;
    extern const ErrorCodeDescriptor InvalidAssetDependency;
    extern const ErrorCodeDescriptor ConflictingAssetDependency;
    extern const ErrorCodeDescriptor AssetServicesUnavailable;
    extern const ErrorCodeDescriptor AssetMissing;
    extern const ErrorCodeDescriptor AssetTypeMismatch;
    extern const ErrorCodeDescriptor AssetPayloadEmpty;
    extern const ErrorCodeDescriptor AssetBudgetExceeded;
    extern const ErrorCodeDescriptor AssetRevisionStale;
    extern const ErrorCodeDescriptor AssetLimitsInvalid;
    extern const ErrorCodeDescriptor ServiceShutdown;
    extern const ErrorCodeDescriptor InvalidEntity;
    extern const ErrorCodeDescriptor DuplicateObject;
    extern const ErrorCodeDescriptor ParentNotFound;
    extern const ErrorCodeDescriptor HierarchyCycle;
    extern const ErrorCodeDescriptor StaleEntity;
    extern const ErrorCodeDescriptor StaleView;
    extern const ErrorCodeDescriptor OperationInProgress;
    extern const ErrorCodeDescriptor NoActiveScene;
    extern const ErrorCodeDescriptor InvalidCandidate;
    extern const ErrorCodeDescriptor StructuralCommitFailed;
    extern const ErrorCodeDescriptor SaveBootstrapInvalid;
    extern const ErrorCodeDescriptor SaveBootstrapAssetUnavailable;
    extern const ErrorCodeDescriptor SaveBootstrapIncompatible;
    extern const ErrorCodeDescriptor SaveBootstrapSpawnMissing;
    extern const ErrorCodeDescriptor PersistentIdentityInvalid;
    extern const ErrorCodeDescriptor PersistentIdentityDuplicate;
    extern const ErrorCodeDescriptor PersistentIdentityBindingInvalid;
    extern const ErrorCodeDescriptor PersistentIdentityBindingMissing;
    extern const ErrorCodeDescriptor PersistentIdentityUnknown;
    extern const ErrorCodeDescriptor PersistentIdentityStale;
    extern const ErrorCodeDescriptor PersistentIdentityTombstoned;
    extern const ErrorCodeDescriptor PersistentIdentityAllocationFailed;
}  // namespace Horo::Runtime::SceneErrors
