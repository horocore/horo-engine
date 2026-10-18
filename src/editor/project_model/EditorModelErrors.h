#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Editor::SceneDocumentErrors {
    extern const ErrorCodeDescriptor HistoryEntryTooLarge;
    extern const ErrorCodeDescriptor InvalidAudioSource;
    extern const ErrorCodeDescriptor InvalidCharacter;
    extern const ErrorCodeDescriptor InvalidAudioListener;
    extern const ErrorCodeDescriptor InvalidBehavior;
    extern const ErrorCodeDescriptor InvalidCamera;
    extern const ErrorCodeDescriptor InvalidLight;
    extern const ErrorCodeDescriptor InvalidName;
    extern const ErrorCodeDescriptor ObjectLocked;
    extern const ErrorCodeDescriptor InvalidPrimitive;
    extern const ErrorCodeDescriptor InvalidPrimitiveMetadata;
    extern const ErrorCodeDescriptor InvalidSavedState;
    extern const ErrorCodeDescriptor InvalidTransform;
    extern const ErrorCodeDescriptor InvalidTriggerVolume;
    extern const ErrorCodeDescriptor NothingToRedo;
    extern const ErrorCodeDescriptor NothingToUndo;
    extern const ErrorCodeDescriptor ObjectNotFound;
    extern const ErrorCodeDescriptor ParentNotFound;
    extern const ErrorCodeDescriptor InvalidPrefabInstance;
    extern const ErrorCodeDescriptor PrefabInstanceNotFound;
    extern const ErrorCodeDescriptor PrimitiveNotCreatable;
    extern const ErrorCodeDescriptor UnknownPrimitive;
}  // namespace Horo::Editor::SceneDocumentErrors

namespace Horo::Editor::SelectionErrors {
    extern const ErrorCodeDescriptor InvalidPrimary;
    extern const ErrorCodeDescriptor ObjectNotFound;
}  // namespace Horo::Editor::SelectionErrors

namespace Horo::Editor::ViewportModelErrors {
    extern const ErrorCodeDescriptor InvalidCamera;
    extern const ErrorCodeDescriptor InvalidFocusBounds;
    extern const ErrorCodeDescriptor InvalidNavigation;
    extern const ErrorCodeDescriptor InvalidLightPreview;
    extern const ErrorCodeDescriptor InvalidProjection;
    extern const ErrorCodeDescriptor InvalidTransformPreview;
}  // namespace Horo::Editor::ViewportModelErrors
