#include "EditorPageTestSupport.h"
#include "Horo/Foundation/Platform.h"
#include "OwnedTestDirectory.h"
#include "editor/screens/EditorPageFileStore.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace Horo::Editor::Tests {
    namespace {
        /** @brief Creates the canonical typed snapshot used by the real durable file-store adapter. */
        ConfigurationSnapshot Snapshot() {
            ConfigurationSchema schema;
            REQUIRE(schema
                        .Register({SettingKey{"page.count"},
                                   SettingValueType::Integer,
                                   std::int64_t{9007199254740993LL},
                                   SettingScope::Project,
                                   ReloadPolicy::NextOperation,
                                   SettingSensitivity::Public,
                                   {}})
                        .HasValue());
            REQUIRE(schema.Seal().HasValue());
            auto resolved = ConfigurationResolver::Resolve(schema, {}, 1);
            REQUIRE(resolved.HasValue());
            return std::move(resolved).Value();
        }
    }  // namespace

    TEST_CASE("Actual page file store isolates scope and activation identity in spaced Unicode project paths",
              "[editor][EditorPage][Files]") {
        Horo::Tests::OwnedTestDirectory directory{"page-files"};
        const auto root = directory.Path() / "project space" / std::filesystem::path{u8"çalışma"};
        REQUIRE(std::filesystem::create_directories(root));
        NativeDurableFileSystem files;
        EditorPageFileStore store{files, directory.Path() / "user"};
        store.SetProject(root);
        Extensions::EditorSurfaceProviderIdentity provider{"fixture.package", "fixture.module", 1};
        auto missing = store.Read(provider, "page", SettingScope::Project);
        REQUIRE(missing.HasValue());
        CHECK_FALSE(missing.Value().has_value());
        const auto epoch = store.ContextRevision(SettingScope::Project);
        REQUIRE(store.Write(provider, "page", SettingScope::Project, epoch, Snapshot()).HasValue());
        provider.activationGeneration = 2;
        auto loaded = store.Read(provider, "page", SettingScope::Project);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value());
        CHECK(loaded.Value()->find("9007199254740993") != std::string::npos);
        auto workspace = store.Read(provider, "page", SettingScope::Workspace);
        REQUIRE(workspace.HasValue());
        CHECK_FALSE(workspace.Value());
        store.SetProject({});
        CHECK(store.Read(provider, "page", SettingScope::Project).HasError());
        store.SetProject(root);
        CHECK(store.Write(provider, "page", SettingScope::Project, epoch, Snapshot()).HasError());
    }

    TEST_CASE("Actual page file store rejects redirected and oversized documents without modifying them", "[editor][EditorPage][Files]") {
        Horo::Tests::OwnedTestDirectory directory{"page-file-errors"};
        NativeDurableFileSystem files;
        EditorPageFileStore store{files, directory.Path()};
        const Extensions::EditorSurfaceProviderIdentity provider{"fixture.package", "fixture.module", 1};
        REQUIRE(store.Write(provider, "page", SettingScope::User, 1, Snapshot()).HasValue());
        auto entries = std::filesystem::directory_iterator(directory.Path() / "extensions");
        REQUIRE(entries != std::filesystem::directory_iterator{});
        const auto path = entries->path();
        SECTION("oversized source remains unchanged") {
            {
                std::ofstream stream{path, std::ios::binary | std::ios::trunc};
                stream << std::string(128 * 1024 + 1, 'x');
            }
            CHECK(store.Read(provider, "page", SettingScope::User).HasError());
            CHECK(std::filesystem::file_size(path) == 128 * 1024 + 1);
        }
        SECTION("hard-link alias cannot be overwritten") {
            std::error_code error;
            std::filesystem::create_hard_link(path, directory.Path() / "alias.json", error);
            if (error) {
                SKIP("Filesystem does not support hard links: " << error.message());
            }
            CHECK(store.Read(provider, "page", SettingScope::User).HasError());
            CHECK(store.Write(provider, "page", SettingScope::User, 1, Snapshot()).HasError());
            CHECK(std::filesystem::hard_link_count(path) == 2);
        }
    }
}  // namespace Horo::Editor::Tests
