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

typedef struct Consumer {
    HoroExtensionServiceClient backend;
} Consumer;

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (!host_valid(host, module) || !HoroExtensionHasServiceTransport(host))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    Consumer *consumer = calloc(1, sizeof(*consumer));
    if (!consumer)
        return HORO_EXTENSION_ERROR_INIT_FAILED;
    module_result(module, consumer);
    consumer->backend.structSize = sizeof(consumer->backend);
    return host->services->bindImport(host->hostContext, (HoroExtensionStringView)TEXT(EXAMPLE_IMPORT), &consumer->backend);
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    if (!module)
        return;
    free(module->moduleContext);
    *module = (HoroExtensionModuleApi){0};
}
