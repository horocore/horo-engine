#include "Horo/Extensions/ExtensionServiceAbi.h"
#define TEXT(s) {s, sizeof(s) - 1}
static unsigned stopped;
static unsigned unloaded;
static void *probe;
static void (*entered)(void *);
static void (*wait_release)(void *, HoroExtensionCancellation);
static HoroExtensionServiceClient client;

HORO_EXTENSION_EXPORT void horo_reference_block(void *context, void (*signal)(void *), void (*wait)(void *, HoroExtensionCancellation)) {
    probe = context;
    entered = signal;
    wait_release = wait;
    stopped = 0;
    unloaded = 0;
}

HORO_EXTENSION_EXPORT unsigned horo_reference_stopped(void) {
    return stopped;
}

HORO_EXTENSION_EXPORT unsigned horo_reference_unloaded(void) {
    return unloaded;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_reference_call(void) {
    const int64_t arguments[] = {20, 22};
    const HoroExtensionServiceRequest request = {.structSize = sizeof(request),
                                                 .method = TEXT("add"),
                                                 .arguments = arguments,
                                                 .argumentCount = 2};
    HoroExtensionServiceResult result = {.structSize = sizeof(result)};
    if (!client.invoke)
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    const HoroExtensionStatus status = client.invoke(client.context, &request, &result);
    return status == HORO_EXTENSION_SUCCESS && result.value == 42 ? HORO_EXTENSION_SUCCESS : HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
}

static HoroExtensionStatus invoke(void *context, const HoroExtensionServiceRequest *request, HoroExtensionServiceResult *result) {
    (void)context;
    (void)request;
    if (entered)
        entered(probe);
    if (wait_release)
        wait_release(probe, request->cancellation);
    result->value = 42;
    return HORO_EXTENSION_SUCCESS;
}

static void shutdown_service(void *context) {
    (void)context;
    ++stopped;
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
#if HORO_REFERENCE_CLIENT
    *module = (HoroExtensionModuleApi){.structSize = sizeof(*module),
                                       .moduleContext = &unloaded,
                                       .moduleId = TEXT("com.horo.concurrent.client"),
                                       .moduleVersion = TEXT("1.0.0")};
    client.structSize = sizeof(client);
    return host->services->bindImport(host->hostContext, (HoroExtensionStringView)TEXT("backend-import"), &client);
#else
    *module = (HoroExtensionModuleApi){.structSize = sizeof(*module),
                                       .moduleContext = &unloaded,
                                       .moduleId = TEXT("com.horo.concurrent.backend"),
                                       .moduleVersion = TEXT("1.0.0")};
    static const HoroExtensionServiceMethod methods[] = {{TEXT("add"), 2, 0}};
    const HoroExtensionServiceDescriptor descriptor = {.structSize = sizeof(descriptor),
                                                       .schemaVersion = 1,
                                                       .service = TEXT("com.horo.concurrent.arithmetic"),
                                                       .contract = TEXT("horo.examples.checked-arithmetic"),
                                                       .version = TEXT("1.0.0"),
                                                       .scriptApi = TEXT("com.horo.concurrent.api"),
                                                       .methods = methods,
                                                       .methodCount = 1,
                                                       .invoke = invoke,
                                                       .shutdown = shutdown_service};
    return host->services->registerService(host->hostContext, &descriptor);
#endif
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    ++unloaded;
    client = (HoroExtensionServiceClient){0};
    *module = (HoroExtensionModuleApi){0};
}
