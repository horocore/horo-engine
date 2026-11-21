#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Prefab/CookedPrefab.h"
#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <new>
#include <nlohmann/json.hpp>
#include <set>

namespace Horo::Application {
    namespace {
        constexpr std::size_t MaximumExtensionBytes = 16'384;
        using Json = nlohmann::ordered_json;

        /** @brief Resolves one exact required artifact against the sorted, verified generation inventory. */
        const Assets::AssetCookManifestEntry *ResolveDependency(std::span<const Assets::AssetCookManifestEntry> entries,
                                                                const Assets::AssetDependency &dependency) {
            const auto found = std::ranges::lower_bound(entries, dependency.id, {}, &Assets::AssetCookManifestEntry::assetId);
            return found != entries.end() && found->assetId == dependency.id && found->assetType == dependency.expectedType ? &*found
                                                                                                                            : nullptr;
        }

        /** @brief Verifies exact v6 child revisions in the same bounded release generation. */
        Result<void> VerifySceneChildren(const Runtime::RuntimeSceneDefinition &scene,
                                         const Assets::AssetCookGenerationContents &generation, const Assets::AssetCookLimits &limits,
                                         const CancellationToken &cancellation) {
            if (scene.ChildScenes().empty())
                return Result<void>::Success();
            const auto type = Assets::AssetTypeId::Parse("core.scene").Value();
            std::set<Assets::AssetId> verified;
            const std::span inventory{generation.entries};
            for (const auto &child : scene.ChildScenes()) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                if (!verified.insert(child.asset).second)
                    continue;
                const auto *entry = ResolveDependency(inventory, {child.asset, type});
                if (!entry)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                const auto index = static_cast<std::size_t>(entry - inventory.data());
                const auto envelope = Assets::DecodeCookedArtifact(generation.artifacts[index], limits);
                if (envelope.HasError())
                    return Result<void>::Failure(envelope.ErrorValue());
                const auto decoded = SceneCook::DecodeCookedChildScene(envelope.Value().payload, child);
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Checks typed prefab and Scene closure against the exact envelope digests packaged here. */
        Result<void> VerifyRuntimeArtifact(const Assets::AssetCookManifestEntry &entry, const std::span<const std::uint8_t> bytes,
                                           const Assets::AssetCookLimits &limits, const Assets::AssetCookGenerationContents &generation,
                                           const CancellationToken &cancellation) {
            const std::span inventory{generation.entries};
            const auto envelope = Assets::DecodeCookedArtifact(bytes, limits);
            if (envelope.HasError())
                return Result<void>::Failure(envelope.ErrorValue());
            if (entry.assetType.Value() == "core.prefab") {
                const auto profile = Prefab::PrefabLimitProfile::Create({}).Value();
                const auto prefab = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{envelope.Value().payload}), entry.assetId, profile);
                if (prefab.HasError())
                    return Result<void>::Failure(prefab.ErrorValue());
                for (const auto &dependency : prefab.Value().Data().dependencies) {
                    const auto *resolved = ResolveDependency(inventory, dependency.asset);
                    if (!resolved || resolved->artifactHash != dependency.artifactDigest)
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DependencyRevisionMismatch));
                }
            } else {
                const auto scene = SceneCook::DecodeCookedSceneWithEmbeddedRevision(envelope.Value().payload,
                                                                                    SceneCook::SceneDefinitionIdForAsset(entry.assetId));
                if (scene.HasError())
                    return Result<void>::Failure(scene.ErrorValue());
                for (const auto &dependency : scene.Value().AssetDependencies()) {
                    if (!ResolveDependency(inventory, dependency))
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                }
                return VerifySceneChildren(scene.Value(), generation, limits, cancellation);
            }
            return Result<void>::Success();
        }

        /** @brief Projects verified Scene and template digests in canonical manifest order. */
        Result<Json> ProjectReleaseEvidence(const Assets::AssetCookGenerationContents &generation, const Assets::AssetCookLimits &limits,
                                            std::span<const Assets::AssetId> roots, const CancellationToken &cancellation) {
            auto records = Json::array();
            std::size_t remainingRoots = roots.size();
            for (std::size_t index = 0; index < generation.entries.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<Json>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                const auto &entry = generation.entries[index];
                const auto &type = entry.assetType.Value();
                if (type == "core.prefab") {
                    if (!std::ranges::binary_search(roots, entry.assetId))
                        return Result<Json>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                    --remainingRoots;
                } else if (type != "core.scene") {
                    continue;
                }
                constexpr std::string_view EmptyRecord{R"({"id":"","type":"","envelope":""})"};
                constexpr std::size_t MinimumRecordBytes = EmptyRecord.size() + 36 + 71 + std::string_view{"core.scene"}.size();
                if (records.size() >= MaximumExtensionBytes / MinimumRecordBytes)
                    return Result<Json>::Failure(MakeError(Prefab::PrefabErrors::PayloadTooLarge));
                if (const auto checked = VerifyRuntimeArtifact(entry, generation.artifacts[index], limits, generation, cancellation);
                    checked.HasError())
                    return Result<Json>::Failure(checked.ErrorValue());
                records.push_back({{"id", entry.assetId.ToString()}, {"type", type}, {"envelope", FormatSha256(entry.artifactHash)}});
            }
            if (remainingRoots)
                return Result<Json>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
            return Result<Json>::Success(std::move(records));
        }
    }  // namespace

    /** @copydoc PreparePrefabReleaseContent */
    Result<PreparedPrefabReleaseContent> PreparePrefabReleaseContent(const DurableFileSystem &files,
                                                                     const Release::ReleaseCookedPayload &cooked,
                                                                     const AssetCookTargetId &target, const Assets::AssetChunkPlan &chunks,
                                                                     std::span<const Assets::AssetId> runtimePrefabRoots,
                                                                     const Assets::AssetArchiveLimits &limits,
                                                                     const CancellationToken &cancellation) {
        using Return = Result<PreparedPrefabReleaseContent>;
        if (cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
        Assets::AssetCookLimits cookLimits;
        cookLimits.maximumArtifactBytes = std::min(cookLimits.maximumArtifactBytes, limits.maximumAssetBytes);
        cookLimits.maximumAssets = std::min(cookLimits.maximumAssets, limits.maximumAssets);
        std::size_t expectedAssets{};
        for (const auto &chunk : chunks.Chunks()) {
            if (chunk.assets.size() > cookLimits.maximumAssets - expectedAssets)
                return Return::Failure(MakeError(Prefab::PrefabErrors::PayloadTooLarge));
            expectedAssets += chunk.assets.size();
        }
        if (!target.IsValid() || cooked.root.empty() || runtimePrefabRoots.size() > expectedAssets)
            return Return::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        try {
            std::vector<Assets::AssetId> roots(runtimePrefabRoots.begin(), runtimePrefabRoots.end());
            std::ranges::sort(roots);
            if (std::ranges::adjacent_find(roots) != roots.end() || !std::ranges::all_of(roots, &Assets::AssetId::IsValid))
                return Return::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            const Assets::AssetCookGeneration pinned{target, cooked.bytesDigest, cooked.root, expectedAssets};
            auto loaded = Assets::ReadCookGenerationContents(files, pinned, limits.maximumArchiveBytes, cookLimits);
            if (loaded.HasError())
                return Return::Failure(loaded.ErrorValue());
            auto &contents = loaded.Value();
            auto records = ProjectReleaseEvidence(contents, cookLimits, roots, cancellation);
            if (records.HasError())
                return Return::Failure(records.ErrorValue());
            std::vector<Assets::AssetArchiveInput> inputs;
            inputs.reserve(contents.entries.size());
            for (std::size_t index = 0; index < contents.entries.size(); ++index)
                inputs.emplace_back(contents.entries[index].assetId, std::move(contents.artifacts[index]));
            auto archive = Assets::BuildAssetArchive(chunks, target, inputs, limits);
            if (archive.HasError())
                return Return::Failure(archive.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
            const auto digest = ComputeSha256(std::as_bytes(std::span{archive.Value()}));
            auto json = Json{{"target", target.Value()},
                             {"cookManifest", FormatSha256(cooked.bytesDigest)},
                             {"archive", FormatSha256(digest)},
                             {"assets", std::move(records).Value()}}
                            .dump();
            if (json.size() > MaximumExtensionBytes)
                return Return::Failure(MakeError(Prefab::PrefabErrors::PayloadTooLarge));
            return Return::Success({std::move(archive).Value(), {"horo.prefab.content", 1, std::move(json)}});
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Prefab::PrefabErrors::PayloadTooLarge));
        }
    }
}  // namespace Horo::Application
