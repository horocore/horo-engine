/* Public C ABI contract unit test; real host activation is qualified separately. */
#include "ExampleConfig.h"
#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *);
HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *, HoroExtensionModuleApi *);
void horo_extension_unload(HoroExtensionModuleApi *);

#define CHECK(value)                                                                                                                       \
    do {                                                                                                                                   \
        if (!(value)) {                                                                                                                    \
            fprintf(stderr, "contract failed at line %d\n", __LINE__);                                                                     \
            return 1;                                                                                                                      \
        }                                                                                                                                  \
    } while (0)
static HoroExtensionServiceDescriptor provider;
static HoroEditorActivityDescriptor activity;
static int copied_nodes;
static int copied_results;

static uint8_t is_cancelled(const void *context) {
    return *(const uint8_t *)context;
}

static HoroExtensionStatus register_service(void *context, const HoroExtensionServiceDescriptor *descriptor) {
    (void)context;
    provider = *descriptor;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus imported(void *context, const HoroExtensionServiceRequest *request, HoroExtensionServiceResult *result) {
    (void)context;
    if (request->argumentCount != 2)
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    result->value = request->arguments[0] + request->arguments[1];
    result->errorLength = 0;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus bind_import(void *context, HoroExtensionStringView id, HoroExtensionServiceClient *client) {
    (void)context;
    if (id.length != sizeof(EXAMPLE_IMPORT) - 1 || memcmp(id.data, EXAMPLE_IMPORT, id.length))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    *client = (HoroExtensionServiceClient){sizeof(*client), NULL, imported};
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus publish(void *context, const HoroEditorActivitySnapshot *snapshot) {
    (void)context;
    if (snapshot->schemaVersion != 2 || snapshot->nodeCount != 4 || snapshot->revision != 2)
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    const HoroEditorActivityNodeV2 *nodes = (const HoroEditorActivityNodeV2 *)snapshot->nodes;
    if (!HoroEditorUiCustomPresentationShape(nodes[3].presentation))
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    ++copied_results;
    return HORO_EXTENSION_SUCCESS;
}

static HoroExtensionStatus register_activity(void *context, const HoroEditorActivityDescriptor *descriptor,
                                             HoroEditorActivitySessionApi *session) {
    (void)context;
    activity = *descriptor;
    const HoroEditorActivityNodeV2 *nodes = (const HoroEditorActivityNodeV2 *)descriptor->initialSnapshot.nodes;
    if (descriptor->initialSnapshot.nodeCount != 4 || !HoroEditorUiCustomPresentationShape(nodes[3].presentation))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    copied_nodes = 4;
    *session = (HoroEditorActivitySessionApi){.structSize = sizeof(*session),
                                              .sessionVersion = 1,
                                              .supportedNodeMask = UINT32_MAX,
                                              .publish = publish};
    return HORO_EXTENSION_SUCCESS;
}

static int query_contract(void) {
    HoroExtensionRequirements query = {.structSize = sizeof(query)};
    CHECK(horo_extension_query(NULL) == HORO_EXTENSION_ERROR_INVALID_ARGS);
    CHECK(horo_extension_query(&query) == HORO_EXTENSION_SUCCESS);
    CHECK(query.minimumHostMinor == 8 && query.requiredHostApiSize == sizeof(HoroExtensionHostApi));
    return 0;
}
#if EXAMPLE_IS_BACKEND
static int backend_contract(void) {
    const int64_t arguments[] = {20, 22};
    HoroExtensionServiceRequest request = {.structSize = sizeof(request),
                                           .method = TEXT("add"),
                                           .arguments = arguments,
                                           .argumentCount = 2};
    HoroExtensionServiceResult result = {.structSize = sizeof(result)};
    CHECK(provider.invoke(provider.provider, &request, &result) == HORO_EXTENSION_SUCCESS);
    CHECK(result.value == 42 && result.errorLength == 0);
    request.method = (HoroExtensionStringView)TEXT("fail");
    CHECK(provider.invoke(provider.provider, &request, &result) != HORO_EXTENSION_SUCCESS && result.errorLength > 0);
    return 0;
}

static int backend_boundaries(void) {
    const int64_t arguments[] = {INT64_MAX, 1};
    HoroExtensionServiceRequest request = {.structSize = sizeof(request),
                                           .method = TEXT("add"),
                                           .arguments = arguments,
                                           .argumentCount = 2};
    HoroExtensionServiceResult result = {.structSize = sizeof(result)};
    CHECK(provider.invoke(provider.provider, &request, &result) == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
    const uint8_t cancel = 1;
    request.cancellation = (HoroExtensionCancellation){&cancel, is_cancelled};
    CHECK(provider.invoke(provider.provider, &request, &result) == HORO_EXTENSION_ERROR_CANCELLED);
    request.cancellation = (HoroExtensionCancellation){0};
    provider.shutdown(provider.provider);
    CHECK(provider.invoke(provider.provider, &request, &result) == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
    return 0;
}
#endif
#if EXAMPLE_IS_EDITOR
static int editor_contract(void) {
    CHECK(copied_nodes == 4);
    const uint8_t cancel = 1;
    HoroEditorActivityAction action = {.structSize = sizeof(action),
                                       .nodeId = TEXT("reference.run"),
                                       .actionId = TEXT("reference.run"),
                                       .revision = 1,
                                       .cancellation = {&cancel, is_cancelled}};
    const HoroEditorActivitySnapshotSink sink = {NULL, publish};
    CHECK(activity.invokeAction(activity.moduleContext, &action, &sink) == HORO_EXTENSION_ERROR_CANCELLED);
    CHECK(copied_results == 0);
    action.cancellation = (HoroExtensionCancellation){0};
    CHECK(activity.invokeAction(activity.moduleContext, &action, &sink) == HORO_EXTENSION_SUCCESS);
    CHECK(copied_results == 1);
    return 0;
}
#endif
int main(void) {
    CHECK(query_contract() == 0);
    const HoroExtensionServiceHostApi services = {sizeof(services), 1, register_service, bind_import};
    HoroExtensionHostApi host = {.structSize = sizeof(host),
                                 .abiVersion = 1,
                                 .abiMinorVersion = 8,
                                 .registerEditorActivity = register_activity,
                                 .services = &services};
    HoroExtensionModuleApi module = {.structSize = sizeof(module)};
    CHECK(horo_extension_load(NULL, &module) == HORO_EXTENSION_ERROR_VERSION_MISMATCH);
    CHECK(horo_extension_load(&host, &module) == HORO_EXTENSION_SUCCESS);
#if EXAMPLE_IS_BACKEND
    CHECK(backend_contract() == 0);
    CHECK(backend_boundaries() == 0);
#endif
#if EXAMPLE_IS_EDITOR
    CHECK(editor_contract() == 0);
#endif
    horo_extension_unload(&module);
    CHECK(module.moduleContext == NULL);
    puts("reference ABI contract passed");
    return 0;
}
