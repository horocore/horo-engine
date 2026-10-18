#include "Horo/Extensions/EditorInspectorAbi.h"

#include <stddef.h>
#define TEXT(value) {value, sizeof(value) - 1}
static uint32_t queryMode;
static uint32_t observedPrefix;
static const HoroExtensionStringView permissions[] = {TEXT("editor.inspector")};
static const HoroEditorActivityMessage messages[] = {{TEXT("en-US"), TEXT("fixture.inspector"), TEXT("Fixture inspector")},
                                                     {TEXT("tr-TR"), TEXT("fixture.inspector"), TEXT("Deneme inceleyicisi")},
                                                     {TEXT("en-US"), TEXT("fixture.count"), TEXT("Exact count")}};
static const HoroEditorActivityNode nodes[] = {
    {.structSize = sizeof(HoroEditorActivityNode), .kind = HORO_EDITOR_ACTIVITY_STACK, .id = TEXT("fixture.root")}};
static const HoroEditorInspectorField fields[] = {{.structSize = sizeof(HoroEditorInspectorField),
                                                   .kind = HORO_EDITOR_INSPECTOR_INTEGER,
                                                   .flags = HORO_EDITOR_PAGE_INTEGER_MINIMUM | HORO_EDITOR_PAGE_INTEGER_MAXIMUM,
                                                   .fieldId = TEXT("count"),
                                                   .nodeId = TEXT("fixture.count"),
                                                   .parentNodeId = TEXT("fixture.root"),
                                                   .labelKey = TEXT("fixture.count"),
                                                   .minimumInteger = INT64_MIN,
                                                   .maximumInteger = INT64_MAX},
                                                  {.structSize = sizeof(HoroEditorInspectorField),
                                                   .kind = HORO_EDITOR_INSPECTOR_BOOLEAN,
                                                   .fieldId = TEXT("flag"),
                                                   .nodeId = TEXT("fixture.flag"),
                                                   .parentNodeId = TEXT("fixture.root"),
                                                   .labelKey = TEXT("fixture.count")},
                                                  {.structSize = sizeof(HoroEditorInspectorField),
                                                   .kind = HORO_EDITOR_INSPECTOR_NUMBER,
                                                   .flags = HORO_EDITOR_PAGE_INTEGER_MINIMUM | HORO_EDITOR_PAGE_INTEGER_MAXIMUM,
                                                   .fieldId = TEXT("number"),
                                                   .nodeId = TEXT("fixture.number"),
                                                   .parentNodeId = TEXT("fixture.root"),
                                                   .labelKey = TEXT("fixture.count"),
                                                   .minimumNumber = -1.5,
                                                   .maximumNumber = 2.5},
                                                  {.structSize = sizeof(HoroEditorInspectorField),
                                                   .kind = HORO_EDITOR_INSPECTOR_STRING,
                                                   .fieldId = TEXT("text"),
                                                   .nodeId = TEXT("fixture.text"),
                                                   .parentNodeId = TEXT("fixture.root"),
                                                   .labelKey = TEXT("fixture.count"),
                                                   .maximumStringBytes = 64}};

/** @brief Negotiates an explicit current or legacy prefix before any registration call. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    if (!requirements || requirements->structSize < sizeof(*requirements))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    *requirements = (HoroExtensionRequirements){.structSize = sizeof(*requirements),
                                                .abiMajorVersion = 1,
                                                .minimumHostMinor = (queryMode == 1 || queryMode == 2) ? 5 : 6,
                                                .requiredHostApiSize = (queryMode == 1 || queryMode == 2)
                                                                           ? offsetof(HoroExtensionHostApi, registerEditorInspector)
                                                                           : sizeof(HoroExtensionHostApi),
                                                .requiredFunctions = queryMode == 1 ? 0 : HORO_EXTENSION_REQUIRES_EDITOR_INSPECTOR};
    return HORO_EXTENSION_SUCCESS;
}

/** @brief Registers copied inert presentation only; the provider receives no document or edit callback. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (!host || host->structSize < offsetof(HoroExtensionHostApi, registerEditorInspector))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    observedPrefix = host->structSize;
    if (!HoroExtensionHasEditorInspectorTransport(host) || !module || module->structSize < sizeof(*module))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    const HoroEditorInspectorDescriptor descriptor = {.structSize = sizeof(descriptor),
                                                      .schemaVersion = HORO_EDITOR_INSPECTOR_SCHEMA_VERSION,
                                                      .kind = HORO_EDITOR_INSPECTOR,
                                                      .surfaceId = TEXT("fixture.inspector"),
                                                      .labelKey = TEXT("fixture.inspector"),
                                                      .behaviorType = TEXT("game.fixture.counter"),
                                                      .behaviorSchemaVersion = 3,
                                                      .fields = fields,
                                                      .fieldCount = queryMode == 3 ? 4 : 1,
                                                      .presentationNodes = nodes,
                                                      .presentationNodeCount = 1,
                                                      .messages = messages,
                                                      .messageCount = 3,
                                                      .permissions = permissions,
                                                      .permissionCount = 1};
    if (!HoroEditorInspectorDescriptorShape(&descriptor))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    const HoroExtensionStatus result = host->registerEditorInspector(host->hostContext, &descriptor);
    if (result != HORO_EXTENSION_SUCCESS)
        return result;
    *module = (HoroExtensionModuleApi){.structSize = sizeof(*module), .moduleId = TEXT("fixture.module"), .moduleVersion = TEXT("1.0.0")};
    return HORO_EXTENSION_SUCCESS;
}

/** @brief Discards fixture negotiation state only after its real host contribution has retired. */
HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    (void)module;
    queryMode = 0;
}

/** @brief Selects a legacy test negotiation before load, never while provider work is running. */
HORO_EXTENSION_EXPORT void horo_test_inspector_query_mode(uint32_t mode) {
    queryMode = mode;
}

/** @brief Exposes the actual prefix observed at native entry without reading any unavailable tail field. */
HORO_EXTENSION_EXPORT uint32_t horo_test_inspector_observed_prefix(void) {
    return observedPrefix;
}
