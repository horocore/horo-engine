#pragma once
#include "Horo/Editor/CollisionProjectSettingsSession.h"
#include "Horo/Scene/SceneSource.h"
#include "OwnedTestDirectory.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace Horo::Tests::CollisionSettings {
    template <typename Id> Id IdOf(const std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return Id::FromBytes(bytes);
    }

    inline Application::ProjectCollisionSettingsInput Input() {
        const auto layer = IdOf<Physics::CollisionLayerId>(1);
        const auto profile = IdOf<Physics::CollisionProfileId>(2);
        const auto spare = IdOf<Physics::CollisionProfileId>(3);
        const auto channel = IdOf<Physics::PhysicsQueryChannelId>(4);
        return {.projectId = "collision-project",
                .schema = {.defaultProfile = profile,
                           .layers = {{layer}},
                           .pairs = {{layer, layer, Physics::SimulationPairResponse::Block}},
                           .queryChannels = {{channel}},
                           .profiles = {{profile, layer, true, true, true, {{channel, Physics::CollisionQueryResponse::Block}}},
                                        {spare, layer, true, true, true, {{channel, Physics::CollisionQueryResponse::Overlap}}}}},
                .layers = {{layer, "World", "Shared layer"}},
                .profiles = {{profile, "Solid", {}}, {spare, "Sensor", {}}},
                .channels = {{channel, "Visibility", {}}}};
    }

    inline void Write(const std::filesystem::path &path, const std::string_view source) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(source.data(), static_cast<std::streamsize>(source.size()));
        output.close();
        REQUIRE(output.good());
    }

    inline Application::ProjectCollisionSettingsInput RemoveSpare(Application::ProjectCollisionSettingsInput input) {
        const auto id = IdOf<Physics::CollisionProfileId>(3);
        std::erase_if(input.schema.profiles, [id](const auto &v) {
            return v.id == id;
        });
        std::erase_if(input.profiles, [id](const auto &v) {
            return v.id == id;
        });
        return input;
    }

    class Project final {
    public:
        OwnedTestDirectory directory{"collision settings UTF8-图"};
        std::filesystem::path root{std::filesystem::canonical(directory.Path())};
        NativeDurableFileSystem files;
        Editor::ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;
        Editor::CollisionInventoryEvidence inventory;
        Editor::CollisionActivationEvidence activation{.revision = 1, .state = Editor::CollisionActivationState::NoPhysics};
        Editor::ProjectCollisionSettingsStore store{files, mutations, root, "collision-project"};

        explicit Project(Application::ProjectCollisionSettingsInput input = Input()) {
            std::filesystem::create_directories(root / ".horo");
            std::filesystem::create_directories(root / "Assets");
            auto document = Application::ProjectCollisionSettings::Create(std::move(input));
            REQUIRE(document.HasValue());
            REQUIRE(store.Save(std::nullopt, document.Value()).HasValue());
            Rebuild();
        }

        void Rebuild() {
            auto report = Assets::RebuildAssetRegistry(registry, root, Assets::AssetRegistryOpenMode::Edit);
            REQUIRE(report.HasValue());
            REQUIRE(report.Value().status == Assets::AssetRegistryBuildStatus::Complete);
            inventory = {report.Value().publishedRevision, report.Value().status};
        }

        std::shared_ptr<Editor::CollisionProjectSettingsSession> Open(Editor::SceneDocumentTransactions *scene = nullptr) {
            const Editor::CollisionProjectSettingsOwners
                owners{registry, files, mutations, inventory, scene, scene ? root / "Assets/Scene.horoasset" : std::filesystem::path{}, {}};
            auto result = Editor::CollisionProjectSettingsSession::Open(root, "collision-project", owners, activation);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        void Scene(const std::string_view bytes) {
            Write(root / "Assets/Scene.horoasset", bytes);
            Write(root / "Assets/Scene.horoasset.horo",
                  R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.scene"})");
            Rebuild();
        }
    };

    inline void Commit(Editor::CollisionProjectSettingsSession &session, Application::ProjectCollisionSettingsInput input) {
        auto preview = session.Preview(std::move(input));
        REQUIRE(preview.HasValue());
        REQUIRE(preview.Value().CanCommit());
        REQUIRE(session.Commit(preview.Value()).HasValue());
    }
}  // namespace Horo::Tests::CollisionSettings
