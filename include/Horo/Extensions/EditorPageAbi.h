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

/** @brief Checks a borrowed wire string's bound and pointer/count pairing without dereferencing foreign memory.
 * @param value Call-lifetime string view. @param maximum Maximum byte length.
 * @return Nonzero if its shape is admissible; UTF-8, NUL and semantic identity remain host copy-validation duties.
 */
static inline int HoroEditorPageTextShape(HoroExtensionStringView value, uint32_t maximum) {
    return value.length <= maximum && (value.length == 0 || value.data != 0);
}

/** @brief Checks the complete value prefix before reading its selected exact typed member.
 * @param value Borrowed full-prefix value. @return Nonzero for a known kind and bounded string/boolean representation.
 */
static inline int HoroEditorPageValueShape(const HoroEditorPageValue *value) {
    if (!value || value->structSize < sizeof(*value) || !HoroEditorPageTextShape(value->binding, 256))
        return 0;
    switch (value->kind) {
        case HORO_EDITOR_PAGE_BOOLEAN:
            return value->booleanValue <= 1;
        case HORO_EDITOR_PAGE_INTEGER:
            return 1;
        case HORO_EDITOR_PAGE_STRING:
            return HoroEditorPageTextShape(value->stringValue, 4096);
        default:
            return 0;
    }
}

/** @brief Validates field transport and integer constraints without allocating, activating or invoking a provider.
 * @param field Full-prefix candidate. @return Nonzero if shape/default constraints hold; host schema validation still owns semantics.
 */
static inline int HoroEditorPageFieldShape(const HoroEditorPageField *field) {
    if (!field || field->structSize < sizeof(*field) || (field->flags & ~15U) != 0 || field->maximumStringBytes > 4096 ||
        !HoroEditorPageTextShape(field->nodeId, 256) || !HoroEditorPageTextShape(field->labelKey, 128) ||
        !HoroEditorPageValueShape(&field->defaultValue))
        return 0;
    if (field->defaultValue.kind != HORO_EDITOR_PAGE_INTEGER)
        return (field->flags & (HORO_EDITOR_PAGE_INTEGER_MINIMUM | HORO_EDITOR_PAGE_INTEGER_MAXIMUM)) == 0 &&
               (field->defaultValue.kind != HORO_EDITOR_PAGE_STRING || field->defaultValue.stringValue.length <= field->maximumStringBytes);
    if ((field->flags & HORO_EDITOR_PAGE_INTEGER_MINIMUM) != 0 && field->defaultValue.integerValue < field->minimumInteger)
        return 0;
    if ((field->flags & HORO_EDITOR_PAGE_INTEGER_MAXIMUM) != 0 && field->defaultValue.integerValue > field->maximumInteger)
        return 0;
    return (field->flags & (HORO_EDITOR_PAGE_INTEGER_MINIMUM | HORO_EDITOR_PAGE_INTEGER_MAXIMUM)) !=
               (HORO_EDITOR_PAGE_INTEGER_MINIMUM | HORO_EDITOR_PAGE_INTEGER_MAXIMUM) ||
           field->minimumInteger <= field->maximumInteger;
}

/** @brief Checks every borrowed descriptor array's bounded extent before any host copy traversal.
 * @param page Complete ABI1.5 descriptor from admitted module memory.
 * @return Nonzero for structurally admissible transport; permission, localization, duplicate and FormKit validation remain mandatory.
 * @details As for all native ABI pointers, supplied nonnull memory must actually cover its declared extent for this call.
 */
static inline int HoroEditorPageDescriptorShape(const HoroEditorPageDescriptor *page) {
    if (!page || page->structSize < sizeof(*page) || page->schemaVersion != HORO_EDITOR_PAGE_SCHEMA_VERSION ||
        page->kind > HORO_EDITOR_PAGE_MODAL_CHILD || page->scope > HORO_EDITOR_PAGE_SESSION || !page->invoke ||
        !HoroEditorPageTextShape(page->surfaceId, 256) || !HoroEditorPageTextShape(page->labelKey, 128) ||
        !HoroEditorPageTextShape(page->parentSurfaceId, 256) || page->fieldCount > 64 || (page->fieldCount != 0 && !page->fields) ||
        page->permissionCount > 32 || (page->permissionCount != 0 && !page->permissions) || page->diagnosticCodeCount > 32 ||
        (page->diagnosticCodeCount != 0 && !page->diagnosticCodes) || page->presentationNodeCount > 256 ||
        (page->presentationNodeCount != 0 && !page->presentationNodes) || page->messageCount > 128 ||
        (page->messageCount != 0 && !page->messages))
        return 0;
    for (uint32_t index = 0; index < page->fieldCount; ++index)
        if (!HoroEditorPageFieldShape(&page->fields[index]))
            return 0;
    return 1;
}

/** @brief Checks callback request prefix, cancellation function and every exact typed value before provider traversal.
 * @param operation Call-lifetime request. @return Nonzero for a structurally valid generation-stamped operation.
 */
static inline int HoroEditorPageOperationShape(const HoroEditorPageOperation *operation) {
    if (!operation || operation->structSize < sizeof(*operation) || operation->operation > HORO_EDITOR_PAGE_CANCEL ||
        operation->revision == 0 || !HoroEditorPageTextShape(operation->surfaceId, 256) ||
        !HoroEditorPageTextShape(operation->actionId, 256) || operation->valueCount > 64 ||
        (operation->valueCount != 0 && !operation->values) || !operation->cancellation.isCancellationRequested)
        return 0;
    for (uint32_t index = 0; index < operation->valueCount; ++index)
        if (!HoroEditorPageValueShape(&operation->values[index]))
            return 0;
    return 1;
}

/** @brief Checks the full finding prefix before the host copies localized output.
 * @param finding Call-lifetime finding. @return Nonzero for a bounded known-severity finding; allowlists remain host-owned.
 */
static inline int HoroEditorPageFindingShape(const HoroEditorPageFinding *finding) {
    return finding && finding->structSize >= sizeof(*finding) && finding->severity <= 2 && HoroEditorPageTextShape(finding->binding, 256) &&
           HoroEditorPageTextShape(finding->diagnosticCode, 128) && HoroEditorPageTextShape(finding->messageKey, 128);
}

/** @brief Emits one structurally checked finding synchronously; neither endpoint may retain borrowed arguments.
 * @param sink Current host output sink. @param finding Provider result. @return Host result or invalid-argument rejection.
 */
static inline HoroExtensionStatus HoroEditorPageAppendFinding(const HoroEditorPageFindingSink *sink, const HoroEditorPageFinding *finding) {
    if (!sink || !sink->append || !HoroEditorPageFindingShape(finding))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    return sink->append(sink->context, finding);
}

/** @brief Dispatches one current-prefix operation to a previously admitted provider endpoint.
 * @param page Registered inert descriptor. @param operation Borrowed request. @param sink Borrowed output sink.
 * @return Provider status or invalid-argument rejection. Caller must hold its executable lease and contain C++ exceptions.
 */
static inline HoroExtensionStatus HoroEditorPageInvoke(const HoroEditorPageDescriptor *page, const HoroEditorPageOperation *operation,
                                                       const HoroEditorPageFindingSink *sink) {
    if (!page || page->structSize < sizeof(*page) || !page->invoke || !HoroEditorPageOperationShape(operation) || !sink || !sink->append)
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    return page->invoke(page->moduleContext, operation, sink);
}

/** @brief Invokes an admitted current-version page session on its owner lane; opaque contexts expire on provider unload.
 * @param session Returned host session. @param open Nonzero to open, zero to close.
 * @return Host status or version/argument rejection; no module or context ownership is acquired.
 */
static inline HoroExtensionStatus HoroEditorPageSetOpen(const HoroEditorPageSessionApi *session, int open) {
    if (!session || session->structSize < sizeof(*session) || session->sessionVersion != HORO_EDITOR_PAGE_SESSION_VERSION ||
        !session->context || !session->open || !session->close)
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    return open ? session->open(session->context) : session->close(session->context);
}
