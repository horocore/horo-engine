#include "Horo/Extensions/ExtensionAbi.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct FixtureAudit {
    char events[128];
    unsigned eventCount;
    unsigned failStage;
    unsigned busy;
    unsigned destroyed;
    unsigned postDestroyCallbacks;
    const HoroPlatformProviderSink *sink;
    atomic_uint heldReady;
    atomic_uint heldRelease;
} FixtureAudit;

static void Record(FixtureAudit *audit, char event) {
    if (audit->eventCount < sizeof(audit->events))
        audit->events[audit->eventCount++] = event;
}

FixtureAudit *horo_test_provider_audit_create(void) {
    FixtureAudit *audit = (FixtureAudit *)calloc(1, sizeof(FixtureAudit));
    if (audit != NULL) {
        atomic_init(&audit->heldReady, 0);
        atomic_init(&audit->heldRelease, 0);
    }
    return audit;
}

void horo_test_provider_audit_free(FixtureAudit *audit) {
    free(audit);
}

void horo_test_provider_fail_at(FixtureAudit *audit, unsigned stage) {
    audit->failStage = stage;
}

void horo_test_provider_set_busy(FixtureAudit *audit, unsigned busy) {
    audit->busy = busy;
}

unsigned horo_test_provider_event_count(const FixtureAudit *audit) {
    return audit->eventCount;
}

char horo_test_provider_event(const FixtureAudit *audit, unsigned index) {
    return index < audit->eventCount ? audit->events[index] : 0;
}

unsigned horo_test_provider_destroyed(const FixtureAudit *audit) {
    return audit->destroyed;
}

unsigned horo_test_provider_post_destroy_callbacks(const FixtureAudit *audit) {
    return audit->postDestroyCallbacks;
}

HoroExtensionStatus horo_test_provider_session_changed(FixtureAudit *audit, uint64_t revision, uint32_t phase) {
    if (audit->sink == NULL)
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    return audit->sink->sessionChanged(audit->sink->context, revision, phase);
}

HoroExtensionStatus horo_test_provider_emit(FixtureAudit *audit, uint64_t requestId, uint64_t generation) {
    if (audit->destroyed) {
        ++audit->postDestroyCallbacks;
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    }
    if (audit->sink == NULL)
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    static const uint8_t payload[] = {1, 2, 3};
    const HoroPlatformProviderCompletion completion = {.structSize = sizeof(HoroPlatformProviderCompletion),
                                                       .requestId = requestId,
                                                       .requestGeneration = generation,
                                                       .sessionRevision = 1,
                                                       .service = 0,
                                                       .operation = 1,
                                                       .resultCode = 0,
                                                       .payload = payload,
                                                       .payloadSize = sizeof(payload)};
    return audit->sink->complete(audit->sink->context, &completion);
}

unsigned horo_test_provider_held_ready(const FixtureAudit *audit) {
    return atomic_load_explicit(&audit->heldReady, memory_order_acquire);
}

void horo_test_provider_release_held(FixtureAudit *audit) {
    atomic_store_explicit(&audit->heldRelease, 1, memory_order_release);
}

HoroExtensionStatus horo_test_provider_emit_held(FixtureAudit *audit, uint64_t requestId, uint64_t generation) {
    const HoroPlatformProviderSink *sink = audit->sink;
    atomic_store_explicit(&audit->heldReady, 1, memory_order_release);
    while (!atomic_load_explicit(&audit->heldRelease, memory_order_acquire)) {
    }
    if (sink == NULL)
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    static const uint8_t payload[] = {1};
    const HoroPlatformProviderCompletion completion = {.structSize = sizeof(HoroPlatformProviderCompletion),
                                                       .requestId = requestId,
                                                       .requestGeneration = generation,
                                                       .sessionRevision = 1,
                                                       .service = 0,
                                                       .operation = 1,
                                                       .resultCode = 0,
                                                       .payload = payload,
                                                       .payloadSize = sizeof(payload)};
    return sink->complete(sink->context, &completion);
}

static HoroExtensionStatus Create(void *context, void **outCandidate) {
    FixtureAudit *audit = (FixtureAudit *)context;
    audit->destroyed = 0;
    Record(audit, 'C');
    *outCandidate = audit;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus Retire(void *candidate) {
    FixtureAudit *audit = (FixtureAudit *)candidate;
    Record(audit, 'R');
    return audit->busy ? HORO_EXTENSION_ERROR_BUSY : HORO_EXTENSION_SUCCESS;
}

static void Destroy(void *candidate) {
    FixtureAudit *audit = (FixtureAudit *)candidate;
    Record(audit, 'Z');
    audit->destroyed = 1;
    audit->sink = NULL;
}

static HoroExtensionStatus Initialize(void *candidate, uint32_t requiredMask, uint32_t *outMask) {
    FixtureAudit *audit = (FixtureAudit *)candidate;
    Record(audit, 'I');
    *outMask = requiredMask | 1U;
    return audit->failStage == 1 ? HORO_EXTENSION_ERROR_INIT_FAILED : HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus BeginSession(void *candidate, const HoroPlatformProviderSink *sink) {
    FixtureAudit *audit = (FixtureAudit *)candidate;
    Record(audit, 'S');
    audit->sink = sink;
    if (sink->sessionChanged(sink->context, 1, 2) != HORO_EXTENSION_SUCCESS)
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    return audit->failStage == 2 ? HORO_EXTENSION_ERROR_INIT_FAILED : HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus OpenIngress(void *candidate, const HoroPlatformProviderSink *sink) {
    FixtureAudit *audit = (FixtureAudit *)candidate;
    Record(audit, 'G');
    audit->sink = sink;
    return audit->failStage == 3 ? HORO_EXTENSION_ERROR_INIT_FAILED : HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus Submit(void *candidate, const HoroPlatformProviderOperation *operation) {
    FixtureAudit *audit = (FixtureAudit *)candidate;
    Record(audit, 'O');
    return operation->structSize == sizeof(HoroPlatformProviderOperation) ? HORO_EXTENSION_SUCCESS : HORO_EXTENSION_ERROR_INVALID_ARGS;
}

static HoroExtensionStatus Cancel(void *candidate, uint64_t requestId, uint64_t generation) {
    (void)requestId;
    (void)generation;
    Record((FixtureAudit *)candidate, 'X');
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus CloseAdmission(void *candidate) {
    Record((FixtureAudit *)candidate, 'A');
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus CloseIngress(void *candidate) {
    Record((FixtureAudit *)candidate, 'K');
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus Drain(void *candidate) {
    FixtureAudit *audit = (FixtureAudit *)candidate;
    Record(audit, 'D');
    if (audit->busy)
        return HORO_EXTENSION_ERROR_BUSY;
    audit->sink = NULL;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus StopSession(void *candidate) {
    Record((FixtureAudit *)candidate, 'T');
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus ShutdownServices(void *candidate) {
    Record((FixtureAudit *)candidate, 'U');
    return HORO_EXTENSION_SUCCESS;
}

HoroPlatformProviderOperations horo_test_provider_operations(void) {
    const HoroPlatformProviderOperations profile = {.structSize = sizeof(HoroPlatformProviderOperations),
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
                                                    .shutdownServices = ShutdownServices};
    return profile;
}

HoroPlatformProviderCreateFunc horo_test_provider_create(void) {
    return Create;
}

HoroPlatformProviderRetireFunc horo_test_provider_retire(void) {
    return Retire;
}

HoroPlatformProviderDestroyFunc horo_test_provider_destroy(void) {
    return Destroy;
}
