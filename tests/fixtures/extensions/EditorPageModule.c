#include "Horo/Extensions/EditorPageAbi.h"

#include <stdatomic.h>
#include <stddef.h>
#define TEXT(value) {value, sizeof(value) - 1}
static HoroEditorPageSessionApi sessions[3];
static atomic_int_least64_t lastInteger;
static atomic_uint calls[3];
static uint32_t legacyQuery;

/** @brief Processes exact typed immutable values on the host scheduler; no GUI, persistence or native host handle is exposed. */
static HoroExtensionStatus Invoke(void *context, const HoroEditorPageOperation *operation, const HoroEditorPageFindingSink *sink) {
    (void)context;
    if (!HoroEditorPageOperationShape(operation) || operation->valueCount != 1 || operation->values[0].kind != HORO_EDITOR_PAGE_INTEGER)
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    if (operation->cancellation.isCancellationRequested(operation->cancellation.context))
        return HORO_EXTENSION_ERROR_CANCELLED;
    if (operation->operation == HORO_EDITOR_PAGE_VALIDATE && operation->values[0].integerValue < 0) {
        const HoroEditorPageFinding finding = {.structSize = sizeof(HoroEditorPageFinding),
                                               .severity = 2,
                                               .binding = TEXT("page.count"),
                                               .diagnosticCode = TEXT("fixture.negative"),
                                               .messageKey = TEXT("fixture.count")};
        return HoroEditorPageAppendFinding(sink, &finding);
    }
    atomic_store(&lastInteger, operation->values[0].integerValue);
    atomic_fetch_add(&calls[operation->operation], 1);
    return HORO_EXTENSION_SUCCESS;
}

static const HoroExtensionStringView ids[] = {TEXT("fixture.settings"), TEXT("fixture.modal"), TEXT("fixture.child")};
static const HoroExtensionStringView settingPermissions[] = {TEXT("editor.settings"), TEXT("configuration.project.write")};
static const HoroExtensionStringView modalPermissions[] = {TEXT("editor.modal")};
static const HoroExtensionStringView diagnostics[] = {TEXT("fixture.negative")};
static const HoroEditorActivityMessage messages[] = {{TEXT("en-US"), TEXT("fixture.title"), TEXT("Fixture page")},
                                                     {TEXT("tr-TR"), TEXT("fixture.title"), TEXT("Deneme sayfasi")},
                                                     {TEXT("en-US"), TEXT("fixture.count"), TEXT("Exact count")}};
static const HoroEditorPageField fields[] = {{.structSize = sizeof(HoroEditorPageField),
                                              .nodeId = TEXT("fixture.number"),
                                              .labelKey = TEXT("fixture.count"),
                                              .defaultValue = {.structSize = sizeof(HoroEditorPageValue),
                                                               .kind = HORO_EDITOR_PAGE_INTEGER,
                                                               .binding = TEXT("page.count"),
                                                               .integerValue = INT64_C(9007199254740993)},
                                              .maximumStringBytes = 4096}};

/** @brief Builds inert complete contributions; registration only stages the data until whole-module admission commits. */
static HoroExtensionStatus Register(const HoroExtensionHostApi *host, uint32_t index) {
    const HoroEditorPageDescriptor descriptor = {.structSize = sizeof(HoroEditorPageDescriptor),
                                                 .schemaVersion = 1,
                                                 .kind = index,
                                                 .scope = index == 0 ? HORO_EDITOR_PAGE_PROJECT : HORO_EDITOR_PAGE_SESSION,
                                                 .surfaceId = ids[index],
                                                 .labelKey = TEXT("fixture.title"),
                                                 .parentSurfaceId = index == 2 ? ids[1] : (HoroExtensionStringView){0},
                                                 .permissions = index == 0 ? settingPermissions : modalPermissions,
                                                 .permissionCount = index == 0 ? 2 : 1,
                                                 .diagnosticCodes = diagnostics,
                                                 .diagnosticCodeCount = 1,
                                                 .fields = fields,
                                                 .fieldCount = 1,
                                                 .messages = messages,
                                                 .messageCount = 3,
                                                 .invoke = Invoke};
    if (!HoroEditorPageDescriptorShape(&descriptor))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    sessions[index] = (HoroEditorPageSessionApi){.structSize = sizeof(HoroEditorPageSessionApi)};
    const HoroExtensionStatus result = host->registerEditorPage(host->hostContext, &descriptor, &sessions[index]);
    if (result == HORO_EXTENSION_SUCCESS &&
        (sessions[index].sessionVersion != 1 || !sessions[index].context || !sessions[index].open || !sessions[index].close))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    return result;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    *requirements =
        (HoroExtensionRequirements){.structSize = sizeof(*requirements),
                                    .abiMajorVersion = 1,
                                    .minimumHostMinor = legacyQuery ? 4 : 5,
                                    .requiredHostApiSize = legacyQuery ? offsetof(HoroExtensionHostApi, registerEditorPage)
                                                                       : offsetof(HoroExtensionHostApi, registerEditorInspector),
                                    .requiredFunctions = legacyQuery ? 0 : HORO_EXTENSION_REQUIRES_EDITOR_PAGE};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (!host || host->structSize < offsetof(HoroExtensionHostApi, registerEditorPage) + sizeof(host->registerEditorPage) ||
        host->abiMinorVersion < 5 || !host->registerEditorPage || !module || module->structSize < sizeof(*module))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    for (uint32_t i = 0; i < 3; ++i) {
        const HoroExtensionStatus status = Register(host, i);
        if (status != HORO_EXTENSION_SUCCESS)
            return status;
    }
    /* Staged sessions must not open before the containing module load transaction commits. */
    if (HoroEditorPageSetOpen(&sessions[0], 1) == HORO_EXTENSION_SUCCESS)
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    *module = (HoroExtensionModuleApi){.structSize = sizeof(*module),
                                       .moduleContext = sessions,
                                       .moduleId = TEXT("fixture.module"),
                                       .moduleVersion = TEXT("1.0.0")};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    (void)module;
    for (uint32_t i = 0; i < 3; ++i)
        sessions[i] = (HoroEditorPageSessionApi){0};
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_test_page_open(uint32_t index) {
    return index < 3 ? HoroEditorPageSetOpen(&sessions[index], 1) : HORO_EXTENSION_ERROR_INVALID_ARGS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_test_page_close(uint32_t index) {
    return index < 3 ? HoroEditorPageSetOpen(&sessions[index], 0) : HORO_EXTENSION_ERROR_INVALID_ARGS;
}

HORO_EXTENSION_EXPORT int64_t horo_test_page_integer(void) {
    return atomic_load(&lastInteger);
}

HORO_EXTENSION_EXPORT uint32_t horo_test_page_calls(uint32_t operation) {
    return operation < 3 ? atomic_load(&calls[operation]) : 0;
}

/** @brief Selects an explicit old1.4 query before loading this isolated fixture module, never while callbacks run. */
HORO_EXTENSION_EXPORT void horo_test_page_legacy_query(void) {
    legacyQuery = 1;
}
