#include "Horo/Editor/GuiScreenHost.h"

#include <type_traits>

namespace {
    using Horo::Editor::EditorGuiContext;
    using Horo::Editor::GuiScreenHost;
    using Horo::Editor::GuiScreenHostComposition;

    static_assert(std::is_aggregate_v<GuiScreenHostComposition>);
    static_assert(std::is_constructible_v<GuiScreenHost, const EditorGuiContext &, GuiScreenHostComposition>);
    static_assert(!std::is_copy_constructible_v<GuiScreenHost> && !std::is_move_constructible_v<GuiScreenHost>);
}  // namespace
