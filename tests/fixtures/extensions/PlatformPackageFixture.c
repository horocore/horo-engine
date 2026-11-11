#include "PlatformPackageFixture.h"

#include "Horo/Extensions/ExtensionAbi.h"

#include <stdlib.h>

typedef struct Candidate {
    const HoroPlatformProviderSink *sink;
} Candidate;

static uint32_t flags;
static HoroPackageProviderAudit audit;

HORO_EXTENSION_EXPORT void horo_package_provider_configure(uint32_t value) {
    flags = value;
}

HORO_EXTENSION_EXPORT void horo_package_provider_inspect(HoroPackageProviderAudit *output) {
    *output = audit;
}

static HoroExtensionStatus Create(void *context, void **output) {
    (void)context;
    Candidate *candidate = (Candidate *)calloc(1, sizeof(Candidate));
    if (candidate == NULL)
        return HORO_EXTENSION_ERROR_INIT_FAILED;
    *output = candidate;
    ++audit.created;
    return flags & HORO_PACKAGE_PROVIDER_PARTIAL_CREATE ? HORO_EXTENSION_ERROR_INIT_FAILED : HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus Retire(void *candidate) {
    (void)candidate;
    return flags & HORO_PACKAGE_PROVIDER_BUSY ? HORO_EXTENSION_ERROR_BUSY : HORO_EXTENSION_SUCCESS;
}

static void Destroy(void *candidate) {
    ++audit.destroyed;
    free(candidate);
}

static HoroExtensionStatus Initialize(void *candidate, uint32_t required, uint32_t *available) {
    (void)candidate;
    (void)required;
    *available = 1;
    return flags & HORO_PACKAGE_PROVIDER_SDK_UNAVAILABLE ? HORO_EXTENSION_ERROR_INIT_FAILED : HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus BeginSession(void *candidate, const HoroPlatformProviderSink *sink) {
    (void)candidate;
    return sink->sessionChanged(sink->context, 1, 2);
}

static HoroExtensionStatus OpenIngress(void *candidate, const HoroPlatformProviderSink *sink) {
    ((Candidate *)candidate)->sink = sink;
    ++audit.sinkRetained;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus Submit(void *candidate, const HoroPlatformProviderOperation *operation) {
    ++audit.submitted;
    if (flags & HORO_PACKAGE_PROVIDER_DELAY)
        return HORO_EXTENSION_SUCCESS;
    const HoroPlatformProviderCompletion completion = {.structSize = sizeof(HoroPlatformProviderCompletion),
                                                       .requestId = operation->requestId,
                                                       .requestGeneration = operation->requestGeneration,
                                                       .sessionRevision = operation->sessionRevision,
                                                       .service = operation->service,
                                                       .operation = operation->operation,
                                                       .resultCode = HORO_PLATFORM_PROVIDER_SUCCESS};
    const HoroPlatformProviderSink *sink = ((Candidate *)candidate)->sink;
    return sink->complete(sink->context, &completion);
}

static HoroExtensionStatus Cancel(void *candidate, uint64_t request, uint64_t generation) {
    (void)candidate;
    (void)request;
    (void)generation;
    ++audit.cancelled;
    return flags & HORO_PACKAGE_PROVIDER_CANCEL_REJECTED ? HORO_EXTENSION_ERROR_INIT_FAILED : HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus CloseAdmission(void *candidate) {
    (void)candidate;
    return flags & HORO_PACKAGE_PROVIDER_CLOSE_ADMISSION_FAILED ? HORO_EXTENSION_ERROR_INIT_FAILED : HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus CloseIngress(void *candidate) {
    (void)candidate;
    return flags & HORO_PACKAGE_PROVIDER_CLOSE_INGRESS_FAILED ? HORO_EXTENSION_ERROR_INIT_FAILED : HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus Drain(void *candidate) {
    if (flags & HORO_PACKAGE_PROVIDER_BUSY)
        return HORO_EXTENSION_ERROR_BUSY;
    if (((Candidate *)candidate)->sink != NULL) {
        ((Candidate *)candidate)->sink = NULL;
        --audit.sinkRetained;
    }
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus StopSession(void *candidate) {
    (void)candidate;
    ++audit.sessionStopped;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus Shutdown(void *candidate) {
    (void)candidate;
    ++audit.servicesStopped;
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    *requirements = (HoroExtensionRequirements){.structSize = sizeof(HoroExtensionRequirements),
                                                .abiMajorVersion = HORO_EXTENSION_ABI_VERSION,
                                                .minimumHostMinor = flags & HORO_PACKAGE_PROVIDER_INCOMPATIBLE_ABI ? 99 : 3,
                                                .requiredHostApiSize = offsetof(HoroExtensionHostApi, registerEditorActivity),
                                                .requiredFunctions = HORO_EXTENSION_REQUIRES_PLATFORM_PROVIDER};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    HoroPlatformProviderOperations operations = {.structSize = sizeof(HoroPlatformProviderOperations),
                                                 .version = HORO_PLATFORM_SERVICES_PROVIDER_OPERATIONS_VERSION,
                                                 .initializeServices = Initialize,
                                                 .beginSession = BeginSession,
                                                 .openIngress = OpenIngress,
                                                 .submit = Submit,
                                                 .cancel = Cancel,
                                                 .closeAdmission = CloseAdmission,
                                                 .closeIngress = CloseIngress,
                                                 .drain = Drain,
                                                 .stopSession = StopSession,
                                                 .shutdownServices = Shutdown};
    if (flags & HORO_PACKAGE_PROVIDER_MALFORMED)
        operations.structSize = 1;
    static const char key[] = "example.provider";
    static const char permission[] = "platform.services.private";
    const HoroExtensionStringView permissions[] = {{permission, sizeof(permission) - 1}};
    const HoroPlatformServicesProviderDescriptor descriptor = {.structSize = sizeof(HoroPlatformServicesProviderDescriptor),
                                                               .abiVersion = HORO_PLATFORM_SERVICES_PROVIDER_ABI_VERSION_2,
                                                               .providerKey = {key, sizeof(key) - 1},
                                                               .providerId = 42,
                                                               .platformMask = HORO_PLATFORM_PROVIDER_WINDOWS |
                                                                               HORO_PLATFORM_PROVIDER_MACOS | HORO_PLATFORM_PROVIDER_LINUX,
                                                               .profileMask = flags & HORO_PACKAGE_PROVIDER_WRONG_PROFILE
                                                                                  ? HORO_PLATFORM_PROVIDER_INTERACTIVE
                                                                                  : HORO_PLATFORM_PROVIDER_HEADLESS,
                                                               .serviceMask = 1,
                                                               .interfaceMajor = 1,
                                                               .interfaceMinor = 1,
                                                               .contractMajor = flags & HORO_PACKAGE_PROVIDER_CONTRACT_V2 ? 2 : 1,
                                                               .permissions = permissions,
                                                               .permissionCount = flags & HORO_PACKAGE_PROVIDER_UNAPPROVED ? 1 : 0,
                                                               .createCandidate = Create,
                                                               .retireCandidate = Retire,
                                                               .destroyCandidate = Destroy,
                                                               .operations = &operations};
    const HoroExtensionStatus status = host->registerPlatformServicesProvider(host->hostContext, &descriptor);
    if (status != HORO_EXTENSION_SUCCESS)
        return status;
    *module = (HoroExtensionModuleApi){.structSize = offsetof(HoroExtensionModuleApi, moduleId), .moduleContext = &audit};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    ++audit.unloaded;
    *module = (HoroExtensionModuleApi){0};
}
