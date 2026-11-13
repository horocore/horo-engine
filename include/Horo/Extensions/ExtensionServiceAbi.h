#pragma once

/** @file ExtensionServiceAbi.h
 * @brief ABI1.8 bounded signed-integer service transport for native extensions.
 * @details The host copies descriptors during load. No STL, VM value, allocator,
 * registry, exception, or executable ownership crosses this C11 boundary.
 */
#include "Horo/Extensions/ExtensionAbi.h"

#define HORO_EXTENSION_SERVICE_SCHEMA_VERSION  1u
#define HORO_EXTENSION_SERVICE_MAX_ARGUMENTS   8u
#define HORO_EXTENSION_SERVICE_MAX_METHODS     32u
#define HORO_EXTENSION_SERVICE_ERROR_BYTES     256u
#define HORO_EXTENSION_SERVICE_HOST_CAPABILITY "extension.services"
#define HORO_EXTENSION_SERVICE_CAPABILITY      "horo.extension.services"
#define HORO_EXTENSION_SERVICE_PERMISSION      "extension.services.invoke"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Copied method shape; every argument and the sole result is signed int64. */
struct HoroExtensionServiceMethod {
    HoroExtensionStringView id;
    uint32_t argumentCount;
    uint32_t reserved;
};
typedef struct HoroExtensionServiceMethod HoroExtensionServiceMethod;

/** @brief Closed semantic failure vocabulary of this deliberately narrow scalar profile. */
enum HoroExtensionServiceFailure {
    HORO_EXTENSION_SERVICE_NO_FAILURE = 0,
    HORO_EXTENSION_SERVICE_INVALID_CALL = 1,
    HORO_EXTENSION_SERVICE_OVERFLOW = 2,
    HORO_EXTENSION_SERVICE_REQUESTED_FAILURE = 3,
    HORO_EXTENSION_SERVICE_RETIRED = 4,
    HORO_EXTENSION_SERVICE_FAILURE_COUNT = 5
};

/** @brief Caller-owned bounded output. Failure leaves value unused; text is UTF-8 ASCII subset. */
struct HoroExtensionServiceResult {
    uint32_t structSize;
    uint32_t errorLength;
    uint32_t failureCode;
    uint32_t reserved;
    int64_t value;
    char error[HORO_EXTENSION_SERVICE_ERROR_BYTES];
};
typedef struct HoroExtensionServiceResult HoroExtensionServiceResult;

/** @brief Borrowed request valid only during invoke; cancellation combines caller and provider retirement. */
struct HoroExtensionServiceRequest {
    uint32_t structSize;
    HoroExtensionStringView method;
    const int64_t *arguments;
    uint32_t argumentCount;
    HoroExtensionCancellation cancellation;
};
typedef struct HoroExtensionServiceRequest HoroExtensionServiceRequest;

/** @brief Provider-owned reentrant synchronous implementation. Shutdown runs once after admitted calls drain, before native unload. */
struct HoroExtensionServiceDescriptor {
    uint32_t structSize;
    uint32_t schemaVersion;
    HoroExtensionStringView service;
    HoroExtensionStringView contract;
    HoroExtensionStringView version;
    HoroExtensionStringView scriptApi; /**< Empty means no script export; otherwise an exact manifest script.api claim. */
    const HoroExtensionServiceMethod *methods;
    uint32_t methodCount;
    uint32_t reserved;
    void *provider;
    HoroExtensionStatus (*invoke)(void *, const HoroExtensionServiceRequest *, HoroExtensionServiceResult *);
    void (*shutdown)(void *);
};
typedef struct HoroExtensionServiceDescriptor HoroExtensionServiceDescriptor;

/** @brief Revocable imported call adapter. Context belongs to the host until importing module unload.
 * @details Worker-safe after committed activation; no load-time calls. Each invocation rechecks exact declaration,
 * provider generation, permission, cancellation and native-code lifetime through host registries.
 */
struct HoroExtensionServiceClient {
    uint32_t structSize;
    void *context;
    HoroExtensionStatus (*invoke)(void *, const HoroExtensionServiceRequest *, HoroExtensionServiceResult *);
};
typedef struct HoroExtensionServiceClient HoroExtensionServiceClient;

/** @brief ABI1.8 host table accessed only after exact prefix/required-function negotiation. */
struct HoroExtensionServiceHostApi {
    uint32_t structSize;
    uint32_t schemaVersion;
    HoroExtensionStatus (*registerService)(void *loadContext, const HoroExtensionServiceDescriptor *);
    HoroExtensionStatus (*bindImport)(void *loadContext, HoroExtensionStringView importId, HoroExtensionServiceClient *);
};
typedef struct HoroExtensionServiceHostApi HoroExtensionServiceHostApi;

/** @brief Checks the complete ABI1.8 prefix and closed service table before any module reads it. */
static inline int HoroExtensionHasServiceTransport(const HoroExtensionHostApi *host) {
    return host && host->structSize >= offsetof(HoroExtensionHostApi, services) + sizeof(host->services) &&
           host->abiVersion == HORO_EXTENSION_ABI_VERSION && host->abiMinorVersion >= 8 && host->services &&
           host->services->structSize == sizeof(HoroExtensionServiceHostApi) &&
           host->services->schemaVersion == HORO_EXTENSION_SERVICE_SCHEMA_VERSION && host->services->registerService &&
           host->services->bindImport;
}

#ifdef __cplusplus
}
#endif
