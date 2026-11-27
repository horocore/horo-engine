#include "Horo/Editor/NativeAssetRegistrySource.h"

#include "Horo/Application/ProjectAssetRegistrySource.h"
#include "Horo/Platform/NativeProjectReadFiles.h"

#include <new>

namespace Horo::Editor {
    /** @copydoc CreateNativeAssetRegistrySource */
    Result<std::shared_ptr<const Assets::IAssetRegistrySource>> CreateNativeAssetRegistrySource(
        const std::filesystem::path &root, std::string identity, const std::uint64_t generation,
        const AssetRegistryInventoryPolicy &policy) {
        using Return = Result<std::shared_ptr<const Assets::IAssetRegistrySource>>;
        if (AssetRegistryInventoryPolicy hard; policy.maximumEntries == 0 || policy.maximumEntries > hard.maximumEntries ||
                                               policy.maximumManifestBytes == 0 ||
                                               policy.maximumManifestBytes > hard.maximumManifestBytes ||
                                               policy.maximumDuration.count() <= 0 || policy.maximumDuration > hard.maximumDuration)
            return Return::Failure(MakeError(Platform::ProjectReadErrors::Invalid));
        try {
            auto files = Platform::CreateNativeProjectReadFiles(root, identity, generation);
            if (files.HasError())
                return Return::Failure(files.ErrorValue());
            return Application::CreateProjectAssetRegistrySource(std::move(files).Value(),
                                                                 {.projectIdentity = std::move(identity),
                                                                  .projectGeneration = generation,
                                                                  .deadline = std::chrono::steady_clock::time_point::max()},
                                                                 {policy.maximumEntries, policy.maximumManifestBytes,
                                                                  policy.maximumDuration});
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
        }
    }
}  // namespace Horo::Editor
