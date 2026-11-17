#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "support/ShaderCompilerToolchainFixture.h"

#include <array>

#ifndef HORO_VFX_GPU_REFLECTION_FIXTURE_PATH
#error "Supply the actual immutable compute capture directory."
#endif
namespace {
    using namespace Horo::Tests::ShaderCompilerFixture;

    std::vector<std::uint8_t> Captured(std::string_view name) {
        return Read(std::filesystem::path{HORO_VFX_GPU_REFLECTION_FIXTURE_PATH} / name);
    }

    std::filesystem::path After(const ExternalProcessRequest &request, std::string_view flag) {
        const auto found = std::ranges::find(request.arguments, flag);
        REQUIRE(found != request.arguments.end());
        REQUIRE(std::next(found) != request.arguments.end());
        return *std::next(found);
    }

    void Write(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }

    /** @brief Replays genuine capture bytes through a scripted runner; proves routing only, never real tool execution. */
    class Processes final : public IExternalProcessRunner {
    public:
        std::vector<std::string> roles;

        Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &) override {
            CHECK(request.environment.base == ProcessEnvironmentBase::Replace);
            const auto role = std::filesystem::path{request.executable}.filename().string();
            roles.push_back(role);
            CHECK(std::ranges::find(request.arguments, "--msl") == request.arguments.end());
            if (role == "dxc")
                Write(After(request, "-Fo"), Captured("compute.spv"));
            else if (role == "spirv-cross") {
                REQUIRE(std::ranges::find(request.arguments, "--reflect") != request.arguments.end());
                REQUIRE(Read(request.arguments[0]) == Captured("compute.spv"));
                Write(After(request, "--output"), Captured("compute.json"));
            } else
                REQUIRE(role == "spirv-val");
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 0});
        }
    };
}  // namespace

TEST_CASE("Vulkan VFX compiler uses two compiler identities and independent Cross reflection with no Apple route",
          "[vfx][vulkan][toolchain]") {
    using namespace Horo::Tests::ShaderCompilerFixture;
    TemporaryDirectory temporary;
    auto configuration = FixtureConfiguration(temporary);
    configuration.tools.resize(3);  // Explicit admitted test catalog/runner, not native tool approval.
    auto processes = std::make_shared<Processes>();
    auto adapter = ExternalShaderCompilerAdapter::Create(configuration, processes);
    REQUIRE(adapter.HasValue());
    auto target = Requirement(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16);
    target.maximumBindings = 32;
    target.supportsCompute = true;
    target.supportsStorageResources = true;
    ShaderCompilerTargetDescriptor descriptor{.requirement = target,
                                              .platformTriple = configuration.hostPlatform,
                                              .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16,
                                              .tools = {configuration.tools[0].identity, configuration.tools[1].identity}};
    ShaderCompilationRequest request{.manifest = MakeVfxGpuShaderManifest(VfxGpuProgramRole::Simulation, target),
                                     .source = Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} /
                                                    "assets/shaders/runtime/vfx_simulation.hlsl"),
                                     .targets = {descriptor},
                                     .reflection =
                                         ShaderCompilerReflectionRequest{configuration.tools[2].identity,
                                                                         VfxGpuShaderReflectionNames(VfxGpuProgramRole::Simulation)}};
    const auto result = CompileShaderTargets(request, adapter.Value(), {});
    INFO((result.HasError() ? result.ErrorValue().message : "Scripted routing accepted genuine capture bytes."));
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().artifacts.size() == 1);
    REQUIRE(result.Value().artifacts[0].reflection != nullptr);
    CHECK((processes->roles == std::vector<std::string>{"dxc", "spirv-val", "spirv-cross"}));
}
