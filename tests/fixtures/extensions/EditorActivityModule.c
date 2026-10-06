#include "Horo/Extensions/EditorActivityAbi.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#define TEXT(value) {value, sizeof(value) - 1}
static HoroEditorActivitySessionApi session;
static uint32_t unloadCount;
static uint32_t actionCount;
static atomic_uint holdAction;
static atomic_uint resultPublished;
static const HoroEditorActivityNode nodes[] = {{.structSize = sizeof(HoroEditorActivityNode),
                                                .kind = HORO_EDITOR_ACTIVITY_TEXT,
                                                .flags = HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL,
                                                .id = TEXT("fixture.output"),
                                                .text = TEXT("fixture output")},
                                               {.structSize = sizeof(HoroEditorActivityNode),
                                                .kind = HORO_EDITOR_ACTIVITY_ACTION,
                                                .id = TEXT("fixture.run"),
                                                .labelKey = TEXT("fixture.run.label"),
                                                .actionId = TEXT("fixture.run")}};

static HoroExtensionStatus Invoke(void *context, const HoroEditorActivityAction *action, const HoroEditorActivitySnapshotSink *sink) {
    (void)context;
    if (action->cancellation.isCancellationRequested(action->cancellation.context))
        return HORO_EXTENSION_ERROR_CANCELLED;
    ++actionCount;
    const HoroEditorActivitySnapshot result = {.structSize = sizeof(HoroEditorActivitySnapshot),
                                               .schemaVersion = 1,
                                               .revision = action->revision + 1,
                                               .badgeCount = actionCount,
                                               .nodes = nodes,
                                               .nodeCount = 2};
    const HoroExtensionStatus status = sink->publish(sink->context, &result);
    atomic_store(&resultPublished, 1);
    while (atomic_load(&holdAction) != 0) {
        /* Test-controlled barrier after the result was copied, before successful callback completion. */
    }
    return status;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    *requirements = (HoroExtensionRequirements){.structSize = sizeof(HoroExtensionRequirements),
                                                .abiMajorVersion = 1,
                                                .minimumHostMinor = 4,
                                                .requiredHostApiSize = sizeof(HoroExtensionHostApi),
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
                                                                         .schemaVersion = 1,
                                                                         .revision = 1,
                                                                         .nodes = nodes,
                                                                         .nodeCount = 2},
                                                     .messages = messages,
                                                     .messageCount = 4,
                                                     .invokeAction = Invoke};
    session = (HoroEditorActivitySessionApi){.structSize = sizeof(HoroEditorActivitySessionApi)};
    const HoroExtensionStatus status = host->registerEditorActivity(host->hostContext, &descriptor, &session);
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
                                                 .schemaVersion = 1,
                                                 .revision = 100,
                                                 .nodes = nodes,
                                                 .nodeCount = 2};
    return session.publish(session.context, &snapshot);
}

HORO_EXTENSION_EXPORT void horo_test_activity_hold_action(uint32_t hold) {
    atomic_store(&resultPublished, 0);
    atomic_store(&holdAction, hold);
}

HORO_EXTENSION_EXPORT uint32_t horo_test_activity_result_published(void) {
    return atomic_load(&resultPublished);
}
