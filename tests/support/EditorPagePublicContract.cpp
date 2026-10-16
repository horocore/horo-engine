#include "Horo/Extensions/EditorPageAbi.h"
#include "Horo/Extensions/EditorPageHost.h"

#include <cstddef>
#include <type_traits>

namespace {
    static_assert(HORO_EXTENSION_ABI_MINOR_VERSION == 5);
    static_assert(offsetof(HoroExtensionHostApi, registerEditorPage) ==
                  offsetof(HoroExtensionHostApi, registerEditorActivity) + sizeof(HoroRegisterEditorActivityFunc));
    static_assert(std::is_standard_layout_v<HoroEditorPageValue> && std::is_trivially_copyable_v<HoroEditorPageValue>);
    static_assert(std::is_standard_layout_v<HoroEditorPageDescriptor> && std::is_trivially_copyable_v<HoroEditorPageDescriptor>);
    static_assert(std::is_standard_layout_v<HoroEditorPageSessionApi> && std::is_trivially_copyable_v<HoroEditorPageSessionApi>);
    static_assert(std::is_same_v<decltype(HoroEditorPageValue::integerValue), std::int64_t>);
    static_assert(!std::is_copy_constructible_v<Horo::Extensions::EditorPageRegistration>);
    static_assert(std::is_nothrow_move_constructible_v<Horo::Extensions::EditorPageRegistration>);
}  // namespace
