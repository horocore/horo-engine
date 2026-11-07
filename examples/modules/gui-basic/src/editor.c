#include "ExampleConfig.h"
#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
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

typedef struct Gui {
    HoroEditorActivitySessionApi session;
    HoroExtensionServiceClient backend;
} Gui;

static const HoroEditorActivityPlotSample samples[] = {{0.0f, 0.2f}, {0.5f, 0.8f}, {1.0f, 0.4f}};
static const HoroEditorUiCustomPresentation style = {sizeof(style),
                                                     1,
                                                     HORO_EDITOR_UI_STANDARD,
                                                     HORO_EDITOR_UI_FONT_SANS_EMPHASIS,
                                                     HORO_EDITOR_UI_ICON_INFO,
                                                     HORO_EDITOR_UI_COLOR_ACCENT,
                                                     HORO_EDITOR_UI_MOTION_FOCUS,
                                                     3};

static HoroExtensionStatus render(uint64_t revision, const char *value, const HoroEditorActivitySnapshotSink *sink) {
    const HoroEditorActivityNodeV2 nodes[] = {{.node = {.structSize = sizeof(HoroEditorActivityNode),
                                                        .kind = HORO_EDITOR_ACTIVITY_ROW,
                                                        .id = TEXT("reference.row")}},
                                              {.node = {.structSize = sizeof(HoroEditorActivityNode),
                                                        .kind = HORO_EDITOR_ACTIVITY_TEXT,
                                                        .flags = HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL,
                                                        .id = TEXT("reference.result"),
                                                        .parent = TEXT("reference.row"),
                                                        .text = {value, (uint32_t)strlen(value)}}},
                                              {.node = {.structSize = sizeof(HoroEditorActivityNode),
                                                        .kind = HORO_EDITOR_ACTIVITY_ACTION,
                                                        .id = TEXT("reference.run"),
                                                        .parent = TEXT("reference.row"),
                                                        .labelKey = TEXT("reference.run.label"),
                                                        .actionId = TEXT("reference.run")}},
                                              {.node = {.structSize = sizeof(HoroEditorActivityNode),
                                                        .kind = HORO_EDITOR_ACTIVITY_PLOT,
                                                        .id = TEXT("reference.plot"),
                                                        .labelKey = TEXT("reference.title"),
                                                        .actionId = TEXT("reference.inspect")},
                                               .samples = samples,
                                               .sampleCount = 3,
                                               .presentation = &style}};
    const HoroEditorActivitySnapshot snapshot = {.structSize = sizeof(snapshot),
                                                 .schemaVersion = 2,
                                                 .revision = revision,
                                                 .nodes = (const HoroEditorActivityNode *)nodes,
                                                 .nodeCount = 4};
    return sink->publish(sink->context, &snapshot);
}

static int action_valid(const HoroEditorActivityAction *input, const HoroEditorActivitySnapshotSink *sink) {
    return input && input->structSize == sizeof(*input) && sink && sink->publish;
}

static HoroExtensionStatus action(void *opaque, const HoroEditorActivityAction *input, const HoroEditorActivitySnapshotSink *sink) {
    Gui *gui = opaque;
    if (!action_valid(input, sink))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    if (cancelled(input->cancellation))
        return HORO_EXTENSION_ERROR_CANCELLED;
    if (!text_equal(input->actionId, "reference.run") && !text_equal(input->actionId, "reference.inspect"))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    if (input->revision == UINT64_MAX)
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
#if EXAMPLE_HAS_BACKEND
    const int64_t arguments[] = {20, 22};
    const HoroExtensionServiceRequest request = {.structSize = sizeof(request),
                                                 .method = TEXT("add"),
                                                 .arguments = arguments,
                                                 .argumentCount = 2,
                                                 .cancellation = input->cancellation};
    HoroExtensionServiceResult result = {.structSize = sizeof(result)};
    const HoroExtensionStatus status = gui->backend.invoke(gui->backend.context, &request, &result);
    if (status != HORO_EXTENSION_SUCCESS)
        return status;
    char text[64];
    snprintf(text, sizeof(text), "20 + 22 = %" PRId64, result.value);
    return render(input->revision + 1, text, sink);
#else
    (void)gui;
    return render(input->revision + 1, "host-rendered form updated", sink);
#endif
}

typedef struct Registration {
    const HoroExtensionHostApi *host;
    Gui *gui;
    HoroEditorActivityDescriptor *descriptor;
} Registration;

static HoroExtensionStatus initial(void *opaque, const HoroEditorActivitySnapshot *snapshot) {
    Registration *registration = opaque;
    registration->descriptor->initialSnapshot = *snapshot;
    return registration->host->registerEditorActivity(registration->host->hostContext, registration->descriptor,
                                                      &registration->gui->session);
}

static HoroExtensionStatus register_gui(const HoroExtensionHostApi *host, Gui *gui) {
    static const HoroEditorActivityMessage messages[] = {{TEXT("en-US"), TEXT("reference.title"), TEXT("Reference module")},
                                                         {TEXT("en-US"), TEXT("reference.run.label"), TEXT("Run checked addition")}};
    HoroEditorActivityDescriptor descriptor = {.structSize = sizeof(descriptor),
                                               .schemaVersion = 1,
                                               .activityId = TEXT(EXAMPLE_PACKAGE ".activity"),
                                               .drawerId = TEXT(EXAMPLE_PACKAGE ".panel"),
                                               .labelKey = TEXT("reference.title"),
                                               .tooltipKey = TEXT("reference.title"),
                                               .iconResource = TEXT("resources/icon.svg"),
                                               .side = HORO_EDITOR_ACTIVITY_LEFT,
                                               .openByDefault = 1,
                                               .messages = messages,
                                               .messageCount = 2,
                                               .moduleContext = gui,
                                               .invokeAction = action};
    Registration registration = {host, gui, &descriptor};
    const HoroEditorActivitySnapshotSink sink = {&registration, initial};
    /* Registration copies inside render while every borrowed node is still alive. */
    gui->session.structSize = sizeof(gui->session);
    return render(1, "reference module ready", &sink);
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (!host_valid(host, module) || !host->registerEditorActivity)
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    Gui *gui = calloc(1, sizeof(*gui));
    if (!gui)
        return HORO_EXTENSION_ERROR_INIT_FAILED;
    module_result(module, gui);
#if EXAMPLE_HAS_BACKEND
    if (!HoroExtensionHasServiceTransport(host))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    gui->backend.structSize = sizeof(gui->backend);
    HoroExtensionStatus status =
        host->services->bindImport(host->hostContext, (HoroExtensionStringView)TEXT(EXAMPLE_IMPORT), &gui->backend);
    if (status != HORO_EXTENSION_SUCCESS)
        return status;
#endif
    return register_gui(host, gui);
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    if (!module)
        return;
    free(module->moduleContext);
    *module = (HoroExtensionModuleApi){0};
}
