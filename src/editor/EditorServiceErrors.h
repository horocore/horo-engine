#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Editor::SettingsErrors {
    extern const ErrorCodeDescriptor DraftStale;
    extern const ErrorCodeDescriptor PersistenceFailed;
    extern const ErrorCodeDescriptor ValidationFailed;
}  // namespace Horo::Editor::SettingsErrors

namespace Horo::Editor::ProjectCreationErrors {
    extern const ErrorCodeDescriptor UnexpectedFailure;
    extern const ErrorCodeDescriptor DestinationOccupied;
    extern const ErrorCodeDescriptor InvalidRequest;
    extern const ErrorCodeDescriptor JobSubmissionFailed;
    extern const ErrorCodeDescriptor NotFound;
    extern const ErrorCodeDescriptor NotReady;
}  // namespace Horo::Editor::ProjectCreationErrors

namespace Horo::Editor::ProjectMetadataErrors {
    extern const ErrorCodeDescriptor DuplicateKey;
    extern const ErrorCodeDescriptor InvalidJson;
    extern const ErrorCodeDescriptor InvalidSchema;
    extern const ErrorCodeDescriptor InvalidValues;
    extern const ErrorCodeDescriptor NotFound;
    extern const ErrorCodeDescriptor ReadFailed;
    extern const ErrorCodeDescriptor SizeInvalid;
}  // namespace Horo::Editor::ProjectMetadataErrors

namespace Horo::Editor::ProjectOpenErrors {
    extern const ErrorCodeDescriptor Busy;
    extern const ErrorCodeDescriptor NotFound;
    extern const ErrorCodeDescriptor CompatibilityBlocked;
    extern const ErrorCodeDescriptor MigrationPlanMissing;
    extern const ErrorCodeDescriptor MetadataUpdateFailed;
    extern const ErrorCodeDescriptor ScenePreflightFailed;
    extern const ErrorCodeDescriptor DerivedStateFailed;
    extern const ErrorCodeDescriptor Cancelled;
    extern const ErrorCodeDescriptor SessionStale;
    extern const ErrorCodeDescriptor WorkerCapacityInsufficient;
}  // namespace Horo::Editor::ProjectOpenErrors

namespace Horo::Editor::ModalErrors {
    extern const ErrorCodeDescriptor Busy;
    extern const ErrorCodeDescriptor CloseDenied;
    extern const ErrorCodeDescriptor DuplicateId;
    extern const ErrorCodeDescriptor InvalidModal;
    extern const ErrorCodeDescriptor ModalNotTop;
    extern const ErrorCodeDescriptor ParentNotTop;
    extern const ErrorCodeDescriptor StackLimitReached;
}  // namespace Horo::Editor::ModalErrors
