#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/EditorActivityHost.h"

#include <cstddef>
#include <type_traits>

namespace {
    using ActionCommand = Horo::Result<void> (Horo::Extensions::EditorActivityHost::*)(
        const Horo::Extensions::EditorSurfaceProviderIdentity &, std::string_view, std::string_view, std::string_view, std::uint64_t) const;
    static_assert(std::is_same_v<decltype(&Horo::Extensions::EditorActivityHost::QueueAction), ActionCommand>);

    // C++ tag names and the C11 typedef spelling expose the same append-only transport contract.
    static_assert(std::is_standard_layout_v<HoroEditorActivityNode> && std::is_trivially_copyable_v<HoroEditorActivityNode>);
    static_assert(std::is_standard_layout_v<HoroEditorActivitySnapshot> && std::is_trivially_copyable_v<HoroEditorActivitySnapshot>);
    static_assert(std::is_standard_layout_v<HoroEditorActivityAction> && std::is_trivially_copyable_v<HoroEditorActivityAction>);
    static_assert(std::is_standard_layout_v<HoroEditorActivitySnapshotSink> &&
                  std::is_trivially_copyable_v<HoroEditorActivitySnapshotSink>);
    static_assert(std::is_standard_layout_v<HoroEditorActivityMessage> && std::is_trivially_copyable_v<HoroEditorActivityMessage>);
    static_assert(std::is_standard_layout_v<HoroEditorActivityDescriptor> && std::is_trivially_copyable_v<HoroEditorActivityDescriptor>);
    static_assert(std::is_standard_layout_v<HoroEditorActivitySessionApi> && std::is_trivially_copyable_v<HoroEditorActivitySessionApi>);
    static_assert(std::is_same_v<HoroEditorActivityActionFunc, HoroExtensionStatus (*)(void *, const HoroEditorActivityAction *,
                                                                                       const HoroEditorActivitySnapshotSink *)>);
    static_assert(std::is_same_v<decltype(HoroEditorActivityDescriptor::invokeAction), HoroEditorActivityActionFunc>);
    static_assert(offsetof(HoroExtensionHostApi, registerEditorActivity) ==
                  offsetof(HoroExtensionHostApi, registerPlatformServicesProvider) + sizeof(HoroRegisterPlatformServicesProviderFunc));
    static_assert(offsetof(HoroEditorActivityDescriptor, structSize) == 0);
    static_assert(offsetof(HoroEditorActivitySessionApi, structSize) == 0);
    static_assert(sizeof(HoroEditorActivitySnapshot::revision) == sizeof(std::uint64_t));
}  // namespace
