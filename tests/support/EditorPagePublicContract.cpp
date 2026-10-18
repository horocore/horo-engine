#include "Horo/Extensions/EditorPageAbi.h"
#include "Horo/Extensions/EditorPageHost.h"

#include <cstddef>
#include <type_traits>

namespace {
    using namespace Horo::Extensions;

    // Read-only host borrows cannot register, edit, schedule, focus, retire or publish canonical page state.
    template <auto Method, typename... Args>
    constexpr bool OwnerMutation = std::is_invocable_v<decltype(Method), EditorPageHost &, Args...> &&
                                   !std::is_invocable_v<decltype(Method), const EditorPageHost &, Args...>;
    static_assert(OwnerMutation<&EditorPageHost::Register, EditorSurfaceContextRegistration, EditorPageDefinition,
                                std::shared_ptr<IEditorPageOperations>, std::shared_ptr<ExtensionRetirement>>);
    static_assert(OwnerMutation<&EditorPageHost::Begin, const EditorSurfaceProviderIdentity &, std::string_view>);
    static_assert(
        OwnerMutation<&EditorPageHost::SetValue, const EditorSurfaceProviderIdentity &, std::string_view, std::uint64_t, EditorPageValue>);
    static_assert(OwnerMutation<&EditorPageHost::Reset, const EditorSurfaceProviderIdentity &, std::string_view, std::uint64_t>);
    static_assert(
        OwnerMutation<&EditorPageHost::Apply, const EditorSurfaceProviderIdentity &, std::string_view, std::uint64_t, EditorUiActionId>);
    static_assert(OwnerMutation<&EditorPageHost::Cancel, const EditorSurfaceProviderIdentity &, std::string_view>);
    static_assert(
        OwnerMutation<&EditorPageHost::Focus, const EditorSurfaceProviderIdentity &, std::string_view, std::uint64_t, EditorUiId>);
    static_assert(OwnerMutation<&EditorPageHost::BeginShutdown>);

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
