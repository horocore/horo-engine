#include "Horo/Application/ProjectAssetRegistrySource.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "OwnedTestDirectory.h"

#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    constexpr auto Sidecar = R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.scene"})";

    /** @brief Writes only inside an exclusively owned test project. */
    void Write(const std::filesystem::path &path, const std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream{path, std::ios::binary};
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(stream.good());
    }

    /** @brief Supplies one retained native root without creating an editor, project session or cache. */
    auto Files(const Tests::OwnedTestDirectory &directory) {
        auto files = Platform::CreateNativeProjectReadFiles(directory.Path(), "navigation-project", 7);
        REQUIRE(files.HasValue());
        return std::move(files).Value();
    }

    /** @brief Exact opening and finite complete-operation deadline used by inventory and sidecars. */
    Platform::ProjectReadContext Context() {
        return {"navigation-project", 7, {}, std::chrono::steady_clock::now() + std::chrono::seconds{10}, {}};
    }
}  // namespace

TEST_CASE("Headless registry capture uses the canonical sidecar owner without creating a derived index", "[project][inventory]") {
    Tests::OwnedTestDirectory directory{"navigation source ü"};
    Write(directory.Path() / "Assets/Scene.horoasset", "{}");
    Write(directory.Path() / "Assets/Scene.horoasset.horo", Sidecar);
    const auto source = CreateProjectAssetRegistrySource(Files(directory), Context());
    REQUIRE(source.HasValue());
    const auto candidate = Assets::PrepareAssetRegistryCandidate(*source.Value());
    REQUIRE(candidate.HasValue());
    CHECK(candidate.Value().status == Assets::AssetRegistryBuildStatus::Complete);
    REQUIRE(candidate.Value().records.size() == 1);
    CHECK(candidate.Value().records.front().sourcePath.String() == "Assets/Scene.horoasset");
    CHECK(candidate.Value().records.front().type.Value() == "core.scene");
    CHECK_FALSE(std::filesystem::exists(directory.Path() / ".horo"));
}

TEST_CASE("Headless registry authority cannot be renewed by later inventory or sidecar calls", "[project][inventory]") {
    Tests::OwnedTestDirectory directory{"navigation source authority"};
    Write(directory.Path() / "Assets/Scene.horoasset", "{}");
    Write(directory.Path() / "Assets/Scene.horoasset.horo", Sidecar);
    auto context = Context();
    CancellationSource cancel;
    context.cancellation = cancel.Token();
    auto revoked = std::make_shared<bool>();
    context.authorityStopped = [revoked] {
        return *revoked;
    };
    auto source = CreateProjectAssetRegistrySource(Files(directory), context);
    REQUIRE(source.HasValue());
    const auto captured = source.Value()->Capture();
    REQUIRE(captured.HasValue());
    REQUIRE(source.Value()->ReadSidecar("Assets/Scene.horoasset.horo", captured.Value().deadline).HasValue());
    SECTION("cancelled") {
        cancel.RequestCancellation();
    }
    SECTION("revoked") {
        *revoked = true;
    }
    CHECK(source.Value()->Capture().HasError());
    CHECK(source.Value()->ReadSidecar("Assets/Scene.horoasset.horo", std::chrono::steady_clock::time_point::max()).HasError());
}

TEST_CASE("Headless registry rejects expired contexts and another project opening", "[project][inventory]") {
    Tests::OwnedTestDirectory directory{"navigation source opening"};
    std::filesystem::create_directory(directory.Path() / "Assets");
    auto context = Context();
    SECTION("deadline") {
        context.deadline = {};
    }
    SECTION("generation") {
        ++context.projectGeneration;
    }
    SECTION("identity") {
        context.projectIdentity = "another-project";
    }
    const auto source = CreateProjectAssetRegistrySource(Files(directory), context);
    REQUIRE(source.HasValue());
    CHECK(source.Value()->Capture().HasError());
}

TEST_CASE("Headless registry retains the absolute caller deadline and shares inventory budgets", "[project][inventory]") {
    Tests::OwnedTestDirectory directory{"navigation source budget"};
    Write(directory.Path() / "Assets/Scene.horoasset", "{}");
    Write(directory.Path() / "Assets/Scene.horoasset.horo", Sidecar);
    const auto files = Files(directory);
    const auto context = Context();
    const auto source = CreateProjectAssetRegistrySource(files, context);
    REQUIRE(source.HasValue());
    const auto captured = source.Value()->Capture();
    REQUIRE(captured.HasValue());
    CHECK(captured.Value().deadline == context.deadline);
    Platform::ProjectInventoryLimits limits;
    limits.maximumEntries = 2;  // Root directory plus two descendants exceed the whole-capture budget.
    const auto bounded = CreateProjectAssetRegistrySource(files, context, limits);
    REQUIRE(bounded.HasValue());
    CHECK(bounded.Value()->Capture().HasError());
    limits.maximumEntries = 0;
    CHECK(CreateProjectAssetRegistrySource(files, context, limits).HasError());
    CHECK(CreateProjectAssetRegistrySource(nullptr, context).HasError());
}
