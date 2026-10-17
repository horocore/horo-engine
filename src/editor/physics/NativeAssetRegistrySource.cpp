#include "Horo/Editor/NativeAssetRegistrySource.h"

#include "Horo/Platform/NativeProjectReadFiles.h"

#include <algorithm>
#include <new>
#include <numeric>

namespace Horo::Editor {
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
        class NativeAssetSource final : public Assets::IAssetRegistrySource {
        public:
            /** @brief Retains explicit immutable read authority and per-opening inventory policy. */
            NativeAssetSource(std::shared_ptr<const Platform::IProjectReadFiles> files, std::string identity, std::uint64_t generation,
                              AssetRegistryInventoryPolicy policy)
                : files_(std::move(files)), identity_(std::move(identity)), generation_(generation), policy_(policy) {}

            /** @copydoc Assets::IAssetRegistrySource::Capture */
            Result<Assets::AssetRegistrySourceManifest> Capture() const override {
                try {
                    const auto context = Context();
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
                    Assets::AssetRegistrySourceManifest result{std::move(root), {}, context.deadline};
                    result.entries.reserve(entries.Value().entries.size());
                    for (auto &entry : entries.Value().entries)
                        result.entries.push_back({std::move(entry.path), AssetKind(entry.kind)});
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
                return {identity_, generation_, {}, std::chrono::steady_clock::now() + policy_.maximumDuration, {}};
            }

            /** @brief Preserves the explicitly selected independent inventory policy. */
            Platform::ProjectInventoryLimits Limits() const {
                return {policy_.maximumEntries, policy_.maximumManifestBytes, policy_.maximumDuration};
            }

            std::shared_ptr<const Platform::IProjectReadFiles> files_;
            std::string identity_;
            std::uint64_t generation_;
            AssetRegistryInventoryPolicy policy_;
        };
    }  // namespace

    /** @copydoc CreateNativeAssetRegistrySource */
    Result<std::shared_ptr<const Assets::IAssetRegistrySource>> CreateNativeAssetRegistrySource(
        const std::filesystem::path &root, std::string identity, const std::uint64_t generation,
        const AssetRegistryInventoryPolicy &policy) {
        using Return = Result<std::shared_ptr<const Assets::IAssetRegistrySource>>;
        const AssetRegistryInventoryPolicy hard;
        if (policy.maximumEntries == 0 || policy.maximumEntries > hard.maximumEntries || policy.maximumManifestBytes == 0 ||
            policy.maximumManifestBytes > hard.maximumManifestBytes || policy.maximumDuration.count() <= 0 ||
            policy.maximumDuration > hard.maximumDuration)
            return Return::Failure(MakeError(Platform::ProjectReadErrors::Invalid));
        try {
            auto files = Platform::CreateNativeProjectReadFiles(root, identity, generation);
            if (files.HasError())
                return Return::Failure(files.ErrorValue());
            return Return::Success(std::make_shared<NativeAssetSource>(std::move(files).Value(), std::move(identity), generation, policy));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
        }
    }
}  // namespace Horo::Editor
