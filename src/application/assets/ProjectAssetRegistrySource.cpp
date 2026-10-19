#include "Horo/Application/ProjectAssetRegistrySource.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <new>
#include <numeric>

namespace Horo::Application {
    namespace {
        /** @brief Maps independent owning enums without depending on their numeric layout. */
        Assets::AssetRegistrySourceKind AssetKind(const Platform::ProjectInventoryKind kind) {
            switch (kind) {
                case Platform::ProjectInventoryKind::Regular:
                    return Assets::AssetRegistrySourceKind::Regular;
                case Platform::ProjectInventoryKind::Directory:
                    return Assets::AssetRegistrySourceKind::Directory;
                case Platform::ProjectInventoryKind::Link:
                    return Assets::AssetRegistrySourceKind::Link;
                case Platform::ProjectInventoryKind::Special:
                    return Assets::AssetRegistrySourceKind::Special;
            }
            return Assets::AssetRegistrySourceKind::Special;
        }

        /** @brief Preserves uppercase-directory precedence while refusing any selected symbolic root alias. */
        Result<std::string> FindAssetRoot(const Platform::ProjectInventory &inventory) {
            std::string selected;
            bool upperLink{};
            bool lowerLink{};
            for (const auto &entry : inventory.entries) {
                if (entry.kind == Platform::ProjectInventoryKind::Directory && (entry.path == "Assets" || entry.path == "assets")) {
                    if (selected.empty() || entry.path == "Assets")
                        selected = entry.path;
                } else if (entry.kind == Platform::ProjectInventoryKind::Link) {
                    upperLink = upperLink || entry.path == "Assets";
                    lowerLink = lowerLink || entry.path == "assets";
                }
            }
            if ((selected != "Assets" && upperLink) || (selected.empty() && lowerLink))
                return Result<std::string>::Failure(MakeError(Platform::ProjectReadErrors::UnsafePath));
            if (selected.empty())
                return Result<std::string>::Failure(MakeError(Platform::ProjectReadErrors::ReadFailed));
            return Result<std::string>::Success(std::move(selected));
        }

        /** @brief Adapts native no-follow inventory to Assets' canonical classifier without decoding sidecars. */
        class ProjectAssetSource final : public Assets::IAssetRegistrySource {
        public:
            /** @brief Retains explicit immutable read authority and per-opening inventory policy. */
            ProjectAssetSource(std::shared_ptr<const Platform::IProjectReadFiles> files, Platform::ProjectReadContext context,
                               Platform::ProjectInventoryLimits policy)
                : files_(std::move(files)), context_(std::move(context)), policy_(policy) {}

            /** @copydoc Assets::IAssetRegistrySource::Capture */
            Result<Assets::AssetRegistrySourceManifest> Capture() const override {
                try {
                    const auto context = Context();
                    if (auto admitted = Platform::CheckProjectReadContext(context); admitted.HasError())
                        return Result<Assets::AssetRegistrySourceManifest>::Failure(admitted.ErrorValue());
                    auto roots = files_->Inventory({}, Platform::ProjectInventoryDepth::Children, context, Limits());
                    if (roots.HasError())
                        return Result<Assets::AssetRegistrySourceManifest>::Failure(roots.ErrorValue());
                    auto selectedRoot = FindAssetRoot(roots.Value());
                    if (selectedRoot.HasError())
                        return Result<Assets::AssetRegistrySourceManifest>::Failure(selectedRoot.ErrorValue());
                    std::string root = std::move(selectedRoot).Value();
                    auto limits = Limits();
                    const auto rootBytes = std::accumulate(roots.Value().entries.begin(), roots.Value().entries.end(), std::size_t{},
                                                           [](std::size_t bytes, const auto &entry) {
                        return bytes + entry.path.size() + 1;
                    });
                    if (roots.Value().entries.size() > limits.maximumEntries || rootBytes > limits.maximumManifestBytes)
                        return Result<Assets::AssetRegistrySourceManifest>::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
                    limits.maximumEntries -= roots.Value().entries.size();
                    limits.maximumManifestBytes -= rootBytes;
                    auto entries = files_->Inventory(root, Platform::ProjectInventoryDepth::Recursive, context, limits);
                    if (entries.HasError())
                        return Result<Assets::AssetRegistrySourceManifest>::Failure(entries.ErrorValue());
                    auto inventory = std::move(entries).Value();
                    Assets::AssetRegistrySourceManifest result{std::move(root), {}, context.deadline};
                    result.entries.reserve(inventory.entries.size());
                    for (auto &entry : inventory.entries)
                        result.entries.emplace_back(std::move(entry.path), AssetKind(entry.kind));
                    return Result<Assets::AssetRegistrySourceManifest>::Success(std::move(result));
                } catch (const std::bad_alloc &) {
                    return Result<Assets::AssetRegistrySourceManifest>::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
                }
            }

            /** @copydoc Assets::IAssetRegistrySource::ReadSidecar */
            Result<std::string> ReadSidecar(std::string_view path, std::chrono::steady_clock::time_point deadline) const override {
                try {
                    auto context = Context();
                    context.deadline = std::min(context.deadline, deadline);
                    if (auto admitted = Platform::CheckProjectReadContext(context); admitted.HasError())
                        return Result<std::string>::Failure(admitted.ErrorValue());
                    auto bytes = files_->InventoryText(path, context);
                    if (bytes.HasError())
                        return Result<std::string>::Failure(bytes.ErrorValue());
                    return Result<std::string>::Success(*bytes.Value().text);
                } catch (const std::bad_alloc &) {
                    return Result<std::string>::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
                }
            }

        private:
            /** @brief Issues a fresh synchronous deadline while retaining the exact opening authority. */
            Platform::ProjectReadContext Context() const {
                auto context = context_;
                context.deadline = std::min(context.deadline, std::chrono::steady_clock::now() + policy_.maximumDuration);
                return context;
            }

            /** @brief Preserves the explicitly selected independent inventory policy. */
            Platform::ProjectInventoryLimits Limits() const {
                return {policy_.maximumEntries, policy_.maximumManifestBytes, policy_.maximumDuration};
            }

            std::shared_ptr<const Platform::IProjectReadFiles> files_;
            Platform::ProjectReadContext context_;
            Platform::ProjectInventoryLimits policy_;
        };
    }  // namespace

    /** @copydoc CreateProjectAssetRegistrySource */
    Result<std::shared_ptr<const Assets::IAssetRegistrySource>> CreateProjectAssetRegistrySource(
        std::shared_ptr<const Platform::IProjectReadFiles> files, Platform::ProjectReadContext context,
        const Platform::ProjectInventoryLimits &policy) {
        using Return = Result<std::shared_ptr<const Assets::IAssetRegistrySource>>;
        if (Platform::ProjectInventoryLimits hard; !files || context.projectIdentity.empty() || context.projectIdentity.size() > 256 ||
                                                   context.projectGeneration == 0 || !IsValidUtf8ScalarSequence(context.projectIdentity) ||
                                                   policy.maximumEntries == 0 || policy.maximumEntries > hard.maximumEntries ||
                                                   policy.maximumManifestBytes == 0 ||
                                                   policy.maximumManifestBytes > hard.maximumManifestBytes ||
                                                   policy.maximumDuration.count() <= 0 || policy.maximumDuration > hard.maximumDuration)
            return Return::Failure(MakeError(Platform::ProjectReadErrors::Invalid));
        try {
            return Return::Success(std::make_shared<ProjectAssetSource>(std::move(files), std::move(context), policy));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
        }
    }
}  // namespace Horo::Application
