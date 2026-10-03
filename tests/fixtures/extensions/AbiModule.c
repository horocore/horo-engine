#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>

static uint32_t loadCount;

#if HORO_ABI_FIXTURE_MODE == 10 || HORO_ABI_FIXTURE_MODE == 11
static uint32_t providerRetired;

static HoroExtensionStatus CreateProvider(void *context, void **outCandidate) {
    *outCandidate = context;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus RetireProvider(void *candidate) {
    (void)candidate;
    ++providerRetired;
    return HORO_EXTENSION_SUCCESS;
}

static void DestroyProvider(void *candidate) {
    (void)candidate;
}

HORO_EXTENSION_EXPORT uint32_t horo_test_provider_retired(void) {
    return providerRetired;
}
#endif

#if HORO_ABI_FIXTURE_MODE == 11
static const HoroPlatformProviderSink *providerSink;

static HoroExtensionStatus InitializeServices(void *candidate, uint32_t requiredMask, uint32_t *outMask) {
    (void)candidate;
    *outMask = requiredMask | 1U;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus BeginSession(void *candidate, const HoroPlatformProviderSink *sink) {
    (void)candidate;
    return sink->sessionChanged(sink->context, 1, 2);
}

static HoroExtensionStatus OpenIngress(void *candidate, const HoroPlatformProviderSink *sink) {
    (void)candidate;
    providerSink = sink;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus SubmitOperation(void *candidate, const HoroPlatformProviderOperation *operation) {
    (void)candidate;
    const HoroPlatformProviderCompletion completion = {.structSize = sizeof(HoroPlatformProviderCompletion),
                                                       .requestId = operation->requestId,
                                                       .requestGeneration = operation->requestGeneration,
                                                       .sessionRevision = operation->sessionRevision,
                                                       .service = operation->service,
                                                       .operation = operation->operation,
                                                       .resultCode = 0};
    return providerSink->complete(providerSink->context, &completion);
}

static HoroExtensionStatus CancelOperation(void *candidate, uint64_t id, uint64_t generation) {
    (void)candidate;
    (void)id;
    (void)generation;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus CloseOperation(void *candidate) {
    (void)candidate;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus CloseIngress(void *candidate) {
    (void)candidate;
    providerSink = NULL;
    return HORO_EXTENSION_SUCCESS;
}

static const HoroPlatformProviderOperations providerOperations = {
    .structSize = sizeof(HoroPlatformProviderOperations),
    .version = HORO_PLATFORM_SERVICES_PROVIDER_OPERATIONS_VERSION,
    .initializeServices = InitializeServices,
    .beginSession = BeginSession,
    .openIngress = OpenIngress,
    .submit = SubmitOperation,
    .cancel = CancelOperation,
    .closeAdmission = CloseOperation,
    .closeIngress = CloseIngress,
    .drain = CloseOperation,
    .stopSession = CloseOperation,
    .shutdownServices = CloseOperation,
};
#endif

#if HORO_ABI_FIXTURE_MODE == 4 || HORO_ABI_FIXTURE_MODE == 5 || HORO_ABI_FIXTURE_MODE == 8
static HoroExtensionStatus ImportAsset(void *context, const HoroAssetImportRequest *request, HoroAssetImportResponse *response) {
    (void)context;
    (void)request;
    (void)response;
    return HORO_EXTENSION_SUCCESS;
}

static void DestroyImporter(void *context) {
    uint32_t *destroyCount = (uint32_t *)context;
    ++*destroyCount;
}

static HoroExtensionStatus RegisterFixtureImporters(const HoroExtensionHostApi *host) {
    static const char contributionId[] = "fixture.importer";
    static const char contributionVersion[] = "1.0.0";
    static uint32_t destroyCount;
    const HoroAssetImporterDescriptor importer = {
        .structSize = sizeof(HoroAssetImporterDescriptor),
        .abiVersion = HORO_ASSET_IMPORTER_ABI_VERSION,
        .contributionId = {contributionId, sizeof(contributionId) - 1},
        .contributionVersion = {contributionVersion, sizeof(contributionVersion) - 1},
        .importerContext = HORO_ABI_FIXTURE_MODE == 8 ? NULL : &destroyCount,
        .importAsset = HORO_ABI_FIXTURE_MODE == 5 ? NULL : ImportAsset,
        .destroyImporter = HORO_ABI_FIXTURE_MODE == 8 ? NULL : DestroyImporter,
    };
    const uint32_t registrationAttempts = HORO_ABI_FIXTURE_MODE == 8 ? 257 : 1;
    for (uint32_t index = 0; index < registrationAttempts; ++index) {
        const HoroExtensionStatus status = host->registerAssetImporter(host->hostContext, &importer);
        if (status != HORO_EXTENSION_SUCCESS)
            return status;
    }
    return HORO_EXTENSION_SUCCESS;
}
#endif

/** @brief Expose whether negotiation prevented the load callback. */
HORO_EXTENSION_EXPORT uint32_t horo_test_load_count(void) {
    return loadCount;
}

#if HORO_ABI_FIXTURE_MODE != 0
/** @brief Return a valid or deliberately incompatible inert requirements table. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    *requirements =
        (HoroExtensionRequirements){.structSize = sizeof(HoroExtensionRequirements),
                                    .abiMajorVersion = HORO_EXTENSION_ABI_VERSION,
                                    .minimumHostMinor = HORO_ABI_FIXTURE_MODE == 2    ? 99
                                                        : HORO_ABI_FIXTURE_MODE == 11 ? 3
                                                        : HORO_ABI_FIXTURE_MODE == 10 ? 2
                                                        : HORO_ABI_FIXTURE_MODE == 9  ? 1
                                                                                      : 0,
                                    .requiredHostApiSize =
                                        HORO_ABI_FIXTURE_MODE == 10 || HORO_ABI_FIXTURE_MODE == 11 ? sizeof(HoroExtensionHostApi)
                                        : HORO_ABI_FIXTURE_MODE == 9 ? offsetof(HoroExtensionHostApi, registerPlatformServicesProvider)
                                                                     : offsetof(HoroExtensionHostApi, abiMinorVersion),
                                    .requiredFunctions =
                                        HORO_ABI_FIXTURE_MODE == 10 || HORO_ABI_FIXTURE_MODE == 11
                                            ? HORO_EXTENSION_REQUIRES_PLATFORM_PROVIDER
                                        : HORO_ABI_FIXTURE_MODE == 4 || HORO_ABI_FIXTURE_MODE == 5 || HORO_ABI_FIXTURE_MODE == 8
                                            ? HORO_EXTENSION_REQUIRES_ASSET_IMPORTER
                                            : 0};
    return HORO_EXTENSION_SUCCESS;
}
#endif

/** @brief Return a legacy module prefix or a deliberately truncated result. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *outModule) {
    ++loadCount;
#if HORO_ABI_FIXTURE_MODE == 10 || HORO_ABI_FIXTURE_MODE == 11
    static const char key[] = "example.provider";
    const HoroPlatformServicesProviderDescriptor provider = {
        .structSize = HORO_ABI_FIXTURE_MODE == 11 ? sizeof(HoroPlatformServicesProviderDescriptor)
                                                  : offsetof(HoroPlatformServicesProviderDescriptor, operations),
        .abiVersion =
            HORO_ABI_FIXTURE_MODE == 11 ? HORO_PLATFORM_SERVICES_PROVIDER_ABI_VERSION_2 : HORO_PLATFORM_SERVICES_PROVIDER_ABI_VERSION,
        .providerKey = {key, sizeof(key) - 1},
        .providerId = 42,
        .platformMask = HORO_PLATFORM_PROVIDER_LINUX | HORO_PLATFORM_PROVIDER_MACOS | HORO_PLATFORM_PROVIDER_WINDOWS,
        .profileMask = HORO_PLATFORM_PROVIDER_HEADLESS,
        .serviceMask = 1,
        .interfaceMajor = 1,
        .interfaceMinor = 1,
        .contractMajor = 1,
        .factoryContext = &providerRetired,
        .createCandidate = CreateProvider,
        .retireCandidate = RetireProvider,
        .destroyCandidate = DestroyProvider,
#if HORO_ABI_FIXTURE_MODE == 11
        .operations = &providerOperations,
#endif
    };
    const HoroExtensionStatus providerStatus = host->registerPlatformServicesProvider(host->hostContext, &provider);
    if (providerStatus != HORO_EXTENSION_SUCCESS)
        return providerStatus;
#endif
#if HORO_ABI_FIXTURE_MODE == 4 || HORO_ABI_FIXTURE_MODE == 5 || HORO_ABI_FIXTURE_MODE == 8
    const HoroExtensionStatus registrationStatus = RegisterFixtureImporters(host);
    if (registrationStatus != HORO_EXTENSION_SUCCESS)
        return registrationStatus;
#else
    (void)host;
#endif
    if (HORO_ABI_FIXTURE_MODE == 6)
        return HORO_EXTENSION_ERROR_INIT_FAILED;
    if (HORO_ABI_FIXTURE_MODE == 7) {
        static const char invalidIdentity[] = "invalid";
        *outModule = (HoroExtensionModuleApi){.structSize = sizeof(HoroExtensionModuleApi),
                                              .moduleId = {invalidIdentity, 257},
                                              .moduleVersion = {invalidIdentity, sizeof(invalidIdentity) - 1}};
        return HORO_EXTENSION_SUCCESS;
    }
    outModule->structSize = HORO_ABI_FIXTURE_MODE == 3 ? 1 : offsetof(HoroExtensionModuleApi, moduleId);
    return HORO_EXTENSION_SUCCESS;
}

/** @brief Accept one deterministic host shutdown callback for conformance coverage. */
HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    *module = (HoroExtensionModuleApi){0};
}
