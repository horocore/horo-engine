#include "Horo/Extensions/EditorActivityAbi.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#define TEXT(value) {value, sizeof(value) - 1}
static HoroEditorActivitySessionApi session;
static uint32_t unloadCount;
static uint32_t actionCount;
static atomic_uint holdAction;
static atomic_uint failAction;
static atomic_uint resultPublished;
static const HoroEditorActivityPlotSample samples[] = {{0.0f, 0.2f}, {0.3f, 0.8f}, {0.7f, 0.4f}, {1.0f, 0.6f}};
/* Real native-package reference: standard Horo text/action and specialized plot share one surface. */
static const HoroEditorActivityNodeV2 nodes[] = {{.node = {.structSize = sizeof(HoroEditorActivityNode),
                                                           .kind = HORO_EDITOR_ACTIVITY_TEXT,
                                                           .flags = HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL,
                                                           .id = TEXT("fixture.output"),
                                                           .text = TEXT("fixture output")}},
                                                 {.node = {.structSize = sizeof(HoroEditorActivityNode),
                                                           .kind = HORO_EDITOR_ACTIVITY_ACTION,
                                                           .id = TEXT("fixture.run"),
                                                           .labelKey = TEXT("fixture.run.label"),
                                                           .actionId = TEXT("fixture.run")}},
                                                 {.node = {.structSize = sizeof(HoroEditorActivityNode),
                                                           .kind = HORO_EDITOR_ACTIVITY_PLOT,
                                                           .id = TEXT("fixture.plot"),
                                                           .labelKey = TEXT("fixture.label"),
                                                           .actionId = TEXT("fixture.inspect")},
                                                  .samples = samples,
                                                  .sampleCount = 4}};

static HoroExtensionStatus Invoke(void *context, const HoroEditorActivityAction *action, const HoroEditorActivitySnapshotSink *sink) {
    (void)context;
    if (action == NULL || action->structSize < sizeof(HoroEditorActivityAction))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    if (action->cancellation.isCancellationRequested == NULL || sink == NULL || sink->publish == NULL)
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    if (action->cancellation.isCancellationRequested(action->cancellation.context))
        return HORO_EXTENSION_ERROR_CANCELLED;
    ++actionCount;
    const HoroEditorActivitySnapshot result = {.structSize = sizeof(HoroEditorActivitySnapshot),
                                               .schemaVersion = HORO_EDITOR_ACTIVITY_PLOT_SCHEMA_VERSION,
                                               .revision = action->revision + 1,
                                               .badgeCount = actionCount,
                                               .nodes = (const HoroEditorActivityNode *)nodes,
                                               .nodeCount = 3};
    const HoroExtensionStatus status = sink->publish(sink->context, &result);
    atomic_store(&resultPublished, 1);
    while (atomic_load(&holdAction) != 0) {
        /* Test-controlled barrier after the result was copied, before successful callback completion. */
    }
    return atomic_exchange(&failAction, 0) ? HORO_EXTENSION_ERROR_INIT_FAILED : status;
}

HORO_EXTENSION_EXPORT void horo_test_activity_fail_next_action(void) {
    atomic_store(&failAction, 1);
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    *requirements = (HoroExtensionRequirements){.structSize = sizeof(HoroExtensionRequirements),
                                                .abiMajorVersion = 1,
                                                .minimumHostMinor = 4,
                                                .requiredHostApiSize = offsetof(HoroExtensionHostApi, registerEditorPage),
                                                .requiredFunctions = HORO_EXTENSION_REQUIRES_EDITOR_ACTIVITY};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (!host->registerEditorActivity)
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    static const HoroEditorActivityMessage messages[] = {{TEXT("en-US"), TEXT("fixture.label"), TEXT("Fixture drawer")},
                                                         {TEXT("tr-TR"), TEXT("fixture.label"), TEXT("Deneme cekmecesi")},
                                                         {TEXT("en-US"), TEXT("fixture.run.label"), TEXT("Run")},
                                                         {TEXT("tr-TR"), TEXT("fixture.run.label"), TEXT("Calistir")}};
    const HoroEditorActivityDescriptor descriptor = {.structSize = sizeof(HoroEditorActivityDescriptor),
                                                     .schemaVersion = 1,
                                                     .activityId = TEXT("fixture.activity"),
                                                     .drawerId = TEXT("fixture.drawer"),
                                                     .labelKey = TEXT("fixture.label"),
                                                     .tooltipKey = TEXT("fixture.label"),
                                                     .iconResource = TEXT("icons/tool.svg"),
                                                     .side = HORO_EDITOR_ACTIVITY_LEFT,
                                                     .initialSnapshot = {.structSize = sizeof(HoroEditorActivitySnapshot),
                                                                         .schemaVersion = HORO_EDITOR_ACTIVITY_PLOT_SCHEMA_VERSION,
                                                                         .revision = 1,
                                                                         .nodes = (const HoroEditorActivityNode *)nodes,
                                                                         .nodeCount = 3},
                                                     .messages = messages,
                                                     .messageCount = 4,
                                                     .invokeAction = Invoke};
    session = (HoroEditorActivitySessionApi){.structSize = sizeof(HoroEditorActivitySessionApi)};
    const HoroExtensionStatus status = host->registerEditorActivity(host->hostContext, &descriptor, &session);
    if (status == HORO_EXTENSION_SUCCESS &&
        (session.structSize < sizeof(HoroEditorActivitySessionApi) || session.sessionVersion != HORO_EDITOR_ACTIVITY_SESSION_VERSION ||
         (session.supportedNodeMask & (1U << HORO_EDITOR_ACTIVITY_ACTION)) == 0 ||
         (session.supportedNodeMask & (1U << HORO_EDITOR_ACTIVITY_PLOT)) == 0 || !session.context || !session.publish))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    *module = (HoroExtensionModuleApi){.structSize = sizeof(HoroExtensionModuleApi),
                                       .moduleId = TEXT("fixture.module"),
                                       .moduleVersion = TEXT("1.0.0")};
    return status;
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    (void)module;
    ++unloadCount;
}

HORO_EXTENSION_EXPORT uint32_t horo_test_activity_unload_count(void) {
    return unloadCount;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_test_activity_publish(void) {
    const HoroEditorActivitySnapshot snapshot = {.structSize = sizeof(HoroEditorActivitySnapshot),
                                                 .schemaVersion = HORO_EDITOR_ACTIVITY_PLOT_SCHEMA_VERSION,
                                                 .revision = 100,
                                                 .nodes = (const HoroEditorActivityNode *)nodes,
                                                 .nodeCount = 3};
    return session.publish(session.context, &snapshot);
}

HORO_EXTENSION_EXPORT void horo_test_activity_hold_action(uint32_t hold) {
    atomic_store(&resultPublished, 0);
    atomic_store(&holdAction, hold);
}

HORO_EXTENSION_EXPORT uint32_t horo_test_activity_result_published(void) {
    return atomic_load(&resultPublished);
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_test_activity_invoke_prefix(const HoroEditorActivityAction *action) {
    return Invoke(NULL, action, NULL);
}
