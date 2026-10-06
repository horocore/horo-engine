#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(HoroExtensionHostApi, structSize) == 0, "Host table starts with its size");
_Static_assert(sizeof(HoroExtensionStatus) == sizeof(uint32_t), "Status values have fixed width");
_Static_assert(sizeof(HoroAssetImportSettingKind) == sizeof(uint32_t), "Setting tags have fixed width");
_Static_assert(offsetof(HoroExtensionModuleApi, structSize) == 0, "Module table starts with its size");
_Static_assert(sizeof(((HoroExtensionHostApi *)0)->abiVersion) == sizeof(uint32_t), "ABI version is fixed-width");

_Static_assert(offsetof(HoroExtensionHostApi, registerEditorActivity) > offsetof(HoroExtensionHostApi, registerPlatformServicesProvider),
               "Activity ABI appends after the old host prefix");
_Static_assert(sizeof(((HoroEditorActivitySnapshot *)0)->revision) == sizeof(uint64_t), "Projection revision has fixed width");
_Static_assert(offsetof(HoroExtensionHostApi, registerEditorActivity) ==
                   offsetof(HoroExtensionHostApi, registerPlatformServicesProvider) + sizeof(HoroRegisterPlatformServicesProviderFunc),
               "Activity callback does not change the legacy host prefix");
_Static_assert(_Generic((HoroRegisterEditorActivityFunc)0,
                   HoroExtensionStatus (*)(void *, const struct HoroEditorActivityDescriptor *, struct HoroEditorActivitySessionApi *): 1,
                   default: 0),
               "C11 registration callback source contract is unchanged");
_Static_assert(_Generic((HoroEditorActivityActionFunc)0,
                   HoroExtensionStatus (*)(void *, const HoroEditorActivityAction *, const HoroEditorActivitySnapshotSink *): 1,
                   default: 0),
               "C11 action callback source contract is unchanged");
_Static_assert(offsetof(HoroEditorActivityDescriptor, structSize) == 0, "Activity descriptor starts with its size");
_Static_assert(offsetof(HoroEditorActivitySessionApi, structSize) == 0, "Activity session starts with its size");

/** @brief Compile the public ABI as C, independently of C++ language extensions. */
int main(void) {
    const HoroExtensionHostApi host = {.structSize = sizeof(HoroExtensionHostApi), .abiVersion = HORO_EXTENSION_ABI_VERSION};
    const HoroExtensionModuleApi module = {.structSize = sizeof(HoroExtensionModuleApi)};
    return host.structSize == sizeof(host) && module.structSize == sizeof(module) &&
                   strcmp(HORO_EDITOR_ACTIVITY_HOST_CAPABILITY, "editor.activity") == 0
               ? 0
               : 1;
}
