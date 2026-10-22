#pragma once
/** @file PrefabCreationTestSupport.h
 * @brief Target-private native publication and canonical Scene/history fixture for create-from-selection regressions.
 */

#include "../unit/editor/PrefabHierarchyTestSupport.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Scene/SceneSource.h"
#include "NativePublicationFiles.h"
#include "editor/screens/workspace/prefab/PrefabCreation.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <new>
#include <optional>
#include <span>

namespace Horo::Editor::PrefabCreationTestSupport {
    using namespace PrefabHierarchyTestSupport;

    /** @brief Injects failures at real durable boundaries while keeping genuine native identity/lease semantics. */
    struct Files final : TestSupport::NativePublicationFiles {
        std::size_t writes{};
        std::size_t failAt{};
        bool failCleanup{};
        bool omitIdentity{};
        bool throwSecond{};
        bool throwInspectionAfterSecond{};
        bool failUncapturedSecond{};
        bool partialSecond{};
        std::function<void()> beforeSecond;
        std::function<void(const std::filesystem::path &)> beforeCleanup;

        Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                         const std::filesystem::perms mode) override {
            ++writes;
            if (writes == 2 && beforeSecond)
                beforeSecond();
            if (writes == 2 && throwSecond)
                throw std::bad_alloc{};
            if (writes == 2 && failUncapturedSecond) {
                const auto written = native.WritePrivateDurable(path, partialSecond ? bytes.first(bytes.size() / 2) : bytes, created, mode);
                if (written.HasError())
                    return written;
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            }
            if (writes == failAt)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            return native.WritePrivateDurable(path, bytes, created, mode);
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            if (beforeCleanup)
                beforeCleanup(path);
            return failCleanup ? Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority)) : native.RemoveDurable(path);
        }

        Result<std::optional<FileEntryInfo>> InspectEntry(const std::filesystem::path &path) const override {
            if (throwInspectionAfterSecond && writes >= 2)
                throw std::bad_alloc{};
            auto result = native.InspectEntry(path);
            if (result.HasError())
                return result;
            auto metadata = std::move(result).Value();
            if (omitIdentity && metadata)
                metadata->identity.reset();
            return Result<std::optional<FileEntryInfo>>::Success(std::move(metadata));
        }
    };

    /** @brief Owns one real project and its canonical Scene/history; no shadow authored model participates. */
    struct Creation final {
        ProjectDirectory directory;
        std::filesystem::path root{std::filesystem::canonical(directory.path)};
        Files files;
        ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentTransactions scene;
        Assets::AssetId asset{PrefabAsset(7).Asset()};
        std::filesystem::path destination{root / "assets/prefab ü 日本.prefab"};

        Creation() {
            REQUIRE(std::filesystem::create_directory(root / "assets"));
            const std::vector<SceneObjectSnapshot> objects{{.id = {10}, .name = "Outside"},
                                                           {.id = {20},
                                                            .parent = SceneObjectId{10},
                                                            .name = "Root",
                                                            .localTransform = {.translation = {3, 4, 5}}},
                                                           {.id = {30},
                                                            .parent = SceneObjectId{20},
                                                            .name = "Child",
                                                            .localTransform = {.translation = {1, 2, 3}},
                                                            .components = {.camera = Runtime::CameraComponent{}}}};
            REQUIRE(document.LoadSaved(objects).HasValue());
            auto bound = SceneDocumentTransactions::Bind(document, history, {PrefabAsset(8).Asset(), 1});
            REQUIRE(bound.HasValue());
            scene = std::move(bound).Value();
        }

        SceneTransactionContext Context() {
            const auto snapshot = scene.Snapshot();
            REQUIRE(snapshot.HasValue());
            return {snapshot.Value().session, snapshot.Value().revision, snapshot.Value().state, true};
        }

        /** @brief Submits the canonical request with an optional accepted Scene source fence. */
        PrefabCreationOutcome Run(const std::optional<SceneTransactionContext> &context = {},
                                  const std::optional<PrefabCreationSourceFence> &sourceFence = {}) {
            return CreatePrefabFromSelection(scene,
                                             {context.value_or(Context()),
                                              {20},
                                              root,
                                              destination,
                                              asset,
                                              ScenePrefabVersion(),
                                              registry.Snapshot(),
                                              nullptr,
                                              sourceFence},
                                             mutations, files);
        }

        void CheckUncommitted(const PrefabCreationOutcome &result) const {
            CHECK_FALSE(result.committed);
            REQUIRE(result.error);
            CHECK(document.Objects().size() == 3);
            CHECK(document.PrefabInstances().empty());
        }

        /** @brief Saves the canonical working Scene and returns the exact accepted disk fence used by publication. */
        PrefabCreationSourceFence SaveSceneFence() {
            const auto path = root / "assets/Scene.horo";
            const auto encoded = SceneSource::EncodeSceneSource({document.Objects(), document.PrefabInstances()});
            const auto bytes = std::as_bytes(std::span{encoded.data(), encoded.size()});
            REQUIRE(files.native.WriteDurable(path, bytes).HasValue());
            return {path, {true, encoded.size(), ComputeSha256(bytes)}};
        }

        std::string Read(const std::filesystem::path &path) {
            std::ifstream input(path, std::ios::binary);
            return {std::istreambuf_iterator<char>{input}, {}};
        }
    };

}  // namespace Horo::Editor::PrefabCreationTestSupport
