#include "EditorModelErrors.h"

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SceneDocumentDomain{"horo.editor.scene_document"};
        const ErrorDomainId SelectionDomain{"horo.editor.selection"};
        const ErrorDomainId ViewportDomain{"horo.editor.viewport"};
    }  // namespace

    namespace SceneDocumentErrors {
        const ErrorCodeDescriptor InvalidCharacter{.domain = ErrorDomainId{"horo.editor.scene"},
                                                   .code = ErrorCode{"editor.scene.character.invalid"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Character component is missing or invalid.",
                                                   .remediationHint =
                                                       "Select an existing Character component and supply valid explicit settings."};
        const ErrorCodeDescriptor HistoryEntryTooLarge{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.history_entry_too_large"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene history entry exceeds the memory budget.",
            .remediationHint = "Reduce the number of objects affected by one command.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidAudioSource{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_audio_source"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene audio source is invalid.",
            .remediationHint = "Use finite supported audio-source values.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidBehavior{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_behavior"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene behavior attachment is invalid.",
            .remediationHint = "Use stable behavior identities and bounded schema-versioned fields.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidCamera{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_camera"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene camera is invalid.",
            .remediationHint = "Use finite camera values and valid clip planes.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidLight{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_light"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene light is invalid.",
            .remediationHint = "Use finite supported light values.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidName{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_name"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene object name is invalid.",
            .remediationHint = "Use a non-empty name within the supported byte limit.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor ObjectLocked{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.object_locked"},
            .defaultSeverity = ErrorSeverity::Warning,
            .summary = "Scene object is locked in the editor.",
            .remediationHint = "Unlock the object or its locked ancestor in the Hierarchy before editing it.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidPrimitive{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_primitive"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene primitive descriptor is invalid.",
            .remediationHint = "Use a supported primitive descriptor.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidPrimitiveMetadata{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_primitive_metadata"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene primitive metadata is invalid.",
            .remediationHint = "Provide the typed authoring descriptor required by the primitive.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidSavedState{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_saved_state"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Saved scene state is invalid.",
            .remediationHint = "Use a revision and state from the active document session.",
            .retryable = false,
            .userActionable = false,
        };
        const ErrorCodeDescriptor InvalidTransform{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_transform"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene object transform is invalid.",
            .remediationHint = "Use a finite transform with a valid rotation.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidTriggerVolume{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_trigger_volume"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene trigger volume is invalid.",
            .remediationHint = "Use a supported collider shape.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor NothingToRedo{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.nothing_to_redo"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "No scene command is available to redo.",
            .remediationHint = "Commit or undo a command before requesting redo.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor NothingToUndo{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.nothing_to_undo"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "No scene command is available to undo.",
            .remediationHint = "Commit a scene command before requesting undo.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor ObjectNotFound{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.object_not_found"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene object was not found.",
            .remediationHint = "Refresh object identity from the active document.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor ParentNotFound{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.parent_not_found"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene object parent was not found.",
            .remediationHint = "Use an existing parent object or no parent.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidPrefabInstance{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.invalid_prefab_instance"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene prefab instance is invalid.",
            .remediationHint = "Use a stable instance ID, valid prefab AssetId, finite root transform, and containing-scene parent.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor PrefabInstanceNotFound{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.prefab_instance_not_found"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene prefab instance was not found.",
            .remediationHint = "Refresh the stable instance identity from the active document.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor PrimitiveNotCreatable{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.primitive_not_creatable"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Primitive cannot be created as a scene object.",
            .remediationHint = "Choose a primitive exposed by the hierarchy creation catalog.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor UnknownPrimitive{
            .domain = SceneDocumentDomain,
            .code = ErrorCode{"scene_document.unknown_primitive"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Primitive is not registered.",
            .remediationHint = "Select a primitive from the active registry.",
            .retryable = false,
            .userActionable = true,
        };
    }  // namespace SceneDocumentErrors

    namespace SelectionErrors {
        const ErrorCodeDescriptor InvalidPrimary{
            .domain = SelectionDomain,
            .code = ErrorCode{"editor.selection.invalid_primary"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Primary selection is invalid.",
            .remediationHint = "Choose the primary object from the selected object set.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor ObjectNotFound{
            .domain = SelectionDomain,
            .code = ErrorCode{"editor.selection.object_not_found"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Selected scene object was not found.",
            .remediationHint = "Refresh selection from the active document.",
            .retryable = false,
            .userActionable = true,
        };
    }  // namespace SelectionErrors

    namespace ViewportModelErrors {
        const ErrorCodeDescriptor InvalidCamera{
            .domain = ViewportDomain,
            .code = ErrorCode{"editor.viewport.invalid_camera"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Viewport camera is invalid.",
            .remediationHint = "Use finite camera values and valid projection ranges.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidFocusBounds{
            .domain = ViewportDomain,
            .code = ErrorCode{"editor.viewport.invalid_focus_bounds"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Viewport focus bounds are invalid.",
            .remediationHint = "Provide finite ordered focus bounds.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidNavigation{
            .domain = ViewportDomain,
            .code = ErrorCode{"editor.viewport.invalid_navigation"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Viewport navigation delta is invalid.",
            .remediationHint = "Use finite navigation values and a positive dolly scale.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidLightPreview{
            .domain = ViewportDomain,
            .code = ErrorCode{"editor.viewport.invalid_light_preview"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Viewport light preview is invalid.",
            .remediationHint = "Use an existing object and a valid Light component.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidProjection{
            .domain = ViewportDomain,
            .code = ErrorCode{"editor.viewport.invalid_projection"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Viewport projection is invalid.",
            .remediationHint = "Use supported finite projection parameters.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidTransformPreview{
            .domain = ViewportDomain,
            .code = ErrorCode{"editor.viewport.invalid_transform_preview"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Viewport transform preview is invalid.",
            .remediationHint = "Use an existing object and a finite transform.",
            .retryable = false,
            .userActionable = true,
        };
    }  // namespace ViewportModelErrors
}  // namespace Horo::Editor
