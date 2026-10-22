#include "Horo/Editor/WorkspaceReadSources.h"

#include <type_traits>

namespace {
    using namespace Horo::Editor;
    using Capture = Horo::Result<WorkspaceReadSources> (*)(const WorkspaceReadSourceFactory *, const std::filesystem::path &);
    static_assert(std::is_same_v<decltype(&CaptureWorkspaceReadSources), Capture>);
    static_assert(std::is_same_v<WorkspaceReadSourceFactory::Capture::result_type, Horo::Result<WorkspaceReadSources>>);
    static_assert(std::is_same_v<decltype(WorkspaceReadSources::world), std::shared_ptr<const Horo::Platform::IProjectReadFiles>>);
    static_assert(std::is_same_v<decltype(WorkspaceReadSources::registry), std::shared_ptr<const Horo::Assets::IAssetRegistrySource>>);
}  // namespace
