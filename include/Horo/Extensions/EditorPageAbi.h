#pragma once
/** @file EditorPageAbi.h @brief ABI 1.5 restricted settings/modal transport with exact signed integer values. */
#include "Horo/Extensions/EditorActivityAbi.h"

#define HORO_EDITOR_PAGE_HOST_CAPABILITY "editor.page"

enum {
    HORO_EDITOR_PAGE_SCHEMA_VERSION = 1,
    HORO_EDITOR_PAGE_SESSION_VERSION = 1
};

enum HoroEditorPageKind {
    HORO_EDITOR_PAGE_SETTINGS = 0,
    HORO_EDITOR_PAGE_MODAL = 1,
    HORO_EDITOR_PAGE_MODAL_CHILD = 2
};

enum HoroEditorPageScope {
    HORO_EDITOR_PAGE_USER = 0,
    HORO_EDITOR_PAGE_PROJECT = 1,
    HORO_EDITOR_PAGE_WORKSPACE = 2,
    HORO_EDITOR_PAGE_SESSION = 3
};

enum HoroEditorPageValueKind {
    HORO_EDITOR_PAGE_BOOLEAN = 0,
    HORO_EDITOR_PAGE_INTEGER = 1,
    HORO_EDITOR_PAGE_STRING = 2
};

enum HoroEditorPageOperationCode {
    HORO_EDITOR_PAGE_VALIDATE = 0,
    HORO_EDITOR_PAGE_SUBMIT = 1,
    HORO_EDITOR_PAGE_CANCEL = 2
};

enum HoroEditorPageFieldFlags {
    HORO_EDITOR_PAGE_FIELD_READ_ONLY = 1U,
    HORO_EDITOR_PAGE_FIELD_DISABLED = 2U,
    HORO_EDITOR_PAGE_INTEGER_MINIMUM = 4U,
    HORO_EDITOR_PAGE_INTEGER_MAXIMUM = 8U
};

/** @brief Exact wire value; only the member selected by kind is read. String memory is borrowed during the call. */
struct HoroEditorPageValue {
    uint32_t structSize;
    uint32_t kind;
    HoroExtensionStringView binding;
    uint32_t booleanValue;
    int64_t integerValue;
    HoroExtensionStringView stringValue;
};
#ifndef __cplusplus
typedef struct HoroEditorPageValue HoroEditorPageValue;
#endif
/** @brief Canonical setting plus its standard FormKit primitive; native GUI/toolkit handles are never transported. */
struct HoroEditorPageField {
    uint32_t structSize;
    uint32_t flags;
    HoroExtensionStringView nodeId;
    HoroExtensionStringView labelKey;
    HoroEditorPageValue defaultValue;
    int64_t minimumInteger;
    int64_t maximumInteger;
    uint32_t maximumStringBytes;
};
#ifndef __cplusplus
typedef struct HoroEditorPageField HoroEditorPageField;
#endif
/** @brief Localization-only validation finding; the host validates every code, binding and key before publication. */
struct HoroEditorPageFinding {
    uint32_t structSize;
    uint32_t severity;
    HoroExtensionStringView binding;
    HoroExtensionStringView diagnosticCode;
    HoroExtensionStringView messageKey;
};
#ifndef __cplusplus
typedef struct HoroEditorPageFinding HoroEditorPageFinding;
#endif
/** @brief Synchronous bounded sink; copied findings cannot retain provider memory after the operation returns. */
struct HoroEditorPageFindingSink {
    void *context;
    HoroExtensionStatus (*append)(void *context, const HoroEditorPageFinding *finding);
};
#ifndef __cplusplus
typedef struct HoroEditorPageFindingSink HoroEditorPageFindingSink;
#endif
/** @brief One scheduled operation; immutable value/cancellation views expire when the callback returns. */
struct HoroEditorPageOperation {
    uint32_t structSize;
    uint32_t operation;
    uint64_t revision;
    HoroExtensionStringView surfaceId;
    HoroExtensionStringView actionId;
    const HoroEditorPageValue *values;
    uint32_t valueCount;
    HoroExtensionCancellation cancellation;
};
#ifndef __cplusplus
typedef struct HoroEditorPageOperation HoroEditorPageOperation;
#endif
#ifdef __cplusplus
using HoroEditorPageOperationFunc = HoroExtensionStatus (*)(void *moduleContext, const HoroEditorPageOperation *operation,
                                                            const HoroEditorPageFindingSink *result);
#else
typedef HoroExtensionStatus (*HoroEditorPageOperationFunc)(void *moduleContext, const HoroEditorPageOperation *operation,
                                                           const HoroEditorPageFindingSink *result);
#endif
/** @brief Complete inert manifest-owned page copied during native module load; scope applies uniformly to every field. */
struct HoroEditorPageDescriptor {
    uint32_t structSize;
    uint32_t schemaVersion;
    uint32_t kind;
    uint32_t scope;
    HoroExtensionStringView surfaceId;
    HoroExtensionStringView labelKey;
    HoroExtensionStringView parentSurfaceId; /**< Required only for a child modal; exact same-provider parent identity. */
    const HoroExtensionStringView *permissions;
    uint32_t permissionCount;
    const HoroExtensionStringView *diagnosticCodes;
    uint32_t diagnosticCodeCount;
    const HoroEditorPageField *fields;
    uint32_t fieldCount;
    const HoroEditorActivityNode *presentationNodes;
    uint32_t presentationNodeCount;
    const HoroEditorActivityMessage *messages;
    uint32_t messageCount;
    void *moduleContext;
    HoroEditorPageOperationFunc invoke;
};
#ifndef __cplusplus
typedef struct HoroEditorPageDescriptor HoroEditorPageDescriptor;
#endif
/** @brief Owner-lane generation-fenced workflow admission, populated after staging; no configuration or UI handle is exposed.
 * @details open is rejected until the whole native load transaction commits. The opaque context expires on unload;
 * providers must discard it then. Close cancels the draft and requests cooperative cancellation of scheduled work.
 */
struct HoroEditorPageSessionApi {
    uint32_t structSize;
    uint32_t sessionVersion;
    void *context;
    HoroExtensionStatus (*open)(void *context);
    HoroExtensionStatus (*close)(void *context);
};
#ifndef __cplusplus
typedef struct HoroEditorPageSessionApi HoroEditorPageSessionApi;
#endif
