#include "Horo/Editor/NativeAssetRegistrySource.h"
#include "Horo/Editor/WorkspaceReadSources.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "OwnedTestDirectory.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor {
    TEST_CASE("Workspace read composition requires an explicit complete host factory", "[editor][world_browser][composition]") {
        Horo::Tests::OwnedTestDirectory directory{"workspace sources"};
        WorkspaceReadSourceFactory factory;
        const auto missing = CaptureWorkspaceReadSources(nullptr, directory.Path());
        const auto empty = CaptureWorkspaceReadSources(&factory, directory.Path());
        REQUIRE(missing.HasError());
        REQUIRE(empty.HasError());
        CHECK(missing.ErrorValue().code.Value() == Platform::ProjectReadErrors::Unavailable.code.Value());
        CHECK(empty.ErrorValue().code.Value() == Platform::ProjectReadErrors::Unavailable.code.Value());
    }

    TEST_CASE("Workspace read composition rejects relative roots before invoking host code", "[editor][world_browser][composition]") {
        bool invoked = false;
        WorkspaceReadSourceFactory factory{[&](const std::filesystem::path &) {
            invoked = true;
            return Result<WorkspaceReadSources>::Success({});
        }};
        const auto captured = CaptureWorkspaceReadSources(&factory, "relative/project");
        REQUIRE(captured.HasError());
        CHECK(captured.ErrorValue().code.Value() == Platform::ProjectReadErrors::Invalid.code.Value());
        CHECK_FALSE(invoked);
    }

    TEST_CASE("Workspace read composition retains original host failures", "[editor][world_browser][composition]") {
        Horo::Tests::OwnedTestDirectory directory{"workspace sources"};
        WorkspaceReadSourceFactory factory{[](const std::filesystem::path &) {
            return Result<WorkspaceReadSources>::Failure(MakeError(Platform::ProjectReadErrors::Stale));
        }};
        const auto captured = CaptureWorkspaceReadSources(&factory, directory.Path());
        REQUIRE(captured.HasError());
        CHECK(captured.ErrorValue().code.Value() == Platform::ProjectReadErrors::Stale.code.Value());
    }

    TEST_CASE("Workspace read composition admits a complete retained pair and rejects partial pairs",
              "[editor][world_browser][composition]") {
        Horo::Tests::OwnedTestDirectory directory{"workspace sources"};
        const auto owner = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000009").Value();
        const auto registry = CreateNativeAssetRegistrySource(directory.Path(), owner.ToString(), 1);
        const auto files = Platform::CreateNativeProjectReadFiles(directory.Path(), owner.ToString(), 1);
        REQUIRE(registry.HasValue());
        REQUIRE(files.HasValue());
        WorkspaceReadSources sources{owner, registry.Value(), files.Value()};
        bool malformed = true;
        SECTION("missing owner") {
            sources.owner = {};
        }
        SECTION("missing registry") {
            sources.registry.reset();
        }
        SECTION("missing world reader") {
            sources.world.reset();
        }
        SECTION("complete pair") {
            malformed = false;
        }
        WorkspaceReadSourceFactory factory{[sources](const std::filesystem::path &) {
            return Result<WorkspaceReadSources>::Success(sources);
        }};
        const auto captured = CaptureWorkspaceReadSources(&factory, directory.Path());
        if (malformed) {
            REQUIRE(captured.HasError());
            CHECK(captured.ErrorValue().code.Value() == Platform::ProjectReadErrors::Invalid.code.Value());
        } else {
            REQUIRE(captured.HasValue());
            CHECK(captured.Value().owner == owner);
            CHECK(captured.Value().registry == registry.Value());
            CHECK(captured.Value().world == files.Value());
        }
    }
}  // namespace Horo::Editor
