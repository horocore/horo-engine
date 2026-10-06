#pragma once
/** @file EditorActivityAbi.h @brief Versioned C11 data/session transport for host-rendered activity destinations and drawers. */
#include "Horo/Extensions/ExtensionAbi.h"

/** @brief Canonical required host capability for the ABI 1.4 editor registration/session transport. */
#define HORO_EDITOR_ACTIVITY_HOST_CAPABILITY "editor.activity"

enum {
    HORO_EDITOR_ACTIVITY_SCHEMA_VERSION = 1,
    HORO_EDITOR_ACTIVITY_SESSION_VERSION = 1
};

enum HoroEditorActivitySideCode {
    HORO_EDITOR_ACTIVITY_LEFT = 0,
    HORO_EDITOR_ACTIVITY_RIGHT = 1,
    HORO_EDITOR_ACTIVITY_BOTTOM = 2
};

/** @brief Explicitly advertised subset of the standard EditorUiForm node vocabulary in session version 1. */
enum HoroEditorActivityNodeCode {
    HORO_EDITOR_ACTIVITY_TEXT = 0,
    HORO_EDITOR_ACTIVITY_LABEL = 1,
    HORO_EDITOR_ACTIVITY_ACTION = 2,
    HORO_EDITOR_ACTIVITY_STACK = 3,
    HORO_EDITOR_ACTIVITY_GROUP = 4
};

enum HoroEditorActivityNodeFlags {
    HORO_EDITOR_ACTIVITY_NODE_DISABLED = 1U,
    HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL = 2U
};

enum HoroEditorActivityPresentationFlags {
    HORO_EDITOR_ACTIVITY_HIDDEN = 1U,
    HORO_EDITOR_ACTIVITY_DISABLED = 2U
};

/** @brief Borrowed standard form node; strings and array memory are copied before returning. */
struct HoroEditorActivityNode {
    // Read across the C/C++ package boundary by CopyActivityForm/CopyNode in EditorActivityAbiConversion.cpp.
    // cppcheck-suppress unusedStructMember
    uint32_t structSize;
    // Read across the C/C++ package boundary by CopyActivityForm/CopyNode in EditorActivityAbiConversion.cpp.
    // cppcheck-suppress unusedStructMember
    uint32_t kind;
    // Read across the C/C++ package boundary by CopyActivityForm/CopyNode in EditorActivityAbiConversion.cpp.
    // cppcheck-suppress unusedStructMember
    uint32_t flags;
    HoroExtensionStringView id;
    HoroExtensionStringView parent;
    HoroExtensionStringView labelKey;
    HoroExtensionStringView text;
    HoroExtensionStringView actionId;
};
typedef struct HoroEditorActivityNode HoroEditorActivityNode;

/** @brief Bounded immutable view snapshot; revisions strictly increase within an activation. */
struct HoroEditorActivitySnapshot {
    // Read across the C/C++ package boundary by CopyActivityForm in EditorActivityAbiConversion.cpp.
    // cppcheck-suppress unusedStructMember
    uint32_t structSize;
    // Read across the C/C++ package boundary by CopyActivityForm and EditorActivitySession::Publish.
    // cppcheck-suppress unusedStructMember
    uint32_t schemaVersion;
    // Read across the C/C++ package boundary by CopyActivityForm and EditorActivitySession::Publish.
    // cppcheck-suppress unusedStructMember
    uint64_t revision;
    // Read across the C/C++ package boundary by CopyActivityForm and EditorActivitySession::Publish.
    // cppcheck-suppress unusedStructMember
    uint32_t presentationFlags;
    // Read across the C/C++ package boundary by CopyActivityForm and EditorActivitySession::Publish.
    // cppcheck-suppress unusedStructMember
    uint32_t badgeCount;
    // Read across the C/C++ package boundary by CopyActivityForm in EditorActivityAbiConversion.cpp.
    // cppcheck-suppress unusedStructMember
    const HoroEditorActivityNode *nodes;
    // Read across the C/C++ package boundary by CopyActivityForm and EditorActivitySession::Publish.
    // cppcheck-suppress unusedStructMember
    uint32_t nodeCount;
};
typedef struct HoroEditorActivitySnapshot HoroEditorActivitySnapshot;

/** @brief Typed action identity and exact copied projection revision admitted by the host. */
struct HoroEditorActivityAction {
    // Invoke in EditorActivityModule.c validates this prefix before reading action members.
    // cppcheck-suppress unusedStructMember
    uint32_t structSize;
    HoroExtensionStringView nodeId;
    HoroExtensionStringView actionId;
    // Invoke in EditorActivityModule.c reads the admitted revision supplied by ExecuteAction.
    // cppcheck-suppress unusedStructMember
    uint64_t revision;
    HoroExtensionCancellation cancellation;
};
typedef struct HoroEditorActivityAction HoroEditorActivityAction;

/** @brief Bounded result sink borrowed by a scheduled module action; publication occurs later on the owner lane. */
struct HoroEditorActivitySnapshotSink {
    // Read across the C/C++ package boundary by Invoke in the external C package action callback.
    // cppcheck-suppress unusedStructMember
    void *context;
    // Read across the C/C++ package boundary by Invoke in the external C package; initialized by the host action pump.
    // cppcheck-suppress unusedStructMember
    HoroExtensionStatus (*publish)(void *context, const HoroEditorActivitySnapshot *snapshot);
};
typedef struct HoroEditorActivitySnapshotSink HoroEditorActivitySnapshotSink;
/** @brief Executes an approved semantic action outside GUI traversal; cancellation must be honored. */
typedef HoroExtensionStatus (*HoroEditorActivityActionFunc)(void *moduleContext, const HoroEditorActivityAction *action,
                                                            const HoroEditorActivitySnapshotSink *result);

/** @brief Copied package-localized message; locale/key/value strings are inert borrowed data. */
struct HoroEditorActivityMessage {
    HoroExtensionStringView locale;
    HoroExtensionStringView key;
    HoroExtensionStringView value;
};
typedef struct HoroEditorActivityMessage HoroEditorActivityMessage;

/** @brief Version-1 manifest-owned activity+panel binding; no host drawing callback or native object is exposed. */
struct HoroEditorActivityDescriptor {
    // RegisterEditorActivityImpl validates this prefix before reading the descriptor.
    // cppcheck-suppress unusedStructMember
    uint32_t structSize;
    // Read across the C/C++ package boundary by RegisterEditorActivityImpl in EditorActivitySession.cpp.
    // cppcheck-suppress unusedStructMember
    uint32_t schemaVersion;
    HoroExtensionStringView activityId;
    HoroExtensionStringView drawerId;
    HoroExtensionStringView labelKey;
    HoroExtensionStringView tooltipKey;
    HoroExtensionStringView iconResource;
    // Read across the C/C++ package boundary by RegisterEditorActivityImpl in EditorActivitySession.cpp.
    // cppcheck-suppress unusedStructMember
    uint32_t side;
    // Read across the C/C++ package boundary by RegisterEditorActivityImpl in EditorActivitySession.cpp.
    // cppcheck-suppress unusedStructMember
    uint32_t group;
    // Read across the C/C++ package boundary by RegisterEditorActivityImpl in EditorActivitySession.cpp.
    // cppcheck-suppress unusedStructMember
    int32_t order;
    // Read across the C/C++ package boundary by RegisterEditorActivityImpl in EditorActivitySession.cpp.
    // cppcheck-suppress unusedStructMember
    uint32_t openByDefault;
    HoroEditorActivitySnapshot initialSnapshot;
    // Read across the C/C++ package boundary by CopyMessages in EditorActivitySession.cpp.
    // cppcheck-suppress unusedStructMember
    const HoroEditorActivityMessage *messages;
    // Read across the C/C++ package boundary by RegisterEditorActivityImpl in EditorActivitySession.cpp.
    // cppcheck-suppress unusedStructMember
    uint32_t messageCount;
    // Read across the C/C++ package boundary by RegisterEditorActivityImpl and ExecuteAction in the host session/action sources.
    // cppcheck-suppress unusedStructMember
    void *moduleContext;
    // Read across the C/C++ package boundary by RegisterEditorActivityImpl in EditorActivitySession.cpp.
    // cppcheck-suppress unusedStructMember
    HoroEditorActivityActionFunc invokeAction;
};
typedef struct HoroEditorActivityDescriptor HoroEditorActivityDescriptor;

/**
 * @brief Owner-lane session API; the opaque context is valid until module unload, then must be discarded.
 * @details Reset/revocation makes publication fail closed. No worker may call publish; scheduled action results use their borrowed sink.
 */
struct HoroEditorActivitySessionApi {
    // Admission and the C package fixture validate this prefix before accessing the session table.
    // cppcheck-suppress unusedStructMember
    uint32_t structSize;
    // Read across the C/C++ package boundary by horo_extension_load ABI negotiation in EditorActivityModule.c.
    // cppcheck-suppress unusedStructMember
    uint32_t sessionVersion;
    // Read across the C/C++ package boundary by horo_extension_load ABI negotiation in EditorActivityModule.c.
    // cppcheck-suppress unusedStructMember
    uint32_t supportedNodeMask;
    // Read across the C/C++ package boundary by horo_extension_load and horo_test_activity_publish in EditorActivityModule.c.
    // cppcheck-suppress unusedStructMember
    void *context;
    // Read across the C/C++ package boundary by horo_extension_load and horo_test_activity_publish in EditorActivityModule.c.
    // cppcheck-suppress unusedStructMember
    HoroExtensionStatus (*publish)(void *context, const HoroEditorActivitySnapshot *snapshot);
};
typedef struct HoroEditorActivitySessionApi HoroEditorActivitySessionApi;
