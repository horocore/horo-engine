#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/GuiScreen.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/IGlobalDockPane.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Editor/Localization/LocalizedGui.h"

#include <type_traits>

namespace {
    static_assert(std::is_base_of_v<Horo::Editor::LocalizedGui, Horo::Editor::EditorModal>);
    static_assert(std::is_base_of_v<Horo::Editor::LocalizedGui, Horo::Editor::GuiScreen>);
    static_assert(std::is_base_of_v<Horo::Editor::LocalizedGui, Horo::Editor::IWorkspacePanel>);
    static_assert(std::is_base_of_v<Horo::Editor::LocalizedGui, Horo::Editor::IGlobalDockPane>);
    using Horo::Editor::EditorGuiContext;
    using Horo::Editor::GuiScreenHost;
    using Horo::Editor::GuiScreenHostComposition;

    static_assert(std::is_aggregate_v<GuiScreenHostComposition>);
    static_assert(std::is_constructible_v<GuiScreenHost, const EditorGuiContext &, GuiScreenHostComposition>);
    static_assert(!std::is_copy_constructible_v<GuiScreenHost> && !std::is_move_constructible_v<GuiScreenHost>);
}  // namespace
