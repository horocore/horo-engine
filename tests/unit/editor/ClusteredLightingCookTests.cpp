#include "ClusteredLightingCook.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Editor::Offline;

    struct OutputRoot {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("horo clustered producer " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        OutputRoot() {
            std::filesystem::create_directory(path);
        }

        ~OutputRoot() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };
}  // namespace

TEST_CASE("Clustered cook publication preserves existing output and foreign candidate directories", "[editor][shader-cook][offline]") {
    OutputRoot root;
    const auto output = root.path / "existing";
    std::filesystem::create_directory(output);
    {
        std::ofstream marker(output / "marker");
        marker << "unchanged";
    }
    const auto result = PublishClusteredLightingCook(output, {}, {});
    Tests::RequireActionableError(result, Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    CHECK(std::filesystem::file_size(output / "marker") == 9);
    CHECK(std::distance(std::filesystem::directory_iterator(root.path), std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Clustered cook failure abandons only its claimed candidate before family publication", "[editor][shader-cook][offline]") {
    OutputRoot root;
    const auto foreign = root.path / ".clustered-lighting-package-0";
    std::filesystem::create_directory(foreign);
    {
        std::ofstream marker(foreign / "marker");
        marker << "foreign";
    }
    const auto output = root.path / "new-package";
    const auto result = PublishClusteredLightingCook(output, {}, {});
    Tests::RequireActionableError(result, Render::ShaderCompilerPipelineErrors::ToolOutputInvalid);
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK_FALSE(std::filesystem::exists(root.path / ".clustered-lighting-package-1"));
    CHECK(std::filesystem::file_size(foreign / "marker") == 7);
    CHECK(std::distance(std::filesystem::directory_iterator(root.path), std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Clustered cook rejects missing parents and symlink destinations without state changes", "[editor][shader-cook][offline]") {
    OutputRoot root;
    SECTION("missing parent") {
        const auto missing = root.path / "missing";
        Tests::RequireActionableError(PublishClusteredLightingCook(missing / "out", {}, {}),
                                      Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        CHECK_FALSE(std::filesystem::exists(missing));
    }
    SECTION("dangling destination symlink") {
        const auto link = root.path / "link";
        std::filesystem::create_symlink(root.path / "absent", link);
        Tests::RequireActionableError(PublishClusteredLightingCook(link, {}, {}), Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));
    }
}

TEST_CASE("Clustered cook resolves all actual production sources beneath repository assets", "[editor][shader-cook][offline]") {
    OutputRoot root;
    constexpr std::array<std::string_view, 4> expected{"assets/shaders/editor/viewport_clustered_lit.hlsl",
                                                       "assets/shaders/editor/viewport_grid.hlsl",
                                                       "assets/shaders/editor/viewport_shadow.hlsl",
                                                       "assets/shaders/runtime/light_culling.hlsl"};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto resolved = ClusteredLightingCookSourcePath(root.path, index);
        REQUIRE(resolved.HasValue());
        CHECK(resolved.Value() == root.path / expected[index]);
        const auto canonical = root.path / expected[index];
        std::filesystem::create_directories(canonical.parent_path());
        {
            std::ofstream file(canonical);
            file << "source path fixture";
        }
        const auto input = Application::Internal::ReadShaderCookInput(resolved.Value(), 1024);
        REQUIRE(input.HasValue());
        CHECK(input.Value().size() == 19);
    }
    Tests::RequireActionableError(ClusteredLightingCookSourcePath(root.path, 4), Render::ShaderCompilerPipelineErrors::InvalidRequest);
    Tests::RequireActionableError(ClusteredLightingCookSourcePath("relative", 0), Render::ShaderCompilerPipelineErrors::InvalidRequest);
}

TEST_CASE("Clustered cook rejects private candidate output names before creating any package state", "[editor][shader-cook][offline]") {
    OutputRoot root;
    for (const auto name : {".clustered-lighting-package-0", ".clustered-lighting-package-1023", ".clustered-lighting-package-other",
                            ".CLUSTERED-LIGHTING-PACKAGE-0", ".ClUsTeReD-LiGhTiNg-PaCkAgE-0"}) {
        const auto destination = root.path / name;
        Tests::RequireActionableError(ValidateClusteredLightingCookDestination(destination),
                                      Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        Tests::RequireActionableError(PublishClusteredLightingCook(destination, {}, {}),
                                      Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        CHECK(std::filesystem::is_empty(root.path));
    }
    CHECK(ValidateClusteredLightingCookDestination(root.path / "clustered-lighting").HasValue());
}

TEST_CASE("Clustered cook requires existing scratch before creating final output or scratch state", "[editor][shader-cook][offline]") {
    OutputRoot root;
    const auto output = root.path / "output";
    SECTION("scratch equals output") {
        Tests::RequireActionableError(ValidateClusteredLightingCookPaths(output, output),
                                      Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        Application::Internal::AuthenticatedMetalCookInputs inputs;
        inputs.scratchRoot = output;
        Tests::RequireActionableError(CookEditorClusteredLighting(root.path, output, inputs),
                                      Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    }
    SECTION("scratch beneath output") {
        const auto scratch = output / "scratch";
        Tests::RequireActionableError(ValidateClusteredLightingCookPaths(output, scratch),
                                      Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        CHECK_FALSE(std::filesystem::exists(scratch));
    }
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::is_empty(root.path));
}

TEST_CASE("Clustered cook rejects output in the actual existing scratch tree including aliases", "[editor][shader-cook][offline]") {
    OutputRoot root;
    const auto scratch = root.path / "scratch";
    const auto nested = scratch / "nested";
    std::filesystem::create_directories(nested);
    auto suppliedScratch = scratch;
    auto output = nested / "output";
    SECTION("actual nested tree") {}
    SECTION("symlink output ancestor") {
        const auto alias = root.path / "output-alias";
        std::filesystem::create_directory_symlink(scratch, alias);
        output = alias / "nested/output";
    }
    SECTION("symlink scratch identity") {
        suppliedScratch = root.path / "scratch-alias";
        std::filesystem::create_directory_symlink(scratch, suppliedScratch);
    }
    REQUIRE(ValidateClusteredLightingCookDestination(output).HasValue());
    Tests::RequireActionableError(ValidateClusteredLightingCookPaths(output, suppliedScratch),
                                  Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::is_empty(nested));
}

TEST_CASE("Clustered cook accepts disjoint scratch and output paths with similar names", "[editor][shader-cook][offline]") {
    OutputRoot root;
    const auto scratch = root.path / "scratch";
    std::filesystem::create_directory(scratch);
    const auto output = root.path / "scratch-output";
    CHECK(ValidateClusteredLightingCookPaths(output, scratch).HasValue());
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::is_empty(scratch));
}
