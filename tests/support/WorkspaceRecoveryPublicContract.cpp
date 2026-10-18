#include "Horo/Editor/WorkspaceLayoutPersistence.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "Horo/Extensions/EditorSurfaceRegistry.h"
#include "Horo/Foundation/Platform.h"

#include <type_traits>

namespace {
    using Save = bool (*)(Horo::DurableFileSystem &, Horo::AtomicFileReplacementReceipt &, const std::filesystem::path &,
                          const Horo::Editor::WorkspaceLayout &, std::string *);
    static_assert(std::is_same_v<decltype(static_cast<Save>(&Horo::Editor::WorkspaceLayoutPersistence::Save)), Save>);
    using SaveLayout = bool (Horo::Editor::WorkspacePanelHost::*)(Horo::DurableFileSystem &, Horo::AtomicFileReplacementReceipt &,
                                                                  const std::filesystem::path &, std::string *) const;
    static_assert(std::is_same_v<decltype(&Horo::Editor::WorkspacePanelHost::SaveLayout), SaveLayout>);
    static_assert(static_cast<unsigned>(Horo::Extensions::EditorSurfaceProviderStatus::Active) == 0);
    static_assert(static_cast<unsigned>(Horo::Extensions::EditorSurfaceProviderStatus::Disabled) == 1);
    static_assert(static_cast<unsigned>(Horo::Extensions::EditorSurfaceProviderStatus::Missing) == 2);
    static_assert(std::is_same_v<decltype(std::declval<const Horo::Extensions::EditorSurfaceRegistry &>().RecoverySnapshot()),
                                 std::vector<Horo::Extensions::EditorSurfaceRecoverySnapshot>>);
}  // namespace
