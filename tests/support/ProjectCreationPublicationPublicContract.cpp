#include <Horo/Editor/ProjectCreationService.h>
#include <Horo/Foundation/Platform.h>
#include <concepts>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    static_assert(std::is_constructible_v<ProjectCreationService, JobSystem &, EngineDataBus &,
                                          std::shared_ptr<Security::SecureRandomSource>, std::shared_ptr<DurableFileSystem>>);
    static_assert(requires(DurableFileSystem &files, const std::filesystem::path &prepared, const std::filesystem::path &destination,
                           AtomicFileReplacementReceipt &receipt, ProjectCreationSnapshot snapshot) {
        { files.PublishDirectoryNoReplaceTracked(prepared, destination, receipt) } -> std::same_as<Result<void>>;
        { snapshot.publication } -> std::same_as<ProjectCreationPublicationState &>;
    });
}  // namespace
