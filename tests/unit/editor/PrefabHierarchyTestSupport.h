#pragma once

#include "SceneDocumentTestSupport.h"
#include "editor/screens/workspace/prefab/PrefabHierarchySources.h"

#include <chrono>
#include <fstream>

namespace Horo::Editor::PrefabHierarchyTestSupport {
    using namespace SceneDocumentTestSupport;

    /** @brief Owns only a newly created test directory, after all document and filesystem handles have retired. */
    struct ProjectDirectory final {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("horo-prefab-hierarchy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        ProjectDirectory() {
            REQUIRE(std::filesystem::create_directory(path));
        }

        ~ProjectDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    /** @brief Writes actual canonical source/sidecar pairs and retains the cooperative project authority. */
    struct Project final {
        ProjectDirectory directory;
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;
        Assets::AssetId asset{PrefabAsset().Asset()};
        Assets::AssetRecord record{ScenePrefabRecord(asset, "hierarchy")};
        std::filesystem::path source{directory.path / record.sourcePath.String()};
        Prefab::PrefabInstanceId instance{Prefab::PrefabInstanceId::Create(1).Value()};

        Project() {
            std::filesystem::create_directories(directory.path / ".horo");
            std::filesystem::create_directories(source.parent_path());
            WriteSource("Source root");
            std::ofstream(source.string() + ".horo")
                << "{\"schemaVersion\":1,\"assetId\":\"" << asset.ToString() << "\",\"assetType\":\"core.prefab\"}";
            REQUIRE(registry.Publish({record}).status == Assets::AssetRegistryBuildStatus::Complete);
        }

        void WriteSource(const std::string &name) const {
            auto document =
                ScenePrefabDocument(asset,
                                    {{.localId = Prefab::LocalObjectId{0}, .name = name},
                                     {.localId = Prefab::LocalObjectId{1}, .parentLocalId = Prefab::LocalObjectId{0}, .name = "Member"}});
            auto encoded = document.SerializeCanonical();
            REQUIRE(encoded.HasValue());
            std::ofstream(source, std::ios::binary | std::ios::trunc) << encoded.Value();
        }

        Result<PreparedPrefabHierarchySources> Capture(const Assets::AssetRegistrySnapshot &publication,
                                                       const std::span<const Assets::AssetId> roots,
                                                       const Prefab::PrefabLimitProfile &limits, const CancellationToken &cancellation) {
            auto lease = mutations.TryAcquire({directory.path, ProjectMutationOwner::Asset, "test.prefab.capture"});
            if (lease.HasError())
                return Result<PreparedPrefabHierarchySources>::Failure(lease.ErrorValue());
            auto capture = CapturePrefabHierarchySources(directory.path, publication, roots, limits, ScenePrefabVersion(), cancellation);
            if (capture.HasError())
                return Result<PreparedPrefabHierarchySources>::Failure(capture.ErrorValue());
            return Result<PreparedPrefabHierarchySources>::Success({std::move(capture).Value(), std::move(lease).Value()});
        }

        PrefabHierarchySourceCapture Loader() {
            return [this](const auto &publication, const auto roots, const auto &limits, const auto &cancellation) {
                return Capture(publication, roots, limits, cancellation);
            };
        }

        void WriteScene() const {
            std::filesystem::create_directories(directory.path / "assets/scenes");
            std::ofstream(directory.path / ".horo/project.json") << R"({"settings":{"defaultScene":"assets/scenes/main.horo"}})";
            const std::vector<SceneObjectSnapshot> objects{{.id = {11}, .name = "Authored parent"}};
            const std::vector<ScenePrefabInstance> placements{
                {.instanceId = instance, .sourcePrefab = PrefabAsset(), .parent = SceneObjectId{11}}};
            std::ofstream(directory.path / "assets/scenes/main.horo") << SceneSource::EncodeSceneSource({objects, placements});
        }
    };
}  // namespace Horo::Editor::PrefabHierarchyTestSupport
