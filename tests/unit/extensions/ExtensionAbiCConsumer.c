#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/EditorInspectorAbi.h"
#include "Horo/Extensions/EditorPageAbi.h"
#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(HoroExtensionHostApi, structSize) == 0, "Host table starts with its size");
_Static_assert(sizeof(HoroExtensionStatus) == sizeof(uint32_t), "Status values have fixed width");
_Static_assert(sizeof(HoroAssetImportSettingKind) == sizeof(uint32_t), "Setting tags have fixed width");
_Static_assert(offsetof(HoroExtensionModuleApi, structSize) == 0, "Module table starts with its size");
_Static_assert(sizeof(((HoroExtensionHostApi *)0)->abiVersion) == sizeof(uint32_t), "ABI version is fixed-width");

_Static_assert(offsetof(HoroExtensionHostApi, registerEditorActivity) > offsetof(HoroExtensionHostApi, registerPlatformServicesProvider),
               "Activity ABI appends after the old host prefix");
_Static_assert(sizeof(((HoroEditorActivitySnapshot *)0)->revision) == sizeof(uint64_t), "Projection revision has fixed width");
_Static_assert(offsetof(HoroExtensionHostApi, registerEditorActivity) ==
                   offsetof(HoroExtensionHostApi, registerPlatformServicesProvider) + sizeof(HoroRegisterPlatformServicesProviderFunc),
               "Activity callback does not change the legacy host prefix");
_Static_assert(_Generic((HoroRegisterEditorActivityFunc)0,
                   HoroExtensionStatus (*)(void *, const struct HoroEditorActivityDescriptor *, struct HoroEditorActivitySessionApi *): 1,
                   default: 0),
               "C11 registration callback source contract is unchanged");
_Static_assert(_Generic((HoroEditorActivityActionFunc)0,
                   HoroExtensionStatus (*)(void *, const HoroEditorActivityAction *, const HoroEditorActivitySnapshotSink *): 1,
                   default: 0),
               "C11 action callback source contract is unchanged");
_Static_assert(offsetof(HoroEditorActivityDescriptor, structSize) == 0, "Activity descriptor starts with its size");
_Static_assert(offsetof(HoroEditorActivitySessionApi, structSize) == 0, "Activity session starts with its size");

/** @brief Exercises exact C11 page defaults and fail-closed prefix/extent admission before native traversal. */
static int CheckPageWire(void) {
    HoroEditorPageField field = {.structSize = sizeof(HoroEditorPageField),
                                 .flags = HORO_EDITOR_PAGE_INTEGER_MINIMUM | HORO_EDITOR_PAGE_INTEGER_MAXIMUM,
                                 .defaultValue = {.structSize = sizeof(HoroEditorPageValue),
                                                  .kind = HORO_EDITOR_PAGE_INTEGER,
                                                  .integerValue = INT64_MAX},
                                 .minimumInteger = INT64_MIN,
                                 .maximumInteger = INT64_MAX};
    HoroEditorPageDescriptor page = {.structSize = 0, .fieldCount = UINT32_MAX};
    HoroEditorPageSessionApi session = {.structSize = 0, .sessionVersion = UINT32_MAX};
    if (!HoroEditorPageFieldShape(&field) || HoroEditorPageDescriptorShape(&page) ||
        HoroEditorPageSetOpen(&session, 1) == HORO_EXTENSION_SUCCESS)
        return 0;
    field.maximumInteger = INT64_MAX - 1;
    if (HoroEditorPageFieldShape(&field))
        return 0;
    field.defaultValue.structSize = 0;
    return !HoroEditorPageFieldShape(&field) && !HoroEditorPageOperationShape(0) && !HoroEditorPageFindingShape(0);
}

_Static_assert(offsetof(HoroExtensionHostApi, registerEditorInspector) ==
                   offsetof(HoroExtensionHostApi, registerEditorPage) + sizeof(HoroRegisterEditorPageFunc),
               "Inspector preserves the exact1.5 prefix");
_Static_assert(_Generic((HoroRegisterEditorInspectorFunc)0,
                   HoroExtensionStatus (*)(void *, const struct HoroEditorInspectorDescriptor *): 1,
                   default: 0),
               "Inspector declaration registration has a real C11 signature");

/** @brief Contract callback returns a typed refusal; transport availability never invokes registration. */
static HoroExtensionStatus RejectInspector(void *context, const struct HoroEditorInspectorDescriptor *descriptor) {
    (void)context;
    (void)descriptor;
    return HORO_EXTENSION_ERROR_INVALID_ARGS;
}

/** @brief Checks real host admission guards including old1.5 prefix, wrong major and absent callback. */
static int InspectorTransportContract(void) {
    HoroExtensionHostApi host = {.structSize = sizeof(host),
                                 .abiVersion = 1,
                                 .abiMinorVersion = 6,
                                 .registerEditorInspector = RejectInspector};
    if (!HoroExtensionHasEditorInspectorTransport(&host))
        return 0;
    host.structSize = offsetof(HoroExtensionHostApi, registerEditorInspector);
    if (HoroExtensionHasEditorInspectorTransport(&host))
        return 0;
    host.structSize = sizeof(host);
    host.abiVersion = 2;
    if (HoroExtensionHasEditorInspectorTransport(&host))
        return 0;
    host.abiVersion = 1;
    host.abiMinorVersion = 5;
    if (HoroExtensionHasEditorInspectorTransport(&host))
        return 0;
    host.abiMinorVersion = 6;
    host.registerEditorInspector = 0;
    return !HoroExtensionHasEditorInspectorTransport(&host);
}

/** @brief Exercises exact full-width constraints and prefix-first malformed shape refusal in the real C11 consumer. */
static int InspectorShapeContract(void) {
    HoroEditorInspectorField field = {.structSize = sizeof(field),
                                      .kind = HORO_EDITOR_INSPECTOR_INTEGER,
                                      .flags = HORO_EDITOR_PAGE_INTEGER_MINIMUM | HORO_EDITOR_PAGE_INTEGER_MAXIMUM,
                                      .fieldId = {"count", 5},
                                      .nodeId = {"count", 5},
                                      .labelKey = {"count", 5},
                                      .minimumInteger = INT64_MIN,
                                      .maximumInteger = INT64_MAX};
    HoroEditorInspectorDescriptor descriptor = {.structSize = sizeof(descriptor),
                                                .schemaVersion = 1,
                                                .kind = HORO_EDITOR_INSPECTOR,
                                                .surfaceId = {"fixture", 7},
                                                .labelKey = {"fixture", 7},
                                                .behaviorType = {"project.counter", 15},
                                                .behaviorSchemaVersion = 3,
                                                .fields = &field,
                                                .fieldCount = 1};
    if (!HoroEditorInspectorFieldShape(&field) || !HoroEditorInspectorDescriptorShape(&descriptor))
        return 0;
    field.minimumInteger = INT64_MAX;
    field.maximumInteger = INT64_MIN;
    if (HoroEditorInspectorFieldShape(&field) || HoroEditorInspectorDescriptorShape(&descriptor))
        return 0;
    field.minimumInteger = INT64_MIN;
    field.maximumInteger = INT64_MAX;
    field.structSize = offsetof(HoroEditorInspectorField, fieldId);
    if (HoroEditorInspectorFieldShape(&field) || HoroEditorInspectorDescriptorShape(&descriptor))
        return 0;
    field.structSize = sizeof(field);
    if (!HoroEditorInspectorFieldShape(&field))
        return 0;
    descriptor.fieldCount = 65;
    if (HoroEditorInspectorDescriptorShape(&descriptor))
        return 0;
    descriptor.structSize = offsetof(HoroEditorInspectorDescriptor, schemaVersion);
    return !HoroEditorInspectorDescriptorShape(&descriptor);
}

/** @brief Compile the public ABI as C, independently of C++ language extensions. */
int main(void) {
    const HoroExtensionHostApi host = {.structSize = sizeof(HoroExtensionHostApi), .abiVersion = HORO_EXTENSION_ABI_VERSION};
    const HoroExtensionModuleApi module = {.structSize = sizeof(HoroExtensionModuleApi)};
    return CheckPageWire() && InspectorTransportContract() && InspectorShapeContract() && host.structSize == sizeof(host) &&
                   module.structSize == sizeof(module) && strcmp(HORO_EDITOR_ACTIVITY_HOST_CAPABILITY, "editor.activity") == 0
               ? 0
               : 1;
}
