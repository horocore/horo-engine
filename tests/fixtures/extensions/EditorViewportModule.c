#include "Horo/Extensions/EditorViewportAbi.h"

#include <stddef.h>
#define TEXT(value) {value, sizeof(value) - 1}
static uint32_t mode;
static const HoroExtensionStringView permissions[] = {TEXT("editor.gizmo")};

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    if (!requirements || requirements->structSize < sizeof(*requirements))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    *requirements = (HoroExtensionRequirements){.structSize = sizeof(*requirements),
                                                .abiMajorVersion = 1,
                                                .minimumHostMinor = mode == 1 ? 6 : 7,
                                                .requiredHostApiSize = mode == 1 ? offsetof(HoroExtensionHostApi, registerEditorViewport)
                                                                                 : sizeof(HoroExtensionHostApi),
                                                .requiredFunctions = HORO_EXTENSION_REQUIRES_EDITOR_VIEWPORT};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (!HoroExtensionHasEditorViewportTransport(host) || !module || module->structSize < sizeof(*module))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    HoroEditorViewportLine line =
        {.structSize = sizeof(line), .x0 = 0.1f, .y0 = 0.7f, .x1 = 0.3f, .y1 = 0.7f, .thickness = 2, .tone = 2, .axis = 0};
    if (mode == 3)
        line.x1 = 2;
    const HoroEditorViewportDescriptor descriptor = {.structSize = sizeof(descriptor),
                                                     .schemaVersion = 1,
                                                     .kind = 1,
                                                     .surfaceId = TEXT("fixture.gizmo"),
                                                     .labelKey = TEXT("fixture.gizmo"),
                                                     .editableType = TEXT("horo.scene.transform"),
                                                     .tool = 1,
                                                     .visible = 1,
                                                     .lines = &line,
                                                     .lineCount = 1,
                                                     .permissions = permissions,
                                                     .permissionCount = 1};
    const HoroExtensionStatus result = host->registerEditorViewport(host->hostContext, &descriptor);
    if (result != HORO_EXTENSION_SUCCESS)
        return result;
    line.x1 = 0; /* Host must retain the copy, never stack/module storage. */
    if (mode == 2)
        return HORO_EXTENSION_ERROR_INIT_FAILED;
    *module = (HoroExtensionModuleApi){.structSize = sizeof(*module), .moduleId = TEXT("fixture.module"), .moduleVersion = TEXT("1.0.0")};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    (void)module;
}

HORO_EXTENSION_EXPORT void horo_test_viewport_mode(uint32_t value) {
    mode = value;
}
