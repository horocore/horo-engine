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
    uint32_t structSize;
    uint32_t kind;
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
    uint32_t structSize;
    uint32_t schemaVersion;
    uint64_t revision;
    uint32_t presentationFlags;
    uint32_t badgeCount;
    const HoroEditorActivityNode *nodes;
    uint32_t nodeCount;
};
typedef struct HoroEditorActivitySnapshot HoroEditorActivitySnapshot;

/** @brief Typed action identity and exact copied projection revision admitted by the host. */
struct HoroEditorActivityAction {
    uint32_t structSize;
    HoroExtensionStringView nodeId;
    HoroExtensionStringView actionId;
    uint64_t revision;
    HoroExtensionCancellation cancellation;
};
typedef struct HoroEditorActivityAction HoroEditorActivityAction;

/** @brief Bounded result sink borrowed by a scheduled module action; publication occurs later on the owner lane. */
struct HoroEditorActivitySnapshotSink {
    void *context;
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
    uint32_t structSize;
    uint32_t schemaVersion;
    HoroExtensionStringView activityId;
    HoroExtensionStringView drawerId;
    HoroExtensionStringView labelKey;
    HoroExtensionStringView tooltipKey;
    HoroExtensionStringView iconResource;
    uint32_t side;
    uint32_t group;
    int32_t order;
    uint32_t openByDefault;
    HoroEditorActivitySnapshot initialSnapshot;
    const HoroEditorActivityMessage *messages;
    uint32_t messageCount;
    void *moduleContext;
    HoroEditorActivityActionFunc invokeAction;
};
typedef struct HoroEditorActivityDescriptor HoroEditorActivityDescriptor;

/**
 * @brief Owner-lane session API; the opaque context is valid until module unload, then must be discarded.
 * @details Reset/revocation makes publication fail closed. No worker may call publish; scheduled action results use their borrowed sink.
 */
struct HoroEditorActivitySessionApi {
    uint32_t structSize;
    uint32_t sessionVersion;
    uint32_t supportedNodeMask;
    void *context;
    HoroExtensionStatus (*publish)(void *context, const HoroEditorActivitySnapshot *snapshot);
};
typedef struct HoroEditorActivitySessionApi HoroEditorActivitySessionApi;
