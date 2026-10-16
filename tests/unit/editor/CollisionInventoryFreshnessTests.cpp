#include "CollisionSettingsTestSupport.h"

namespace Horo::Tests::CollisionSettings {
    namespace {
        class DeniedDirectory final {
        public:
            explicit DeniedDirectory(std::filesystem::path path)
                : path_(std::move(path)), prior_(std::filesystem::status(path_).permissions()) {
                std::error_code ignored;
                std::filesystem::permissions(path_, std::filesystem::perms::none, ignored);
            }

            ~DeniedDirectory() {
                std::error_code ignored;
                std::filesystem::permissions(path_, prior_, ignored);
            }

        private:
            std::filesystem::path path_;
            std::filesystem::perms prior_;
        };

        void NewOwner(const std::filesystem::path &root, const std::filesystem::path &directory = "Assets") {
            Write(root / directory / "new.horoasset", "opaque");
            Write(root / directory / "new.horoasset.horo",
                  R"({"schemaVersion":1,"assetId":"40213243-5465-7687-98a9-bacbdcedfe0f","assetType":"custom.owner"})");
        }
    }  // namespace

    TEST_CASE("Collision physical inventory detects new external owners without a registry or index refresh",
              "[collision][inventory][freshness]") {
        Project project;
        auto session = project.Open();
        const auto snapshot = session->Snapshot().Value();
        auto review = session->Preview(RemoveSpare(snapshot->Input()));
        REQUIRE(review.HasValue());
        REQUIRE(review.Value().CanCommit());
        const auto published = project.registry.Snapshot();
        const auto indexPath = project.root / std::filesystem::path{ProjectLayout::AssetIndexPath};
        const auto before = Assets::CaptureAssetSource(project.root, indexPath, 1024 * 1024).Value().identity;
        NewOwner(project.root);
        CHECK(session->Commit(review.Value()).HasError());
        CHECK(session->Preview(RemoveSpare(snapshot->Input())).HasError());
        CHECK(session->Snapshot().Value() == snapshot);
        CHECK_FALSE(session->IsDirty());
        CHECK(project.registry.Snapshot().Revision() == published.Revision());
        CHECK(Assets::CaptureAssetSource(project.root, indexPath, 1024 * 1024).Value().identity == before);
    }

    TEST_CASE("Collision store rechecks actual source membership under project publication ownership", "[collision][inventory][store]") {
        Project project;
        const auto before = project.store.Load().Value();
        const Editor::CollisionPublicationSourceFence reviewed{project.registry.Snapshot(), {}};
        auto input = RemoveSpare(before.settings->Input());
        input.revision = 2;
        const auto candidate = Application::ProjectCollisionSettings::Create(input);
        REQUIRE(candidate.HasValue());
        NewOwner(project.root);
        CHECK(project.store.Save(before, candidate.Value(), reviewed).HasError());
        CHECK(project.store.Load().Value().identity == before.identity);
        CHECK(project.registry.Snapshot().Revision() == reviewed.inventory.Revision());
    }

    TEST_CASE("Collision reference scan never treats inaccessible or newly owned subtrees as complete empty inputs",
              "[collision][inventory][permissions]") {
        Project project;
        auto session = project.Open();
        NewOwner(project.root, "Assets/private");
        const auto path = project.root / "Assets/private";
        const DeniedDirectory denied{path};
        // Privileged/Windows hosts may still enumerate: changed membership must reject in either case, without a skip.
        auto result = session->Preview(RemoveSpare(session->Snapshot().Value()->Input()));
        CHECK(result.HasError());
        CHECK_FALSE(session->IsDirty());
    }
}  // namespace Horo::Tests::CollisionSettings
