#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "VfxGpuCook.h"
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

TEST_CASE("VFX cook resolves all actual production sources beneath repository assets", "[editor][shader-cook][offline]") {
    OutputRoot root;
    constexpr std::array<std::string_view, 2> expected{"assets/shaders/runtime/vfx_simulation.hlsl",
                                                       "assets/shaders/runtime/vfx_gpu_draw.hlsl"};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto resolved = VfxGpuCookSourcePath(root.path, index);
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
    Tests::RequireActionableError(VfxGpuCookSourcePath(root.path, 2), Render::ShaderCompilerPipelineErrors::InvalidRequest);
    Tests::RequireActionableError(VfxGpuCookSourcePath("relative", 0), Render::ShaderCompilerPipelineErrors::InvalidRequest);
}

TEST_CASE("VFX cook reserves candidate names and rejects incomplete genuine families without output", "[vfx][shader-cook][offline]") {
    OutputRoot root;
    const auto scratch = root.path / "scratch";
    std::filesystem::create_directory(scratch);
    for (const auto name : {".vfx-gpu-package-0", ".VFX-GPU-PACKAGE-0", ".VfX-GpU-PaCkAgE-other"}) {
        Tests::RequireActionableError(ValidateVfxGpuCookPaths(root.path / name, scratch),
                                      Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        CHECK_FALSE(std::filesystem::exists(root.path / name));
    }
    const auto output = root.path / "output";
    Tests::RequireActionableError(PublishVfxGpuCook(output, {}, {}), Render::ShaderCompilerPipelineErrors::ToolOutputInvalid);
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::is_empty(scratch));
}

TEST_CASE("VFX cook requires existing scratch before creating final output or scratch state", "[editor][shader-cook][offline]") {
    OutputRoot root;
    const auto output = root.path / "output";
    SECTION("scratch equals output") {
        Tests::RequireActionableError(ValidateVfxGpuCookPaths(output, output), Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        Application::Internal::AuthenticatedMetalCookInputs inputs;
        inputs.scratchRoot = output;
        Tests::RequireActionableError(CookVfxGpuFamily(root.path, output, inputs), Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    }
    SECTION("scratch beneath output") {
        const auto scratch = output / "scratch";
        Tests::RequireActionableError(ValidateVfxGpuCookPaths(output, scratch), Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        CHECK_FALSE(std::filesystem::exists(scratch));
    }
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::is_empty(root.path));
}

TEST_CASE("VFX cook rejects output in the actual existing scratch tree including aliases", "[editor][shader-cook][offline]") {
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

    Tests::RequireActionableError(ValidateVfxGpuCookPaths(output, suppliedScratch), Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::is_empty(nested));
}

TEST_CASE("VFX cook accepts disjoint scratch and output paths with similar names", "[editor][shader-cook][offline]") {
    OutputRoot root;
    const auto scratch = root.path / "scratch";
    std::filesystem::create_directory(scratch);
    const auto output = root.path / "scratch-output";
    CHECK(ValidateVfxGpuCookPaths(output, scratch).HasValue());
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::is_empty(scratch));
}
