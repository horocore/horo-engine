#pragma once

#include "Horo/Editor/EditorSettingsStore.h"

#include <iosfwd>

namespace Horo::Editor::SettingsStoreInternal {
    void WriteSettings(std::ostream &out, const EditorSettings &settings);
}  // namespace Horo::Editor::SettingsStoreInternal
