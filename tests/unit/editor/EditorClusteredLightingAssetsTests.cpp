#include "EditorClusteredLightingAssets.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    constexpr std::array<std::string_view, 4> Prefixes{"shaders/editor/viewport-clustered-metal", "shaders/editor/viewport-grid-metal",
                                                       "shaders/editor/viewport-shadow-metal", "shaders/runtime/light-culling-metal"};

    void WriteCandidate(const std::filesystem::path &root) {
        for (const auto prefix : Prefixes)
            for (const auto suffix : {".horoshdr", ".reflection.json"}) {
                const auto path = root / (std::string{prefix} + suffix);
                std::filesystem::create_directories(path.parent_path());
                std::ofstream output(path);
                output << "candidate presence only";
                REQUIRE(output.good());
            }
    }

    struct Bundle {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo clustered assets " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Bundle() {
            std::filesystem::create_directories(root / "shaders/editor");
        }

        ~Bundle() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path Payload() const {
            return root / "shaders/editor/viewport-clustered-metal.horoshdr";
        }

        std::filesystem::path Metadata() const {
            return root / "shaders/editor/viewport-clustered-metal.reflection.json";
        }
    };
}  // namespace

TEST_CASE("Metal clustered assets require exact bounded packaged pairs before cold preparation",
          "[editor][shader-cook][clustered-assets]") {
    Bundle bundle;
    Tests::RequireActionableError(LoadPackagedMetalClusteredLighting(bundle.root), ClusteredLightingAssetErrors::Unavailable);
    {
        std::ofstream output(bundle.Payload());
        output << "not a cooked shader";
    }
    Tests::RequireActionableError(LoadPackagedMetalClusteredLighting(bundle.root), ClusteredLightingAssetErrors::Unavailable);
    std::filesystem::resize_file(bundle.Payload(), 16U * 1024U * 1024U + 1);
    Tests::RequireActionableError(LoadPackagedMetalClusteredLighting(bundle.root), ClusteredLightingAssetErrors::Unavailable);
}

TEST_CASE("Metal clustered asset loading rejects symlink inputs and malformed immutable metadata",
          "[editor][shader-cook][clustered-assets]") {
    Bundle bundle;
    {
        std::ofstream output(bundle.Metadata());
        output << "{}";
    }
    std::filesystem::create_symlink(bundle.Metadata(), bundle.Payload());
    Tests::RequireActionableError(LoadPackagedMetalClusteredLighting(bundle.root), ClusteredLightingAssetErrors::Unavailable);
    std::filesystem::remove(bundle.Payload());
    {
        std::ofstream output(bundle.Payload());
        output << "scripted invalid data";
    }
    CHECK(LoadPackagedMetalClusteredLighting(bundle.root).HasError());
}

TEST_CASE("Metal clustered family borrows exact complete owned program roles", "[editor][shader-cook][clustered-assets]") {
    EditorClusteredLightingAssets assets;
    const auto views = assets.ProgramViews();
    REQUIRE(views.size() == 3);
    for (std::size_t index = 0; index < views.size(); ++index) {
        CHECK(views[index].id.value == index + 1);
        CHECK(views[index].manifest == &assets.manifests[index]);
        CHECK(views[index].compiled == &assets.programs[index]);
    }
    const auto target = EditorClusteredLightingMetalTarget();
    CHECK(target.backend == Render::ShaderTargetBackend::Metal);
    CHECK(target.payloadFormat == Render::ShaderPayloadFormat::MetalLibrary24);
    CHECK(target.supportsCompute);
    CHECK(target.supportsStorageResources);
}

TEST_CASE("Metal clustered input presence distinguishes absent complete and partial families", "[editor][shader-cook][clustered-assets]") {
    Bundle bundle;
    const auto absent = HasPackagedMetalClusteredLightingInputs(bundle.root);
    REQUIRE(absent.HasValue());
    CHECK_FALSE(absent.Value());
    WriteCandidate(bundle.root);
    const auto complete = HasPackagedMetalClusteredLightingInputs(bundle.root);
    REQUIRE(complete.HasValue());
    CHECK(complete.Value());
    CHECK(LoadPackagedMetalClusteredLighting(bundle.root).HasError());
    const auto index = GENERATE(0, 1, 2, 3);
    const auto suffix = GENERATE(".horoshdr", ".reflection.json");
    std::filesystem::remove(bundle.root / (std::string{Prefixes[index]} + suffix));
    Tests::RequireActionableError(HasPackagedMetalClusteredLightingInputs(bundle.root), ClusteredLightingAssetErrors::Unavailable);
}

TEST_CASE("Metal clustered presence rejects isolated late inputs and symlink families", "[editor][shader-cook][clustered-assets]") {
    Bundle bundle;
    SECTION("isolated kernel") {
        const auto path = bundle.root / "shaders/runtime/light-culling-metal.reflection.json";
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path);
        output << "partial";
    }
    SECTION("symlink leaf") {
        std::filesystem::create_symlink(bundle.Metadata(), bundle.Payload());
    }
    SECTION("symlink directory") {
        std::filesystem::create_directory_symlink(bundle.root / "shaders/editor", bundle.root / "shaders/runtime");
    }
    SECTION("non-directory component") {
        std::ofstream output(bundle.root / "shaders/runtime");
        output << "not a directory";
    }
    Tests::RequireActionableError(HasPackagedMetalClusteredLightingInputs(bundle.root), ClusteredLightingAssetErrors::Unavailable);
}
