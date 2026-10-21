#pragma once
/** @file EditorInspectorAbi.h @brief ABI1.6 inert typed behavior inspector and property-drawer declarations. */
#include "Horo/Extensions/EditorPageAbi.h"

#include <float.h>
#define HORO_EDITOR_INSPECTOR_HOST_CAPABILITY "editor.inspector"

enum {
    HORO_EDITOR_INSPECTOR_SCHEMA_VERSION = 1,
    HORO_EDITOR_INSPECTOR_COMPOSITION_SCHEMA_VERSION = 2
};

enum HoroEditorInspectorKind {
    HORO_EDITOR_INSPECTOR = 0,
    HORO_EDITOR_PROPERTY_DRAWER = 1
};

enum HoroEditorInspectorFieldKind {
    HORO_EDITOR_INSPECTOR_BOOLEAN = 0,
    HORO_EDITOR_INSPECTOR_INTEGER = 1,
    HORO_EDITOR_INSPECTOR_NUMBER = 2,
    HORO_EDITOR_INSPECTOR_STRING = 3
};

/** @brief Copied field presentation; no default, authored payload, setter, document pointer or native handle is supplied. */
struct HoroEditorInspectorField {
    uint32_t structSize;
    uint32_t kind;
    uint32_t flags; /**< Page READ_ONLY/DISABLED and minimum/maximum bits apply to the selected numeric kind. */
    HoroExtensionStringView fieldId;
    HoroExtensionStringView nodeId;
    HoroExtensionStringView parentNodeId;
    HoroExtensionStringView labelKey;
    int64_t minimumInteger;
    int64_t maximumInteger;
    double minimumNumber;
    double maximumNumber;
    uint32_t maximumStringBytes;
};
#ifndef __cplusplus
typedef struct HoroEditorInspectorField HoroEditorInspectorField;
#endif
/** @brief Complete inert manifest-owned declaration, copied during native load and published only after whole-module admission.
 * @details An Inspector declares a behavior schema; a PropertyDrawer also declares exactly one propertyField. Current editable
 * profile is bool/int64/double/string. Vector/quaternion and opaque component data keep generic host presentation. Strings borrow
 * only for registration. Providers never receive selection objects, JSON envelopes, editing authority or UI resources.
 */
struct HoroEditorInspectorDescriptor {
    uint32_t structSize;
    uint32_t schemaVersion;
    uint32_t kind;
    HoroExtensionStringView surfaceId;
    HoroExtensionStringView labelKey;
    HoroExtensionStringView behaviorType;
    uint32_t behaviorSchemaVersion;
    HoroExtensionStringView propertyField;
    const HoroEditorInspectorField *fields;
    uint32_t fieldCount;
    const HoroEditorActivityNode *presentationNodes;
    uint32_t presentationNodeCount;
    const HoroEditorActivityMessage *messages;
    uint32_t messageCount;
    const HoroExtensionStringView *permissions;
    uint32_t permissionCount;
};
#ifndef __cplusplus
typedef struct HoroEditorInspectorDescriptor HoroEditorInspectorDescriptor;
#endif
/** @brief Versioned custom presentation for one existing field; the binding and value authority remain unchanged. */
struct HoroEditorInspectorPresentation {
    HoroExtensionStringView nodeId;
    HoroEditorUiCustomPresentation presentation;
};
#ifndef __cplusplus
typedef struct HoroEditorInspectorPresentation HoroEditorInspectorPresentation;
#endif
/** @brief Schema two prefix extension; presentationNodes use the schema two activity-node stride. */
struct HoroEditorInspectorDescriptorV2 {
    HoroEditorInspectorDescriptor base;
    const HoroEditorInspectorPresentation *presentations;
    uint32_t presentationCount;
};
#ifndef __cplusplus
typedef struct HoroEditorInspectorDescriptorV2 HoroEditorInspectorDescriptorV2;
#endif
/** @brief Checks a borrowed inspector string bound before its deep host copy.
 * @param text Call-lifetime view. @param maximum Maximum admitted bytes. @return Nonzero for bounded paired storage.
 */
static inline int HoroEditorInspectorTextShape(HoroExtensionStringView text, uint32_t maximum) {
    return text.length <= maximum && (text.length == 0 || text.data != 0);
}

/** @brief Validates the full field prefix and selected exact constraints before native producer or host traversal.
 * @param field Call-lifetime full-prefix field. @return Nonzero for an admissible shape; host semantic validation remains mandatory.
 */
static inline int HoroEditorInspectorFieldShape(const HoroEditorInspectorField *field) {
    if (!field || field->structSize < sizeof(*field) || field->kind > HORO_EDITOR_INSPECTOR_STRING || (field->flags & ~15U) != 0 ||
        field->maximumStringBytes > 4096 || !HoroEditorInspectorTextShape(field->fieldId, 96) ||
        !HoroEditorInspectorTextShape(field->nodeId, 256) || !HoroEditorInspectorTextShape(field->parentNodeId, 256) ||
        !HoroEditorInspectorTextShape(field->labelKey, 128))
        return 0;
    if (field->kind != HORO_EDITOR_INSPECTOR_INTEGER && field->kind != HORO_EDITOR_INSPECTOR_NUMBER)
        return (field->flags & (HORO_EDITOR_PAGE_INTEGER_MINIMUM | HORO_EDITOR_PAGE_INTEGER_MAXIMUM)) == 0;
    if (field->kind == HORO_EDITOR_INSPECTOR_INTEGER)
        return (field->flags & 12U) != 12U || field->minimumInteger <= field->maximumInteger;
    if ((field->flags & HORO_EDITOR_PAGE_INTEGER_MINIMUM) != 0 && !(field->minimumNumber >= -DBL_MAX && field->minimumNumber <= DBL_MAX))
        return 0;
    if ((field->flags & HORO_EDITOR_PAGE_INTEGER_MAXIMUM) != 0 && !(field->maximumNumber >= -DBL_MAX && field->maximumNumber <= DBL_MAX))
        return 0;
    return (field->flags & 12U) != 12U || field->minimumNumber <= field->maximumNumber;
}

/** @brief Checks descriptor prefixes and bounded paired extents before any deep native copy.
 * @param descriptor Call-lifetime module-owned descriptor. @return Nonzero for structural admission; no registration is performed.
 * @details Nonnull supplied native memory must cover its declared extent. These checks are not a memory sandbox or permission grant.
 */
static inline int HoroEditorInspectorDescriptorShape(const HoroEditorInspectorDescriptor *descriptor) {
    if (!descriptor || descriptor->structSize < sizeof(*descriptor) ||
        (descriptor->schemaVersion != HORO_EDITOR_INSPECTOR_SCHEMA_VERSION &&
         descriptor->schemaVersion != HORO_EDITOR_INSPECTOR_COMPOSITION_SCHEMA_VERSION) ||
        descriptor->kind > HORO_EDITOR_PROPERTY_DRAWER || descriptor->behaviorSchemaVersion == 0 ||
        !HoroEditorInspectorTextShape(descriptor->surfaceId, 256) || !HoroEditorInspectorTextShape(descriptor->labelKey, 128) ||
        !HoroEditorInspectorTextShape(descriptor->behaviorType, 160) || !HoroEditorInspectorTextShape(descriptor->propertyField, 96) ||
        descriptor->fieldCount == 0 || descriptor->fieldCount > 64 || !descriptor->fields || descriptor->presentationNodeCount > 256 ||
        (descriptor->presentationNodeCount != 0 && !descriptor->presentationNodes) || descriptor->messageCount > 128 ||
        (descriptor->messageCount != 0 && !descriptor->messages) || descriptor->permissionCount > 32 ||
        (descriptor->permissionCount != 0 && !descriptor->permissions))
        return 0;
    if (descriptor->schemaVersion == HORO_EDITOR_INSPECTOR_COMPOSITION_SCHEMA_VERSION) {
        if (descriptor->structSize < sizeof(HoroEditorInspectorDescriptorV2))
            return 0;
        const HoroEditorInspectorDescriptorV2 *extended = (const HoroEditorInspectorDescriptorV2 *)descriptor;
        if (extended->presentationCount > 64 || (extended->presentationCount && !extended->presentations))
            return 0;
        for (uint32_t index = 0; index < extended->presentationCount; ++index)
            if (!HoroEditorInspectorTextShape(extended->presentations[index].nodeId, 256) ||
                !HoroEditorUiCustomPresentationShape(&extended->presentations[index].presentation))
                return 0;
    }
    for (uint32_t index = 0; index < descriptor->fieldCount; ++index)
        if (!HoroEditorInspectorFieldShape(&descriptor->fields[index]))
            return 0;
    return 1;
}
