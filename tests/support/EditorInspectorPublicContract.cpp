#include "Horo/Extensions/EditorInspectorAbi.h"
#include "Horo/Extensions/EditorInspectorHost.h"
#include "Horo/Extensions/EditorViewportAbi.h"
#include "Horo/Extensions/EditorViewportHost.h"
#include "Horo/Extensions/ExtensionManager.h"

#include <cstddef>
#include <type_traits>

namespace {
    static_assert(offsetof(HoroExtensionHostApi, registerEditorViewport) ==
                  offsetof(HoroExtensionHostApi, registerEditorInspector) + sizeof(HoroRegisterEditorInspectorFunc));
    static_assert(std::is_trivially_copyable_v<HoroEditorViewportDescriptor>);
    static_assert(std::is_trivially_copyable_v<Horo::Extensions::EditorViewportFrame>);
    static_assert(!std::is_copy_constructible_v<Horo::Extensions::EditorViewportRegistration>);
    static_assert(HORO_EXTENSION_ABI_MINOR_VERSION == 7);
    static_assert(std::is_standard_layout_v<HoroEditorInspectorField> && std::is_trivially_copyable_v<HoroEditorInspectorField>);
    static_assert(std::is_standard_layout_v<HoroEditorInspectorDescriptor> && std::is_trivially_copyable_v<HoroEditorInspectorDescriptor>);
    static_assert(offsetof(HoroExtensionHostApi, registerEditorInspector) ==
                  offsetof(HoroExtensionHostApi, registerEditorPage) + sizeof(HoroRegisterEditorPageFunc));
    static_assert(std::is_same_v<decltype(HoroEditorInspectorField::minimumInteger), std::int64_t>);
    static_assert(!std::is_copy_constructible_v<Horo::Extensions::EditorInspectorRegistration>);
    static_assert(std::is_nothrow_move_constructible_v<Horo::Extensions::EditorInspectorRegistration>);
    static_assert(std::is_same_v<decltype(Horo::Extensions::ExtensionEditorHosts::inspectors),
                                 std::shared_ptr<Horo::Extensions::EditorInspectorHost>>);
}  // namespace
