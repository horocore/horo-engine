#include "Horo/Extensions/EditorActivityHost.h"

#include <type_traits>

namespace {
    using ActionCommand = Horo::Result<void> (Horo::Extensions::EditorActivityHost::*)(
        const Horo::Extensions::EditorSurfaceProviderIdentity &, std::string_view, std::string_view, std::string_view, std::uint64_t) const;
    static_assert(std::is_same_v<decltype(&Horo::Extensions::EditorActivityHost::QueueAction), ActionCommand>);
}  // namespace
