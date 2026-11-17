#include "Horo/Extensions/ExtensionServiceAbi.h"

#define TEXT(s) {s, sizeof(s) - 1}
static unsigned events[4];
static unsigned cursor;

static void note(unsigned value) {
    if (cursor < 4)
        events[cursor++] = value;
}

HORO_EXTENSION_EXPORT unsigned horo_reference_service_event(unsigned index) {
    return index < cursor ? events[index] : 0;
}

static HoroExtensionStatus invoke(void *context, const HoroExtensionServiceRequest *request, HoroExtensionServiceResult *result) {
    (void)context;
    (void)request;
    (void)result;
    note(9);
    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
}

static void shutdown_service(void *context) {
    (void)context;
    note(2);
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    if (!requirements || requirements->structSize < sizeof(*requirements))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    *requirements = (HoroExtensionRequirements){.structSize = sizeof(*requirements),
                                                .abiMajorVersion = 1,
                                                .minimumHostMinor = 8,
                                                .requiredHostApiSize = sizeof(HoroExtensionHostApi),
                                                .requiredFunctions = HORO_EXTENSION_REQUIRES_SERVICES};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (!module || !HoroExtensionHasServiceTransport(host))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    cursor = 0;
    note(1);
    *module = (HoroExtensionModuleApi){.structSize = sizeof(*module),
                                       .moduleContext = &cursor,
                                       .moduleId = TEXT("com.horo.service-failure.backend"),
                                       .moduleVersion = TEXT("1.0.0")};
    static const HoroExtensionServiceMethod methods[] = {{TEXT("add"), 2, 0}};
    const HoroExtensionServiceDescriptor descriptor = {.structSize = sizeof(descriptor),
                                                       .schemaVersion = 1,
                                                       .service = TEXT("com.horo.service-failure.arithmetic"),
                                                       .contract = TEXT("horo.examples.checked-arithmetic"),
                                                       .version = TEXT("1.0.0"),
                                                       .methods = methods,
                                                       .methodCount = 1,
                                                       .invoke = invoke,
                                                       .shutdown = shutdown_service};
    const HoroExtensionStatus registered = host->services->registerService(host->hostContext, &descriptor);
    if (registered != HORO_EXTENSION_SUCCESS)
        return registered;
    return HORO_EXTENSION_ERROR_INIT_FAILED;
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    note(3);
    *module = (HoroExtensionModuleApi){0};
}
