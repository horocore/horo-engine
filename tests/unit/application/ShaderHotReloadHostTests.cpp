#include "EditorViewportShaderRecipe.h"
#include "ShaderHotReloadHost.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Application::Internal;

    struct SourceFixture final {
        std::filesystem::path root;
        ShaderHotReloadSourceRecipe recipe;
        ShaderHotReloadFence fence{1, 1, 1, 1, 1};

        SourceFixture() {
            root = std::filesystem::temp_directory_path() /
                   ("horo shader capture " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root / "shaders");
            std::ofstream(root / "shaders/main.hlsl") << "immutable source";
            std::ofstream(root / "shaders/include.hlsl") << "dependency";
            Render::ShaderTargetRequirement target{Render::ShaderTargetBackend::OpenGL, Render::ShaderPayloadFormat::Glsl410, 1, 1, 8, 0};
            auto manifest = Editor::MakeEditorViewportShaderManifest(Editor::EditorViewportShaderProgram::Grid, target);
            REQUIRE(manifest.HasValue());
            ShaderBuildRequest blueprint;
            blueprint.compilation.manifest = std::move(manifest).Value();
            blueprint.compilation.targets = {{.requirement = target}};
            blueprint.compilation.reflection = Render::ShaderCompilerReflectionRequest{{},
                                                                                       Editor::EditorViewportShaderReflectionNames(
                                                                                           Editor::EditorViewportShaderProgram::Grid)};
            recipe.root = root;
            recipe.programs = {{{1}, std::move(blueprint), "shaders/main.hlsl", {{"include.hlsl", "shaders/include.hlsl"}}}};
        }

        ~SourceFixture() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    };
}  // namespace

TEST_CASE("Explicit shader capture owns one complete immutable source closure", "[application][shader-reload][source]") {
    SourceFixture fixture;
    auto captured = CaptureShaderHotReloadSources(fixture.recipe, fixture.fence);
    REQUIRE(captured.HasValue());
    std::ofstream(fixture.root / "shaders/main.hlsl") << "changed";
    const auto &program = captured.Value().programs.front();
    CHECK(std::string(program.build.compilation.source.begin(), program.build.compilation.source.end()) == "immutable source");
    CHECK(program.build.compilation.dependencies.size() == 1);
    CHECK(program.build.sources.size() == 2);
    CHECK(program.build.compilation.dependencies[0].digest ==
          ComputeSha256(std::as_bytes(std::span(program.build.compilation.dependencies[0].content))));
    CHECK(captured.Value().fence == fixture.fence);
}

TEST_CASE("Source capture rejects aggregate overflow before admitting any family", "[application][shader-reload][source]") {
    SourceFixture fixture;
    fixture.recipe.maximumCapturedBytes = 20;
    auto captured = CaptureShaderHotReloadSources(fixture.recipe, fixture.fence);
    REQUIRE(captured.HasError());
    CHECK(captured.ErrorValue().domain.Value() == "horo.application.shader_reload");
    CHECK(captured.ErrorValue().code.Value() == "application.shader_reload.source_capacity");
}

TEST_CASE("Source closure rejects unbounded metadata before source reads", "[application][shader-reload][source]") {
    SourceFixture fixture;
    fixture.recipe.root = fixture.root / "missing";
    fixture.recipe.programs[0].blueprint.compilation.targets[0].platformTriple.assign(257, 'x');
    auto captured = CaptureShaderHotReloadSources(fixture.recipe, fixture.fence);
    REQUIRE(captured.HasError());
    CHECK(captured.ErrorValue().domain.Value() == "horo.application.shader_reload");
    CHECK(captured.ErrorValue().code.Value() == "application.shader_reload.source_capacity");
}

TEST_CASE("Source closure rejects traversal and missing declared dependency", "[application][shader-reload][source]") {
    SourceFixture fixture;
    fixture.recipe.programs[0].dependencies[0].relativePath = "../outside.hlsl";
    CHECK(CaptureShaderHotReloadSources(fixture.recipe, fixture.fence).HasError());
    fixture.recipe.programs[0].dependencies[0].relativePath = "missing.hlsl";
    CHECK(CaptureShaderHotReloadSources(fixture.recipe, fixture.fence).HasError());
}

TEST_CASE("Source capture cooperatively cancels before reading", "[application][shader-reload][source]") {
    SourceFixture fixture;
    CancellationSource cancel;
    cancel.RequestCancellation();
    auto captured = CaptureShaderHotReloadSources(fixture.recipe, fixture.fence, cancel.Token());
    REQUIRE(captured.HasError());
    CHECK(captured.ErrorValue().domain.Value() == "horo.application.shader_reload");
    CHECK(captured.ErrorValue().code.Value() == "application.shader_reload.source_cancelled");
}

TEST_CASE("Viewport developer configuration preserves complete selected family and finite compiler budgets",
          "[application][shader-reload][host]") {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Application::Internal;
    Render::ShaderCompilerToolIdentity reflection{Render::ShaderCompilerTool::SpirvCross, "qualified-release", {}};
    Render::ShaderCompilerTargetDescriptor target{.requirement = {Render::ShaderTargetBackend::OpenGL, Render::ShaderPayloadFormat::Glsl410,
                                                                  1, 1, 8, 0},
                                                  .platformTriple = "qualified-host",
                                                  .tools = {reflection}};
    auto configured = MakeEditorViewportHotReloadConfiguration(std::filesystem::temp_directory_path(), target, reflection, {1, 1, 1, 1, 0});
    REQUIRE(configured.HasValue());
    REQUIRE(configured.Value().sources.programs.size() == 3);
    const auto &programs = configured.Value().sources.programs;
    CHECK(programs[0].id.value == 1);
    CHECK(programs[1].id.value == 2);
    CHECK(programs[2].id.value == 3);
    CHECK(programs[0].blueprint.compilation.manifest.parameters[4].arrayCount == 16);
    CHECK(programs[0].blueprint.compilation.source.empty());
    CHECK(programs[0].blueprint.limits.maximumPayloadBytes == 8U * 1024U * 1024U);
    target.tools.clear();
    CHECK(MakeEditorViewportHotReloadConfiguration(std::filesystem::temp_directory_path(), target, reflection, {1, 1, 1, 1, 0}).HasError());
}

TEST_CASE("Source closure rejects a physical symlink outside its explicit authority", "[application][shader-reload][source]") {
    SourceFixture fixture;
    std::error_code error;
    std::filesystem::create_symlink(fixture.root.parent_path(), fixture.root / "escape", error);
    if (error) {
        SUCCEED("Host does not grant symlink creation");
        return;
    }
    fixture.recipe.programs[0].relativeSource = "escape/outside.hlsl";
    CHECK(CaptureShaderHotReloadSources(fixture.recipe, fixture.fence).HasError());
}

TEST_CASE("Vulkan viewport developer family retains Lit and canonical shared Shadow roles without Grid",
          "[application][shader-reload][host]") {
    using namespace Horo;
    using namespace Horo::Application::Internal;
    Render::ShaderCompilerToolIdentity reflection{Render::ShaderCompilerTool::SpirvCross, "qualified-release", {}};
    Render::ShaderCompilerTargetDescriptor target{.requirement = {Render::ShaderTargetBackend::Vulkan, Render::ShaderPayloadFormat::SpirV16,
                                                                  1, 1, 16, 128},
                                                  .platformTriple = "qualified-vulkan-host",
                                                  .tools = {reflection}};
    const auto configured =
        MakeEditorViewportHotReloadConfiguration(std::filesystem::temp_directory_path(), target, reflection, {1, 1, 1, 1, 0});
    REQUIRE(configured.HasValue());
    const auto &programs = configured.Value().sources.programs;
    REQUIRE(programs.size() == 2);
    CHECK(programs[0].id.value == 1);
    CHECK(programs[1].id.value == 3);
    CHECK(programs[0].relativeSource == "shaders/editor/viewport.hlsl");
    CHECK(programs[1].relativeSource == "shaders/editor/viewport_shadow.hlsl");
    CHECK(programs[1].blueprint.compilation.manifest.parameters.size() == 1);
    REQUIRE(programs[1].blueprint.compilation.reflection);
    CHECK(programs[1].blueprint.compilation.reflection->names[0].name == "type.ShadowParameters");
    for (const auto &program : programs) {
        CHECK(program.blueprint.limits.maximumPayloadBytes == 8U * 1024U * 1024U);
        CHECK(program.blueprint.limits.maximumDebugPayloadBytes == 8U * 1024U * 1024U);
        CHECK(program.blueprint.compilation.source.empty());
    }
}

namespace {
    struct UnchangedTarget final : Horo::Render::IShaderHotReloadTarget {
        std::size_t prepared{};

        Horo::Result<Horo::Render::ShaderHotReloadTargetState> CaptureState() const override {
            return Horo::Result<Horo::Render::ShaderHotReloadTargetState>::Success({Horo::Render::ShaderTargetBackend::OpenGL, 41, 1});
        }

        Horo::Result<std::unique_ptr<Horo::Render::IShaderHotReloadCandidate>> Prepare(
            std::span<const Horo::Render::ShaderHotReloadProgramView>, Horo::Render::ShaderHotReloadTargetState,
            const Horo::CancellationToken &) override {
            ++prepared;
            return Horo::Result<std::unique_ptr<Horo::Render::IShaderHotReloadCandidate>>::Failure(
                Horo::MakeError(Horo::Application::ShaderHotReloadErrors::TargetFailure));
        }

        Horo::Result<Horo::Render::ShaderHotReloadTargetState> Adopt(std::unique_ptr<Horo::Render::IShaderHotReloadCandidate>,
                                                                     Horo::Render::ShaderHotReloadTargetState) override {
            return Horo::Result<Horo::Render::ShaderHotReloadTargetState>::Failure(
                Horo::MakeError(Horo::Application::ShaderHotReloadErrors::TargetFailure));
        }
    };
}  // namespace

TEST_CASE("Developer host rejects nondevelopment capture policy before selecting any source", "[application][shader-reload][host]") {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Application::Internal;
    SourceFixture fixture;
    JobSystem jobs{{.workerCount = 0}};
    BuildOutputStore output{64};
    OperationStore operations{8, 16};
    ShaderBuildService compiler{{}, output};
    UnchangedTarget target;
    ShaderHotReloadHostConfiguration config{.publication = {.policy = ShaderHotReloadPolicy::Packaged, .context = fixture.fence},
                                            .sources = fixture.recipe};
    CHECK(ShaderHotReloadHost::Create(compiler, jobs, operations, output, target, config).HasError());
    CHECK(target.prepared == 0);
}

TEST_CASE("Developer host exposes reload and cancellation while preserving the last good target", "[application][shader-reload][host]") {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Application::Internal;
    SourceFixture fixture;
    JobSystem jobs{{.workerCount = 1, .ioWorkerCount = 1}};
    BuildOutputStore output{64};
    OperationStore operations{8, 16};
    ShaderBuildService compiler{{}, output};
    UnchangedTarget target;
    ShaderHotReloadHostConfiguration config{.publication = {.policy = ShaderHotReloadPolicy::Development, .context = fixture.fence},
                                            .sources = fixture.recipe};
    auto created = ShaderHotReloadHost::Create(compiler, jobs, operations, output, target, config);
    REQUIRE(created.HasValue());
    auto host = std::move(created).Value();
    REQUIRE(host->Reload().HasValue());
    CHECK(host->Cancel().HasValue());
    CHECK(host->Shutdown().HasValue());
    CHECK(host->Reload().HasError());
    CHECK(target.prepared == 0);
    CHECK(target.CaptureState().Value().liveGeneration == 1);
}
