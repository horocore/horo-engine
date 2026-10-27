#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "support/ShaderCompilerToolchainFixture.h"

#include <array>
#include <utility>

using namespace Horo::Tests::ShaderCompilerFixture;

namespace {
    /** @brief Synchronous unit-only argument recorder; retains no callbacks and claims no actual tool execution. */
    class RecordingProcesses final : public IExternalProcessRunner {
    public:
        struct Call {
            std::string role;
            std::vector<std::string> arguments;
        };

        std::vector<Call> calls;

        Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &cancellation) override {
            calls.push_back({std::filesystem::path{request.executable}.filename().string(), request.arguments});
            return delegate_.Run(request, cancellation);
        }

    private:
        FixtureProcessRunner delegate_;
    };

    /** @brief Checks backend-specific lowering flags and exact deterministic cross-stage varying names. */
    void CheckCrossPolicy(const RecordingProcesses::Call &call, const ShaderTargetBackend backend, const bool vfx) {
        const auto has = [&](const std::string_view option) {
            return std::ranges::find(call.arguments, option) != call.arguments.end();
        };
        CHECK(has("--msl-decoration-binding") == (vfx && backend == ShaderTargetBackend::Metal));
        CHECK(has("--no-support-nonzero-baseinstance") == (vfx && backend == ShaderTargetBackend::OpenGL));
        const bool glVfx = vfx && backend == ShaderTargetBackend::OpenGL;
        CHECK(std::ranges::count(call.arguments, "--rename-interface-variable") == (glVfx ? 2 : 0));
        if (glVfx) {
            const bool vertex = has("VSMain");
            for (const auto &[location, name] : {std::pair{"0", "horo_vfx_uv"}, std::pair{"1", "horo_vfx_color"}}) {
                const std::array expected{std::string{"--rename-interface-variable"}, std::string{vertex ? "out" : "in"},
                                          std::string{location}, std::string{name}};
                CHECK(std::search(call.arguments.begin(), call.arguments.end(), expected.begin(), expected.end()) != call.arguments.end());
            }
        }
    }
}  // namespace

TEST_CASE("VFX raster cook rejects foreign logical reflection names through the production adapter",
          "[runtime][renderer][shader-compiler][toolchain][vfx]") {
    for (const auto backend : {ShaderTargetBackend::Metal, ShaderTargetBackend::OpenGL}) {
        DYNAMIC_SECTION("Backend " << static_cast<unsigned>(backend)) {
            TemporaryDirectory temporary;
            auto processes = std::make_shared<FixtureProcessRunner>();
            auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
            REQUIRE(adapter.HasValue());
            std::vector tools{FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                              FixtureIdentity(ShaderCompilerTool::SpirvCross)};
            if (backend == ShaderTargetBackend::Metal)
                tools.push_back(FixtureIdentity(ShaderCompilerTool::AppleMetal));
            auto request =
                Request(backend, backend == ShaderTargetBackend::Metal ? ShaderPayloadFormat::MetalLibrary24 : ShaderPayloadFormat::Glsl410,
                        std::move(tools));
            request.manifest = MakeVfxShaderManifest(request.targets.front().requirement);
            if (backend == ShaderTargetBackend::OpenGL)
                request.targets.front().intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan12SpirV15;
            request.source = Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/runtime/vfx_instanced.hlsl");
            request.dependencies.clear();
            request.reflection = ShaderCompilerReflectionRequest{FixtureIdentity(ShaderCompilerTool::SpirvCross),
                                                                 {{{1}, "VfxInstances", {{{1}, "ClipFromLocal"}, {{2}, "InstanceColor"}}},
                                                                  {{2}, "VfxMaterial", {{{3}, "Tint"}, {{4}, "AlphaCutoff"}}},
                                                                  {{3}, "BaseColor", {}},
                                                                  {{4}, "BaseColorSampler", {}}}};
            SECTION("Foreign binding") {
                request.reflection->names[0].name = "ForeignInstances";
            }
            SECTION("Foreign member") {
                request.reflection->names[0].parameters[1].name = "ForeignColor";
            }
            SECTION("Names cannot exchange semantic identities") {
                std::swap(request.reflection->names[0].parameters[0].name, request.reflection->names[0].parameters[1].name);
            }
            const auto result = CompileShaderTargets(request, adapter.Value(), {});
            REQUIRE(result.HasError());
            CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::ToolOutputInvalid.domain,
                                     ShaderCompilerPipelineErrors::ToolOutputInvalid.code));
        }
    }
}

TEST_CASE("Explicit VFX profiles map compile validation debug and backend lowering without changing ordinary routes",
          "[runtime][renderer][shader-compiler][toolchain][vfx]") {
    const auto backend = GENERATE(ShaderTargetBackend::OpenGL, ShaderTargetBackend::Metal);
    const bool vfx = GENERATE(false, true);
    TemporaryDirectory temporary;
    auto processes = std::make_shared<RecordingProcesses>();
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());
    std::vector tools{FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                      FixtureIdentity(ShaderCompilerTool::SpirvCross)};
    if (backend == ShaderTargetBackend::Metal)
        tools.push_back(FixtureIdentity(ShaderCompilerTool::AppleMetal));
    auto request =
        Request(backend, backend == ShaderTargetBackend::Metal ? ShaderPayloadFormat::MetalLibrary24 : ShaderPayloadFormat::Glsl410,
                std::move(tools));
    if (vfx)
        request.manifest = MakeVfxShaderManifest(request.targets.front().requirement);
    auto &target = request.targets.front();
    target.emitDebugInformation = true;
    if (vfx && backend == ShaderTargetBackend::OpenGL)
        target.intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan12SpirV15;
    const auto compiled = CompileShaderTargets(request, adapter.Value(), {});
    REQUIRE(compiled.HasValue());  // Scripted process output verifies routing only, never native shader correctness.
    const std::string environment = vfx && backend == ShaderTargetBackend::OpenGL ? "vulkan1.2" : "vulkan1.3";
    bool sawDebug{}, sawValidator{}, sawTranslation{};
    for (const auto &call : processes->calls) {
        const auto has = [&](const std::string_view option) {
            return std::ranges::find(call.arguments, option) != call.arguments.end();
        };
        if (call.role == "dxc") {
            CHECK(has("-fspv-target-env=" + environment));
            sawDebug = sawDebug || has("-Zi");
        } else if (call.role == "spirv-val") {
            const auto option = std::ranges::find(call.arguments, "--target-env");
            REQUIRE(option != call.arguments.end());
            REQUIRE(option + 1 != call.arguments.end());
            CHECK(*(option + 1) == environment);
            sawValidator = true;
        } else if (call.role == "spirv-cross") {
            CheckCrossPolicy(call, backend, vfx);
            sawTranslation = true;
        }
    }
    CHECK(sawDebug);
    CHECK(sawValidator);
    CHECK(sawTranslation);
}

TEST_CASE("Canonical VFX GL requires its explicit legal intermediate descriptor before tool execution",
          "[runtime][renderer][shader-compiler][toolchain][vfx]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<RecordingProcesses>();
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());
    auto request = Request(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410,
                           {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                            FixtureIdentity(ShaderCompilerTool::SpirvCross)});
    request.manifest = MakeVfxShaderManifest(request.targets.front().requirement);
    const auto result = CompileShaderTargets(request, adapter.Value(), {});
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::UnsupportedTarget.domain,
                             ShaderCompilerPipelineErrors::UnsupportedTarget.code));
    CHECK(processes->calls.empty());
}

TEST_CASE("Canonical VFX GL and Metal reject old cook revisions before tool execution",
          "[runtime][renderer][shader-compiler][toolchain][vfx]") {
    const auto backend = GENERATE(ShaderTargetBackend::OpenGL, ShaderTargetBackend::Metal);
    TemporaryDirectory temporary;
    auto processes = std::make_shared<RecordingProcesses>();
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());
    std::vector tools{FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                      FixtureIdentity(ShaderCompilerTool::SpirvCross)};
    if (backend == ShaderTargetBackend::Metal)
        tools.push_back(FixtureIdentity(ShaderCompilerTool::AppleMetal));
    auto request =
        Request(backend, backend == ShaderTargetBackend::Metal ? ShaderPayloadFormat::MetalLibrary24 : ShaderPayloadFormat::Glsl410,
                std::move(tools));
    if (backend == ShaderTargetBackend::OpenGL)
        request.targets.front().intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan12SpirV15;
    request.manifest = MakeVfxShaderManifest(request.targets.front().requirement);
    request.manifest.sourceRevision = 2;
    const auto result = CompileShaderTargets(request, adapter.Value(), {});
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::UnsupportedTarget.domain,
                             ShaderCompilerPipelineErrors::UnsupportedTarget.code));
    CHECK(processes->calls.empty());
}

TEST_CASE("Shader toolchain lock rejects executable bytes that do not match the reviewed artifact",
          "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    const std::filesystem::path executable = temporary.Path() / "dxc";
    {
        std::ofstream output(executable, std::ios::binary);
        output << "not the approved compiler";
    }
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    auto digest = ParseSha256(Approved(ShaderCompilerTool::Dxc).executableSha256);
    REQUIRE(digest.HasValue());
    configuration.tools = {{Identity(ShaderCompilerTool::Dxc), executable, digest.Value()}};
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    RequireError(ExternalShaderCompilerAdapter::Create(std::move(configuration), processes),
                 ShaderCompilerPipelineErrors::ToolDigestMismatch);
}

TEST_CASE("Null shader validation artifact is deterministic and leaves no scratch generation",
          "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    auto adapter = CreateEmptyAdapter(temporary, processes);
    REQUIRE(adapter.HasValue());

    auto request = Request(ShaderTargetBackend::Null, ShaderPayloadFormat::ValidationFixture, {});
    request.targets.front().platformTriple = "headless-null";
    RequireRepeatable(request, adapter.Value());
    CHECK(std::filesystem::is_empty(temporary.Path() / "scratch"));
}

TEST_CASE("Shader toolchain adapter retains its shared process runner", "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    std::weak_ptr<IExternalProcessRunner> retained = processes;

    {
        auto adapter = CreateEmptyAdapter(temporary, processes);
        REQUIRE(adapter.HasValue());
        processes.reset();
        CHECK_FALSE(retained.expired());
    }
    CHECK(retained.expired());
}

TEST_CASE("Shader toolchain adapter rejects a missing process runner", "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    RequireError(CreateEmptyAdapter(temporary, {}), ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid);
}

TEST_CASE("Verified host catalogs admit exact tools without a built-in OS restriction", "[runtime][renderer][shader-compiler][toolchain]") {
    for (const std::string hostPlatform : {"windows-x86_64", "macos-arm64"}) {
        TemporaryDirectory temporary;
        const std::filesystem::path executable = temporary.Path() / "tool";
        {
            std::ofstream output(executable, std::ios::binary);
            output << hostPlatform;
        }
        const std::vector<std::uint8_t> executableBytes = Read(executable);
        const Sha256Digest executableDigest = ComputeSha256(std::as_bytes(std::span{executableBytes}));
        const auto hostBytes = std::as_bytes(std::span{hostPlatform.data(), hostPlatform.size()});
        ShaderCompilerToolIdentity identity{ShaderCompilerTool::Dxc, "catalog-contract-test", ComputeSha256(hostBytes)};

        ShaderCompilerToolchainConfiguration configuration;
        configuration.hostPlatform = hostPlatform;
        configuration.scratchRoot = temporary.Path() / "scratch";
        configuration.tools = {{identity, executable, executableDigest}};
        configuration.verifiedCatalog = std::make_shared<ExactToolCatalog>(hostPlatform, identity, executableDigest);
        auto processes = std::make_shared<NativeExternalProcessRunner>();
        CHECK(ExternalShaderCompilerAdapter::Create(std::move(configuration), processes).HasValue());
    }
}

TEST_CASE("Production shader adapter reports an unavailable required tool without fallback",
          "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    auto adapter = CreateEmptyAdapter(temporary, processes);
    REQUIRE(adapter.HasValue());

    const auto request = Request(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16,
                                 {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools)});
    const auto result = CompileShaderTargets(request, adapter.Value(), {});
    RequireError(result, ShaderCompilerPipelineErrors::AdapterFailure);
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::ToolMissing.domain,
                             ShaderCompilerPipelineErrors::ToolMissing.code));
    CHECK(std::filesystem::is_empty(temporary.Path() / "scratch"));
}

TEST_CASE("Verified fixture tools exercise every contract-tested backend route", "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>();
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());

    RequireFixtureRoute(adapter.Value(), ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16,
                        {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools)});
    RequireFixtureRoute(adapter.Value(), ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410,
                        {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                         FixtureIdentity(ShaderCompilerTool::SpirvCross)});
    RequireFixtureRoute(adapter.Value(), ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24,
                        {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                         FixtureIdentity(ShaderCompilerTool::SpirvCross), FixtureIdentity(ShaderCompilerTool::AppleMetal)});
    RequireFixtureRoute(adapter.Value(), ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60,
                        {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::DxilValidator)});
}

TEST_CASE("Locked Linux shader tools produce repeatable validated native artifacts",
          "[runtime][renderer][shader-compiler][toolchain][integration]") {
    auto dxc = Installation(ShaderCompilerTool::Dxc, "HORO_TEST_SHADER_DXC");
    auto spirvTools = Installation(ShaderCompilerTool::SpirvTools, "HORO_TEST_SHADER_SPIRV_VAL");
    auto spirvCross = Installation(ShaderCompilerTool::SpirvCross, "HORO_TEST_SHADER_SPIRV_CROSS");
    auto dxilValidator = Installation(ShaderCompilerTool::DxilValidator, "HORO_TEST_SHADER_DXIL_VALIDATOR");
    if (!dxc || !spirvTools || !spirvCross || !dxilValidator)
        SKIP("Set the four HORO_TEST_SHADER_* paths to the reviewed Linux tool artifacts.");

    TemporaryDirectory temporary;
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    configuration.tools = {*dxc, *spirvTools, *spirvCross, *dxilValidator};
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), processes);
    REQUIRE(adapter.HasValue());

    const auto verify = [&](const ShaderTargetBackend backend, const ShaderPayloadFormat format,
                            std::vector<ShaderCompilerToolIdentity> tools) {
        const ShaderCompilationRequest request = Request(backend, format, std::move(tools));
        RequireRepeatable(request, adapter.Value());
    };

    verify(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16,
           {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools)});
    verify(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410,
           {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools), Identity(ShaderCompilerTool::SpirvCross)});
    verify(ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60,
           {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::DxilValidator)});
}
