#pragma once
/** @file EditorActivityAbi.h @brief Versioned C11 data/session transport for host-rendered activity destinations and drawers. */
#include "Horo/Extensions/ExtensionAbi.h"

/** @brief Canonical required host capability for the ABI 1.4 editor registration/session transport. */
#define HORO_EDITOR_ACTIVITY_HOST_CAPABILITY  "editor.activity"
#define HORO_EDITOR_COMMAND_HOST_CAPABILITY   "editor.commands"
#define HORO_EDITOR_CUSTOM_UI_HOST_CAPABILITY "editor.custom-composition"

enum {
    HORO_EDITOR_ACTIVITY_SCHEMA_VERSION = 1,
    HORO_EDITOR_ACTIVITY_PLOT_SCHEMA_VERSION = 2,
    HORO_EDITOR_ACTIVITY_MAXIMUM_PLOT_SAMPLES = 256,
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
    HORO_EDITOR_ACTIVITY_GROUP = 4,
    HORO_EDITOR_ACTIVITY_PLOT = 5,
    HORO_EDITOR_ACTIVITY_ROW = 6,
    HORO_EDITOR_ACTIVITY_GRID = 7
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
#ifndef __cplusplus
typedef struct HoroEditorActivityNode HoroEditorActivityNode;
#endif

/** @brief Normalized semantic data, never screen coordinates or draw commands. */
struct HoroEditorActivityPlotSample {
    float x;
    float y;
};
#ifndef __cplusplus
typedef struct HoroEditorActivityPlotSample HoroEditorActivityPlotSample;
#endif

enum HoroEditorUiFontRole {
    HORO_EDITOR_UI_FONT_NONE = 0,
    HORO_EDITOR_UI_FONT_SANS = 1,
    HORO_EDITOR_UI_FONT_SANS_COMPACT = 2,
    HORO_EDITOR_UI_FONT_SANS_EMPHASIS = 3,
    HORO_EDITOR_UI_FONT_MONOSPACE = 4,
    HORO_EDITOR_UI_FONT_ICON = 5,
    HORO_EDITOR_UI_FONT_COUNT = 6
};

enum HoroEditorUiIconRole {
    HORO_EDITOR_UI_ICON_NONE = 0,
    HORO_EDITOR_UI_ICON_GENERIC = 1,
    HORO_EDITOR_UI_ICON_INFO = 2,
    HORO_EDITOR_UI_ICON_WARNING = 3,
    HORO_EDITOR_UI_ICON_ERROR = 4,
    HORO_EDITOR_UI_ICON_CREATE = 5,
    HORO_EDITOR_UI_ICON_RENAME = 6,
    HORO_EDITOR_UI_ICON_DUPLICATE = 7,
    HORO_EDITOR_UI_ICON_DELETE = 8,
    HORO_EDITOR_UI_ICON_RESET = 9,
    HORO_EDITOR_UI_ICON_CHECK = 10,
    HORO_EDITOR_UI_ICON_SETTINGS = 11,
    HORO_EDITOR_UI_ICON_MORE = 12,
    HORO_EDITOR_UI_ICON_VISIBILITY = 13,
    HORO_EDITOR_UI_ICON_VISIBILITY_OFF = 14,
    HORO_EDITOR_UI_ICON_LOCK = 15,
    HORO_EDITOR_UI_ICON_SEARCH = 16,
    HORO_EDITOR_UI_ICON_BACK = 17,
    HORO_EDITOR_UI_ICON_FORWARD = 18,
    HORO_EDITOR_UI_ICON_FOLDER = 19,
    HORO_EDITOR_UI_ICON_FILE = 20,
    HORO_EDITOR_UI_ICON_PLAY = 21,
    HORO_EDITOR_UI_ICON_PAUSE = 22,
    HORO_EDITOR_UI_ICON_STOP = 23,
    HORO_EDITOR_UI_ICON_RECORD = 24,
    HORO_EDITOR_UI_ICON_COUNT = 25
};

enum HoroEditorUiColorRole {
    HORO_EDITOR_UI_COLOR_NONE = 0,
    HORO_EDITOR_UI_COLOR_SURFACE = 1,
    HORO_EDITOR_UI_COLOR_SURFACE_SUBTLE = 2,
    HORO_EDITOR_UI_COLOR_TEXT_PRIMARY = 3,
    HORO_EDITOR_UI_COLOR_TEXT_SECONDARY = 4,
    HORO_EDITOR_UI_COLOR_TEXT_DISABLED = 5,
    HORO_EDITOR_UI_COLOR_BORDER = 6,
    HORO_EDITOR_UI_COLOR_ACCENT = 7,
    HORO_EDITOR_UI_COLOR_FOCUS = 8,
    HORO_EDITOR_UI_COLOR_POSITIVE = 9,
    HORO_EDITOR_UI_COLOR_WARNING = 10,
    HORO_EDITOR_UI_COLOR_CRITICAL = 11,
    HORO_EDITOR_UI_COLOR_SURFACE_RAISED = 12,
    HORO_EDITOR_UI_COLOR_SURFACE_HOVER = 13,
    HORO_EDITOR_UI_COLOR_BORDER_STRONG = 14,
    HORO_EDITOR_UI_COLOR_ACCENT_HOVER = 15,
    HORO_EDITOR_UI_COLOR_ACCENT_ACTIVE = 16,
    HORO_EDITOR_UI_COLOR_ON_ACCENT = 17,
    HORO_EDITOR_UI_COLOR_OVERLAY = 18,
    HORO_EDITOR_UI_COLOR_COUNT = 19
};

enum HoroEditorUiMotionRole {
    HORO_EDITOR_UI_MOTION_NONE = 0,
    HORO_EDITOR_UI_MOTION_INSTANT = 1,
    HORO_EDITOR_UI_MOTION_FAST = 2,
    HORO_EDITOR_UI_MOTION_NORMAL = 3,
    HORO_EDITOR_UI_MOTION_SLOW = 4,
    HORO_EDITOR_UI_MOTION_HOVER = 5,
    HORO_EDITOR_UI_MOTION_PRESS = 6,
    HORO_EDITOR_UI_MOTION_FOCUS = 7,
    HORO_EDITOR_UI_MOTION_MODAL = 8,
    HORO_EDITOR_UI_MOTION_COUNT = 9
};

enum HoroEditorUiCustomWidget {
    HORO_EDITOR_UI_STANDARD = 0,
    HORO_EDITOR_UI_SCALAR_DIAL = 1
};

/** @brief Copied semantic choices, never font atlases, native icons or a draw callback. Clipping is always host-owned. */
struct HoroEditorUiCustomPresentation {
    uint32_t structSize;
    uint32_t schemaVersion;
    uint32_t widget;
    uint32_t fontRole;
    uint32_t iconRole;
    uint32_t accentRole;
    uint32_t motionRole;
    uint32_t controlRows;
};
#ifndef __cplusplus
typedef struct HoroEditorUiCustomPresentation HoroEditorUiCustomPresentation;
#endif

/** @brief Checks shared semantic presentation before any string copy or resource resolution.
 * @param style Borrowed full-prefix profile. @return Nonzero for the closed version1 catalog.
 */
static inline int HoroEditorUiCustomPresentationShape(const HoroEditorUiCustomPresentation *style) {
    return style && style->structSize >= sizeof(*style) && style->schemaVersion == 1 && style->widget <= HORO_EDITOR_UI_SCALAR_DIAL &&
           style->fontRole > HORO_EDITOR_UI_FONT_NONE && style->fontRole < HORO_EDITOR_UI_FONT_ICON &&
           style->iconRole < HORO_EDITOR_UI_ICON_COUNT && style->accentRole > HORO_EDITOR_UI_COLOR_NONE &&
           style->accentRole < HORO_EDITOR_UI_COLOR_COUNT && style->motionRole < HORO_EDITOR_UI_MOTION_COUNT && style->controlRows >= 1 &&
           style->controlRows <= 8;
}

/** @brief Schema2 fixed-stride node. Non-plot nodes require a null samples pointer and zero count.
 * @details A schema2 snapshot's nodes points to an array of these records (cast through the v1 pointer).
 * The unchanged snapshot/descriptor sizes preserve v1 binary layout. Copy completes before return.
 * Publishers negotiate support through the session's PLOT node-mask bit. No pointer is retained.
 */
struct HoroEditorActivityNodeV2 {
    HoroEditorActivityNode node;
    const HoroEditorActivityPlotSample *samples;
    uint32_t sampleCount;
    const HoroEditorUiCustomPresentation *presentation;
    HoroExtensionStringView descriptionKey;
    HoroExtensionStringView accessibleLabelKey;
    uint32_t columns;
};
#ifndef __cplusplus
typedef struct HoroEditorActivityNodeV2 HoroEditorActivityNodeV2;
#endif

/** @brief Validates the bounded normalized specialized payload before allocation or publication.
 * @param plot Borrowed schema2 record. @return Nonzero only for finite ordered samples in the plot budget.
 * @details Comparisons reject NaN and infinities without a platform math dependency. No pointer is retained.
 */
static inline int HoroEditorActivityPlotNodeShape(const HoroEditorActivityNodeV2 *plot) {
    uint32_t index;
    float previous = -1.0f;
    if (!plot || !plot->samples || plot->sampleCount < 2 || plot->sampleCount > HORO_EDITOR_ACTIVITY_MAXIMUM_PLOT_SAMPLES)
        return 0;
    for (index = 0; index < plot->sampleCount; ++index) {
        const float x = plot->samples[index].x;
        const float y = plot->samples[index].y;
        if (!(x >= 0.0f && x <= 1.0f && x > previous && y >= 0.0f && y <= 1.0f))
            return 0;
        previous = x;
    }
    return 1;
}

/** @brief Checks all schema2 extents before host copying; it is not a native-memory sandbox.
 * @param node Borrowed complete fixed-stride record. @return Nonzero for the closed structural profile.
 */
static inline int HoroEditorActivityNodeV2Shape(const HoroEditorActivityNodeV2 *node) {
    if (!node || node->node.structSize < sizeof(node->node) || node->node.kind > HORO_EDITOR_ACTIVITY_GRID ||
        node->descriptionKey.length > 128 || (node->descriptionKey.length && !node->descriptionKey.data) ||
        node->accessibleLabelKey.length > 128 || (node->accessibleLabelKey.length && !node->accessibleLabelKey.data) ||
        (node->presentation && !HoroEditorUiCustomPresentationShape(node->presentation)) || node->columns > 16)
        return 0;
    if (node->node.kind == HORO_EDITOR_ACTIVITY_GRID) {
        if (node->columns == 0)
            return 0;
    } else if (node->columns > 1)
        return 0;
    if (node->node.kind == HORO_EDITOR_ACTIVITY_PLOT)
        return HoroEditorActivityPlotNodeShape(node);
    return !node->samples && node->sampleCount == 0;
}

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
#ifndef __cplusplus
typedef struct HoroEditorActivitySnapshot HoroEditorActivitySnapshot;
#endif

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
#ifndef __cplusplus
typedef struct HoroEditorActivityAction HoroEditorActivityAction;
#endif

/** @brief Bounded result sink borrowed by a scheduled module action; publication occurs later on the owner lane. */
struct HoroEditorActivitySnapshotSink {
    // Read across the C/C++ package boundary by Invoke in the external C package action callback.
    // cppcheck-suppress unusedStructMember
    void *context;
    // Read across the C/C++ package boundary by Invoke in the external C package; initialized by the host action pump.
    // cppcheck-suppress unusedStructMember
    HoroExtensionStatus (*publish)(void *context, const HoroEditorActivitySnapshot *snapshot);
};
#ifndef __cplusplus
typedef struct HoroEditorActivitySnapshotSink HoroEditorActivitySnapshotSink;
#endif
/** @brief Executes an approved semantic action outside GUI traversal; cancellation must be honored. */
#ifdef __cplusplus
using HoroEditorActivityActionFunc = HoroExtensionStatus (*)(void *moduleContext, const HoroEditorActivityAction *action,
                                                             const HoroEditorActivitySnapshotSink *result);
#else
typedef HoroExtensionStatus (*HoroEditorActivityActionFunc)(void *moduleContext, const HoroEditorActivityAction *action,
                                                            const HoroEditorActivitySnapshotSink *result);
#endif

/** @brief Copied package-localized message; locale/key/value strings are inert borrowed data. */
struct HoroEditorActivityMessage {
    HoroExtensionStringView locale;
    HoroExtensionStringView key;
    HoroExtensionStringView value;
};
#ifndef __cplusplus
typedef struct HoroEditorActivityMessage HoroEditorActivityMessage;
#endif

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
#ifndef __cplusplus
typedef struct HoroEditorActivityDescriptor HoroEditorActivityDescriptor;
#endif

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
#ifndef __cplusplus
typedef struct HoroEditorActivitySessionApi HoroEditorActivitySessionApi;
#endif

/** @brief Opt-in schema2 command routed to an existing semantic action; all strings are copied at registration.
 * @details IDs must also be manifest-declared editor.command contributions owned by this module.
 * Shortcuts use the command registry grammar, for example Ctrl+Shift+P or Meta+Shift+P.
 */
struct HoroEditorActivityCommand {
    HoroExtensionStringView id;
    HoroExtensionStringView nodeId;
    HoroExtensionStringView actionId;
    HoroExtensionStringView labelKey;
    HoroExtensionStringView shortcut;
};
#ifndef __cplusplus
typedef struct HoroEditorActivityCommand HoroEditorActivityCommand;
#endif

/** @brief Append-only activity descriptor profile with explicit command publication.
 * @details Set activity.structSize to sizeof this record and activity.schemaVersion to 2.
 * Require editor.commands in the module manifest. Schema1 and its binary layout remain supported.
 * Commands execute through the existing scheduled action callback, never during GUI drawing.
 */
struct HoroEditorActivityDescriptorV2 {
    HoroEditorActivityDescriptor activity;
    const HoroEditorActivityCommand *commands;
    uint32_t commandCount;
};
#ifndef __cplusplus
typedef struct HoroEditorActivityDescriptorV2 HoroEditorActivityDescriptorV2;
#endif
