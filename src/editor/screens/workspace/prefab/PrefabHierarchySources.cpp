#include "editor/screens/workspace/prefab/PrefabHierarchySources.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumSources = 256;
        constexpr std::size_t MaximumCaptureBytes = 64U * 1024U * 1024U;

        /** @brief Adds exact dependency identities once, preserving a bounded sorted capture selection. */
        Result<void> Include(std::vector<Assets::AssetId> &selection, const Assets::AssetId asset) {
            const auto place = std::ranges::lower_bound(selection, asset);
            if (place != selection.end() && *place == asset)
                return Result<void>::Success();
            if (selection.size() == MaximumSources)
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ObjectCountExceeded));
            selection.insert(place, asset);
            return Result<void>::Success();
        }

        /** @brief Parses a captured canonical source pair and discovers only typed nesting/inheritance edges. */
        Result<Prefab::PrefabDependencySource> ParseSource(const Assets::AssetCookPinnedSource &source,
                                                           const Prefab::PrefabLimitProfile &limits,
                                                           const Application::HoroVersion &version,
                                                           std::vector<Assets::AssetId> &selection) {
            if (source.record.type.Value() != "core.prefab")
                return Result<Prefab::PrefabDependencySource>::Failure(MakeError(Prefab::PrefabErrors::DependencyTypeMismatch));
            const std::string_view bytes{reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()};
            auto document = Prefab::PrefabDocument::Parse(bytes, limits, {.expectedProjectVersion = version});
            if (document.HasError())
                return Result<Prefab::PrefabDependencySource>::Failure(document.ErrorValue());
            if (document.Value().Data().assetId != source.record.id)
                return Result<Prefab::PrefabDependencySource>::Failure(MakeError(Prefab::PrefabErrors::ReferenceInvalid));
            if (const auto &composition = document.Value().Data().composition; composition) {
                if (composition->variantParent) {
                    if (auto added = Include(selection, composition->variantParent->Asset()); added.HasError())
                        return Result<Prefab::PrefabDependencySource>::Failure(added.ErrorValue());
                }
                for (const auto &nested : composition->nestedPlacements) {
                    if (auto added = Include(selection, nested.sourcePrefab.Asset()); added.HasError())
                        return Result<Prefab::PrefabDependencySource>::Failure(added.ErrorValue());
                }
            }
            auto canonical = document.Value().SerializeCanonical();
            if (canonical.HasError())
                return Result<Prefab::PrefabDependencySource>::Failure(canonical.ErrorValue());
            const auto digest = ComputeSha256(std::as_bytes(std::span(canonical.Value().data(), canonical.Value().size())));
            const auto sourceVersion = document.Value().Data().projectVersion;
            return Result<Prefab::PrefabDependencySource>::Success({std::move(document).Value(), {sourceVersion, digest}});
        }
    }  // namespace

    /** @copydoc CapturePrefabHierarchySources */
    Result<PrefabHierarchySources> CapturePrefabHierarchySources(
        const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry, const std::span<const Assets::AssetId> roots,
        const Prefab::PrefabLimitProfile &limits, const Application::HoroVersion &version, const CancellationToken &cancellation) {
        try {
            std::vector<Assets::AssetId> selection;
            for (const auto asset : roots) {
                if (auto added = Include(selection, asset); added.HasError())
                    return Result<PrefabHierarchySources>::Failure(added.ErrorValue());
            }
            const Assets::AssetCookLimits captureLimits{.maximumSourceBytes = Prefab::PrefabHardLimits::SourceDocumentBytes,
                                                        .maximumAssets = MaximumSources};
            for (;;) {
                auto inputs = Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, selection, captureLimits, MaximumCaptureBytes,
                                                                              cancellation);
                if (inputs.HasError())
                    return Result<PrefabHierarchySources>::Failure(inputs.ErrorValue());
                std::vector<Prefab::PrefabDependencySource> sources;
                sources.reserve(selection.size());
                const auto capturedCount = selection.size();
                for (const auto &source : inputs.Value().Sources()) {
                    auto parsed = ParseSource(source, limits, version, selection);
                    if (parsed.HasError())
                        return Result<PrefabHierarchySources>::Failure(parsed.ErrorValue());
                    sources.push_back(std::move(parsed).Value());
                }
                if (selection.size() != capturedCount)
                    continue;
                auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(registry, std::move(sources), limits);
                if (resolver.HasError())
                    return Result<PrefabHierarchySources>::Failure(resolver.ErrorValue());
                if (auto current = inputs.Value().VerifyUnchanged(cancellation); current.HasError())
                    return Result<PrefabHierarchySources>::Failure(current.ErrorValue());
                return Result<PrefabHierarchySources>::Success({std::move(resolver).Value(), std::move(inputs).Value()});
            }
        } catch (const std::bad_alloc &) {
            return Result<PrefabHierarchySources>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Editor
