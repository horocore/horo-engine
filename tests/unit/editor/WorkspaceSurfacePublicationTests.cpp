#include "../../support/NativePublicationFiles.h"
#include "../../support/OwnedTestDirectory.h"
#include "Horo/Editor/WorkspaceLayoutPersistence.h"
#include "Horo/Platform/PlatformErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace Horo::Editor::Tests {
    namespace {
        /** @brief Fault injection around the real native commit, retaining the exact committed receipt. */
        class PublicationFiles final : public TestSupport::NativePublicationFiles {
        public:
            bool failBefore{};
            bool failAfter{};

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms permissions) override {
                return native.WritePrivateDurable(path, bytes, created, permissions);
            }

            Result<void> AtomicReplaceTracked(const std::filesystem::path &stage, const std::filesystem::path &destination,
                                              AtomicFileReplacementReceipt &receipt) override {
                if (failBefore)
                    return Result<void>::Failure(MakeError(PlatformErrors::ConfigurationWriteFailed));
                auto replaced = native.AtomicReplaceTracked(stage, destination, receipt);
                if (replaced.HasError())
                    return replaced;
                if (failAfter)
                    return Result<void>::Failure(MakeError(PlatformErrors::ConfigurationWriteFailed));
                return replaced;
            }
        };

        /** @brief Creates a valid workspace whose opaque zero bytes must survive provider absence. */
        WorkspaceLayout Layout() {
            WorkspaceLayout layout;
            layout.root = LayoutNode{PanelNode{"root", "horo.scene"}};
            layout.surfaces.push_back({"com.example.inspector", "com.example", "com.example.editor", true, false, true, {0, 255, 3}, {}});
            return layout;
        }
    }  // namespace

    TEST_CASE("Workspace publication failures preserve the previous document and distinguish uncertain commit",
              "[unit][editor][persistence][extension-recovery]") {
        Horo::Tests::OwnedTestDirectory directory{"workspace-recovery"};
        const auto path = directory.Path() / "çalışma alanı.json";
        PublicationFiles files;
        auto layout = Layout();
        AtomicFileReplacementReceipt initial;
        REQUIRE(WorkspaceLayoutPersistence::Save(files, initial, path, layout));
        REQUIRE(initial.WasCommitted());
        auto candidate = layout;
        candidate.surfaces.front().state = {6, 7};
        files.failBefore = true;
        AtomicFileReplacementReceipt before;
        REQUIRE_FALSE(WorkspaceLayoutPersistence::Save(files, before, path, candidate));
        CHECK_FALSE(before.WasCommitted());
        REQUIRE(WorkspaceLayoutPersistence::Load(path));
        CHECK(WorkspaceLayoutPersistence::Load(path)->surfaces == layout.surfaces);
        files.failBefore = false;
        files.failAfter = true;
        AtomicFileReplacementReceipt after;
        std::string error;
        REQUIRE_FALSE(WorkspaceLayoutPersistence::Save(files, after, path, candidate, &error));
        CHECK(after.WasCommitted());
        CHECK(error.find("uncertain") != std::string::npos);
        CHECK(WorkspaceLayoutPersistence::Load(path)->surfaces == candidate.surfaces);
    }

    TEST_CASE("Invalid workspace and foreign staging files cannot replace or corrupt durable provider intent",
              "[unit][editor][persistence][extension-recovery]") {
        Horo::Tests::OwnedTestDirectory directory{"workspace-staging"};
        const auto path = directory.Path() / "workspace.json";
        PublicationFiles files;
        auto layout = Layout();
        AtomicFileReplacementReceipt initial;
        REQUIRE(WorkspaceLayoutPersistence::Save(files, initial, path, layout));
        auto invalid = layout;
        invalid.surfaces.push_back(invalid.surfaces.front());
        AtomicFileReplacementReceipt rejected;
        CHECK_FALSE(WorkspaceLayoutPersistence::Save(files, rejected, path, invalid));
        auto stage = path;
        stage += ".pending";
        {
            std::ofstream sentinel(stage);
            sentinel << "foreign stage";
        }
        AtomicFileReplacementReceipt collision;
        CHECK_FALSE(WorkspaceLayoutPersistence::Save(files, collision, path, layout));
        CHECK_FALSE(collision.WasCommitted());
        std::ifstream input(stage);
        std::string text;
        std::getline(input, text);
        CHECK(text == "foreign stage");
        CHECK(WorkspaceLayoutPersistence::Load(path)->surfaces == layout.surfaces);
    }
}  // namespace Horo::Editor::Tests
