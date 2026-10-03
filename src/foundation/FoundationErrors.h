#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::ConfigurationErrors {
    extern const ErrorCodeDescriptor SchemaInvalid;
    extern const ErrorCodeDescriptor SchemaSealed;
    extern const ErrorCodeDescriptor DraftStale;
    extern const ErrorCodeDescriptor ValueInvalid;
    extern const ErrorCodeDescriptor JsonParseError;
    extern const ErrorCodeDescriptor FileNotFound;
    extern const ErrorCodeDescriptor FileWriteError;
    extern const ErrorCodeDescriptor ResolutionFailed;
    extern const ErrorCodeDescriptor InputTooLarge;
    extern const ErrorCodeDescriptor PersistenceUnavailable;
}  // namespace Horo::ConfigurationErrors

namespace Horo::JobErrors {
    extern const ErrorCodeDescriptor Cancelled;
    extern const ErrorCodeDescriptor Failed;
    extern const ErrorCodeDescriptor InvalidHandle;
    extern const ErrorCodeDescriptor NotFound;
    extern const ErrorCodeDescriptor QueueFull;
    extern const ErrorCodeDescriptor Shutdown;
    extern const ErrorCodeDescriptor TaskGroupClosed;
    extern const ErrorCodeDescriptor WaitForbidden;
    extern const ErrorCodeDescriptor WaitTimedOut;
    extern const ErrorCodeDescriptor WaitCapacityDeadlock;
    extern const ErrorCodeDescriptor InvalidProgress;
    extern const ErrorCodeDescriptor ProgressRegressed;
    extern const ErrorCodeDescriptor TerminalImmutable;
}  // namespace Horo::JobErrors

namespace Horo::HashingErrors {
    extern const ErrorCodeDescriptor InvalidSha256Text;
}  // namespace Horo::HashingErrors

namespace Horo::ModuleDescriptorErrors {
    extern const ErrorCodeDescriptor InvalidSettingsContribution;
    extern const ErrorCodeDescriptor DuplicateSetting;
    extern const ErrorCodeDescriptor SettingOwnerConflict;
    extern const ErrorCodeDescriptor DuplicateEnvironmentBinding;
    extern const ErrorCodeDescriptor InvalidDescriptor;
    extern const ErrorCodeDescriptor DuplicateModule;
    extern const ErrorCodeDescriptor MissingDependency;
    extern const ErrorCodeDescriptor IncompatibleDependency;
    extern const ErrorCodeDescriptor MissingCapability;
    extern const ErrorCodeDescriptor DependencyCycle;
}  // namespace Horo::ModuleDescriptorErrors

namespace Horo::ObservabilityErrors {
    extern const ErrorCodeDescriptor InvalidBundleRequest;
    extern const ErrorCodeDescriptor BundleReadFailed;
    extern const ErrorCodeDescriptor BundleWriteFailed;
    extern const ErrorCodeDescriptor BundleSizeExceeded;
}  // namespace Horo::ObservabilityErrors

namespace Horo::Math::Errors {
    extern const ErrorCodeDescriptor InvalidAffineMatrix;
    extern const ErrorCodeDescriptor InvalidBounds;
    extern const ErrorCodeDescriptor InvalidHomogeneousPoint;
    extern const ErrorCodeDescriptor InvalidPlane;
    extern const ErrorCodeDescriptor InvalidProjection;
    extern const ErrorCodeDescriptor InvalidRay;
    extern const ErrorCodeDescriptor InvalidView;
    extern const ErrorCodeDescriptor NonFiniteInput;
    extern const ErrorCodeDescriptor SingularMatrix;
    extern const ErrorCodeDescriptor ZeroLength;
}  // namespace Horo::Math::Errors

namespace Horo::PathErrors {
    extern const ErrorCodeDescriptor DirectoryCreateFailed;
    extern const ErrorCodeDescriptor PathEscape;
    extern const ErrorCodeDescriptor InvalidPath;
}  // namespace Horo::PathErrors

namespace Horo::ErrorCodeRegistryErrors {
    extern const ErrorCodeDescriptor InvalidDescriptor;
    extern const ErrorCodeDescriptor InvalidNamespace;
    extern const ErrorCodeDescriptor DomainOwnershipConflict;
    extern const ErrorCodeDescriptor DuplicateCode;
    extern const ErrorCodeDescriptor InvalidDeprecation;
}  // namespace Horo::ErrorCodeRegistryErrors

namespace Horo::ValidationErrors {
    extern const ErrorCodeDescriptor InvalidLimits;
    extern const ErrorCodeDescriptor UnknownDiagnostic;
    extern const ErrorCodeDescriptor InvalidSource;
    extern const ErrorCodeDescriptor CapacityExceeded;
    extern const ErrorCodeDescriptor PassClosed;
}  // namespace Horo::ValidationErrors
