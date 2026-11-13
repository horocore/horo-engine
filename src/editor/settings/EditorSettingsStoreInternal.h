#pragma once

#include "Horo/Editor/EditorSettingsStore.h"

#include <iosfwd>
#include <string_view>

namespace Horo::Editor::SettingsStoreInternal {
    void WriteSettings(std::ostream &out, const EditorSettings &settings, std::string_view configuration = {});
}  // namespace Horo::Editor::SettingsStoreInternal
