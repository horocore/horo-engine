#pragma once
/** @file GuiRuntimeBridge.h
 * @brief Internal development compiler contract; never part of the extension SDK.
 */
#include "Horo/Editor/GuiLiveReload.h"
#include "IObject.h"
#include "RuntimeInclude.h"

#include <array>

RUNTIME_MODIFIABLE_INCLUDE;

/** @brief Stateless replaceable module; all references are borrowed for one synchronous call. */
struct HoroGuiModule : TInterface<IID_ENDInterfaceID, IObject> {
    virtual Horo::Editor::GuiLive::Binding Find(Horo::Editor::GuiLive::Id route, Horo::Editor::GuiLive::Id signature) const noexcept = 0;
};

/** @brief Owner-thread table refreshed by RCC++ Init after publication or rollback. */
struct SystemTable {
    std::array<HoroGuiModule *, 64> modules{};
};

namespace Horo::Editor::Development {
    /** @brief Explicit application composition anchor for generated built-in GUI registrations. */
    void LinkGuiModules();
}  // namespace Horo::Editor::Development
