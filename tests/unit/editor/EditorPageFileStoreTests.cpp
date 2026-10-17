#include "EditorPageTestSupport.h"
#include "Horo/Foundation/Platform.h"
#include "OwnedTestDirectory.h"
#include "editor/screens/EditorPageFileStore.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>

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

        /** @brief Publishes and locates the single real user document before a filesystem rejection probe. */
        std::filesystem::path CreateUserDocument(EditorPageFileStore &store, const Extensions::EditorSurfaceProviderIdentity &provider,
                                                 const std::filesystem::path &root) {
            REQUIRE(store.Write(provider, "page", SettingScope::User, 1, Snapshot()).HasValue());
            std::filesystem::path path;
            for (const auto &entry : std::filesystem::directory_iterator(root / "extensions")) {
                if (entry.path().extension() == ".json") {
                    REQUIRE(path.empty());
                    path = entry.path();
                }
            }
            REQUIRE_FALSE(path.empty());
            REQUIRE(std::filesystem::is_regular_file(path));
            REQUIRE(store.Read(provider, "page", SettingScope::User).HasValue());
            return path;
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
        const auto path = CreateUserDocument(store, provider, directory.Path());
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

    TEST_CASE("Page directory substitution preserves the original document bytes and directory contents", "[editor][EditorPage][Files]") {
        Horo::Tests::OwnedTestDirectory directory{"page-directory-substitution"};
        NativeDurableFileSystem files;
        EditorPageFileStore store{files, directory.Path()};
        const Extensions::EditorSurfaceProviderIdentity provider{"fixture.package", "fixture.module", 1};
        const auto path = CreateUserDocument(store, provider, directory.Path());
        std::ifstream source{path, std::ios::binary};
        REQUIRE(source.is_open());
        const std::string before{std::istreambuf_iterator<char>{source}, {}};
        source.close();
        REQUIRE_FALSE(before.empty());
        const auto original = directory.Path() / "original.json";
        std::filesystem::rename(path, original);
        REQUIRE(std::filesystem::create_directory(path));
        const auto sentinel = path / "retain.txt";
        {
            std::ofstream stream{sentinel};
            stream << "retained directory";
            stream.close();
            REQUIRE(stream.good());
        }
        CHECK(store.Read(provider, "page", SettingScope::User).HasError());
        CHECK(store.Write(provider, "page", SettingScope::User, 1, Snapshot()).HasError());
        std::ifstream afterInput{original, std::ios::binary};
        REQUIRE(afterInput.is_open());
        const std::string after{std::istreambuf_iterator<char>{afterInput}, {}};
        CHECK(after == before);
        CHECK(std::filesystem::is_directory(path));
        std::ifstream stream{sentinel};
        std::string retained;
        std::getline(stream, retained);
        CHECK(retained == "retained directory");
    }
}  // namespace Horo::Editor::Tests
