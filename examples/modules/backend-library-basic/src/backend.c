#include "ExampleConfig.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int text_equal(HoroExtensionStringView text, const char *expected) {
    const size_t size = strlen(expected);
    return text.data && text.length == size && memcmp(text.data, expected, size) == 0;
}

static int cancelled(HoroExtensionCancellation token) {
    return token.isCancellationRequested && token.isCancellationRequested(token.context);
}

static HoroExtensionStatus module_result(HoroExtensionModuleApi *module, void *state) {
    *module = (HoroExtensionModuleApi){.structSize = sizeof(*module),
                                       .moduleContext = state,
                                       .moduleId = TEXT(EXAMPLE_PACKAGE "." EXAMPLE_ROLE),
                                       .moduleVersion = TEXT(EXAMPLE_VERSION)};
    return HORO_EXTENSION_SUCCESS;
}

static int host_valid(const HoroExtensionHostApi *host, const HoroExtensionModuleApi *module) {
    return host && module && module->structSize >= sizeof(*module) && host->structSize >= sizeof(*host) && host->abiVersion == 1 &&
           host->abiMinorVersion >= 8;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    if (!requirements || requirements->structSize < sizeof(*requirements))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    *requirements = (HoroExtensionRequirements){.structSize = sizeof(*requirements),
                                                .abiMajorVersion = 1,
                                                .minimumHostMinor = 8,
                                                .requiredHostApiSize = sizeof(HoroExtensionHostApi),
                                                .requiredFunctions = EXAMPLE_REQUIRED_FUNCTIONS};
    return HORO_EXTENSION_SUCCESS;
}

typedef struct Provider {
    int stopped;
} Provider;

static HoroExtensionStatus failed(HoroExtensionServiceResult *result, uint32_t code, const char *message) {
    const size_t count = strlen(message);
    memcpy(result->error, message, count);
    result->errorLength = (uint32_t)count;
    result->failureCode = code;
    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
}

static int overflow(int64_t left, int64_t right) {
    return (right > 0 && left > INT64_MAX - right) || (right < 0 && left < INT64_MIN - right);
}

static int request_valid(const Provider *provider, const HoroExtensionServiceRequest *request, const HoroExtensionServiceResult *result) {
    return provider && request && result && request->structSize == sizeof(*request) && result->structSize == sizeof(*result);
}

static HoroExtensionStatus invoke(void *opaque, const HoroExtensionServiceRequest *request, HoroExtensionServiceResult *result) {
    const Provider *provider = opaque;
    if (!request_valid(provider, request, result))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    if (cancelled(request->cancellation))
        return HORO_EXTENSION_ERROR_CANCELLED;
    if (provider->stopped)
        return failed(result, HORO_EXTENSION_SERVICE_RETIRED, "provider retired");
    if (request->argumentCount != 2 || !request->arguments)
        return failed(result, HORO_EXTENSION_SERVICE_INVALID_CALL, "two signed integers required");
    if (text_equal(request->method, "fail"))
        return failed(result, HORO_EXTENSION_SERVICE_REQUESTED_FAILURE, "requested reference failure");
    if (!text_equal(request->method, "add"))
        return failed(result, HORO_EXTENSION_SERVICE_INVALID_CALL, "unknown method");
    if (overflow(request->arguments[0], request->arguments[1]))
        return failed(result, HORO_EXTENSION_SERVICE_OVERFLOW, "signed addition overflow");
    result->value = request->arguments[0] + request->arguments[1];
    result->errorLength = 0;
    result->failureCode = 0;
    return HORO_EXTENSION_SUCCESS;
}

static void shutdown_provider(void *opaque) {
    ((Provider *)opaque)->stopped = 1;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (!host_valid(host, module) || !HoroExtensionHasServiceTransport(host))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    Provider *provider = calloc(1, sizeof(*provider));
    if (!provider)
        return HORO_EXTENSION_ERROR_INIT_FAILED;
    module_result(module, provider);
    static const HoroExtensionServiceMethod methods[] = {{TEXT("add"), 2, 0}, {TEXT("fail"), 2, 0}};
    const HoroExtensionServiceDescriptor descriptor = {.structSize = sizeof(descriptor),
                                                       .schemaVersion = 1,
                                                       .service = TEXT(EXAMPLE_SERVICE),
                                                       .contract = TEXT(EXAMPLE_CONTRACT),
                                                       .version = TEXT(EXAMPLE_VERSION),
#if EXAMPLE_HAS_SCRIPT
                                                       .scriptApi = TEXT(EXAMPLE_SCRIPT_API),
#endif
                                                       .methods = methods,
                                                       .methodCount = 2,
                                                       .provider = provider,
                                                       .invoke = invoke,
                                                       .shutdown = shutdown_provider};
    return host->services->registerService(host->hostContext, &descriptor);
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    if (!module)
        return;
    free(module->moduleContext);
    *module = (HoroExtensionModuleApi){0};
}
