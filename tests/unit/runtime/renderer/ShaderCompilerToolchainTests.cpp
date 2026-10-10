#include "Horo/Application/ShaderBuildService.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "ShaderCompilerToolchain.h"
#include "support/TypedIdentityTestSupport.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using Tests::RequireError;

    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::uint64_t next = 1;
            path_ = std::filesystem::temp_directory_path() / ("horo-shader-toolchain-test-" + std::to_string(next++));
            std::filesystem::create_directories(path_);
        }

        ~TemporaryDirectory() {
            Cleanup();
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;
        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        void Cleanup() noexcept {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        std::filesystem::path path_;
    };

    class ExactToolCatalog final : public IVerifiedShaderCompilerToolCatalog {
    public:
        ExactToolCatalog(std::string hostPlatform, ShaderCompilerToolIdentity identity, const Sha256Digest &executableDigest)
            : hostPlatform_(std::move(hostPlatform)), identity_(std::move(identity)), executableDigest_(executableDigest) {}

        [[nodiscard]] bool Approves(const std::string_view hostPlatform, const ShaderCompilerToolIdentity &identity,
                                    const Sha256Digest &executableDigest) const noexcept override {
            return hostPlatform == hostPlatform_ && identity.tool == identity_.tool && identity.release == identity_.release &&
                   identity.buildDigest == identity_.buildDigest && executableDigest == executableDigest_;
        }

    private:
        std::string hostPlatform_;
        ShaderCompilerToolIdentity identity_;
        Sha256Digest executableDigest_;
    };

    class TestToolCatalog final : public IVerifiedShaderCompilerToolCatalog {
    public:
        [[nodiscard]] bool Approves(std::string_view, const ShaderCompilerToolIdentity &, const Sha256Digest &) const noexcept override {
            return true;
        }
    };

    class FixtureProcessRunner final : public IExternalProcessRunner {
    public:
        enum class Output {
            Normal,
            Warning,
            Failure,
            IncludeFailure,
            Timeout,
            Flood,
            WaitForCancellation
        };

        explicit FixtureProcessRunner(const Output output = Output::Normal) : output_(output) {}

        std::atomic<std::size_t> started{};

        [[nodiscard]] Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &token) override {
            ++started;
            if (output_ == Output::WaitForCancellation) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                while (!token.IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                return Result<ExternalProcessResult>::Success(
                    {token.IsCancellationRequested() ? ProcessTerminationReason::Cancelled : ProcessTerminationReason::TimedOut, -1});
            }
            if (output_ == Output::Timeout)
                return Result<ExternalProcessResult>::Success({ProcessTerminationReason::TimedOut, -1});
            if (output_ == Output::Failure || output_ == Output::IncludeFailure) {
                const auto source =
                    request.workingDirectory / (output_ == Output::Failure ? "source.hlsl" : "include/portable_constants.hlsli");
                request.onOutput({ProcessOutputStream::StandardError, source.string() + ":2:3: error X3000: invalid shader", false});
                return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 1});
            }
            if (output_ == Output::Warning)
                request.onOutput({ProcessOutputStream::StandardOutput,
                                  (request.workingDirectory / "source.hlsl").string() + ":2:3: warning W1234: retained warning", false});
            if (output_ == Output::Flood)
                for (std::size_t index = 0; index < 20; ++index)
                    request.onOutput({ProcessOutputStream::StandardError, std::string(500, 'x'), true});
            if (request.onOutput)
                request.onOutput(
                    {ProcessOutputStream::StandardOutput, (request.workingDirectory / "source.hlsl").string() + ":2:3: compiled", false});

            const std::filesystem::path executable{request.executable};
            const std::string role = executable.filename().string();
            if (role == "spirv-val" || role == "dxil-validator")
                return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 0});

            const auto outputAfter = [&](const std::string_view option) -> std::filesystem::path {
                const auto found = std::ranges::find(request.arguments, option);
                REQUIRE(found != request.arguments.end());
                REQUIRE(std::next(found) != request.arguments.end());
                return *std::next(found);
            };
            if (role == "dxc") {
                const bool spirv = std::ranges::find(request.arguments, "-spirv") != request.arguments.end();
                std::vector<std::uint8_t> bytes(spirv ? 20U : 32U);
                if (spirv)
                    bytes = {0x03, 0x02, 0x23, 0x07, 0, 0x06, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                else
                    bytes[0] = 'D', bytes[1] = 'X', bytes[2] = 'B', bytes[3] = 'C';
                Write(outputAfter("-Fo"), bytes);
                if (const auto pdbOption = std::ranges::find(request.arguments, "-Fd"); pdbOption != request.arguments.end())
                    Write(*std::next(pdbOption), {'P', 'D', 'B'});
            } else if (role == "spirv-cross") {
                const bool metal = std::ranges::find(request.arguments, "--msl") != request.arguments.end();
                const std::string text = metal ? "#include <metal_stdlib>" : "#version 410\nvoid main(){}";
                Write(outputAfter("--output"), {text.begin(), text.end()});
            } else if (role == "apple-metal") {
                const bool library = std::ranges::find(request.arguments, "metallib") != request.arguments.end();
                Write(outputAfter("-o"),
                      library ? std::vector<std::uint8_t>{'M', 'T', 'L', 'B'} : std::vector<std::uint8_t>{'A', 'I', 'R'});
            }
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 0});
        }

    private:
        Output output_;

        static void Write(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
    };

    [[nodiscard]] std::vector<std::uint8_t> Read(const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        const auto size = stream.tellg();
        stream.seekg(0);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
        return bytes;
    }

    [[nodiscard]] ShaderTargetRequirement Requirement(const ShaderTargetBackend backend, const ShaderPayloadFormat format) {
        ShaderTargetRequirement requirement;
        requirement.backend = backend;
        requirement.payloadFormat = format;
        requirement.descriptorVersion = 1;
        requirement.interfaceSchemaVersion = 1;
        requirement.maximumBindings = 16;
        requirement.maximumInlineConstantBytes = 128;
        return requirement;
    }

    [[nodiscard]] const ApprovedShaderCompilerTool &Approved(const ShaderCompilerTool tool) {
        const auto catalog = ApprovedShaderCompilerTools();
        const auto found = std::ranges::find_if(catalog, [tool](const ApprovedShaderCompilerTool &entry) {
            return entry.hostPlatform == "linux-x86_64-ubuntu-26.04" && entry.tool == tool;
        });
        REQUIRE(found != catalog.end());
        return *found;
    }

    [[nodiscard]] ShaderCompilerToolIdentity Identity(const ShaderCompilerTool tool) {
        const auto &approved = Approved(tool);
        auto digest = ParseSha256(approved.archiveSha256);
        REQUIRE(digest.HasValue());
        return {tool, std::string{approved.release}, digest.Value()};
    }

    [[nodiscard]] std::optional<ShaderCompilerToolInstallation> Installation(const ShaderCompilerTool tool, const char *variable) {
        const char *path = std::getenv(variable);
        if (path == nullptr || *path == '\0')
            return std::nullopt;
        const auto &approved = Approved(tool);
        auto executableDigest = ParseSha256(approved.executableSha256);
        REQUIRE(executableDigest.HasValue());
        return ShaderCompilerToolInstallation{Identity(tool), std::filesystem::path{path}, executableDigest.Value()};
    }

    [[nodiscard]] ShaderCompilationRequest Request(const ShaderTargetBackend backend, const ShaderPayloadFormat format,
                                                   std::vector<ShaderCompilerToolIdentity> tools) {
        ShaderCompilationRequest request;
        request.source = Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/shaders/portable_baseline.hlsl");
        std::vector<std::uint8_t> include =
            Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/shaders/include/portable_constants.hlsli");
        request.dependencies = {{"include/portable_constants.hlsli", ComputeSha256(std::as_bytes(std::span{include})), std::move(include)}};
        request.manifest = {.schemaVersion = 1,
                            .sourceIdentity = "tests.shaders.portable_baseline",
                            .sourceRevision = 1,
                            .entryPoints = {{ShaderStage::Vertex, "VertexMain"}, {ShaderStage::Fragment, "FragmentMain"}},
                            .bindings = {},
                            .parameters = {},
                            .inlineConstants = {},
                            .specializationInputs = {},
                            .targets = {Requirement(backend, format)}};
        ShaderCompilerTargetDescriptor target;
        target.requirement = request.manifest.targets.front();
        target.platformTriple = "windows-x64-d3d12";
        if (backend == ShaderTargetBackend::Vulkan)
            target.platformTriple = "desktop-vulkan";
        else if (backend == ShaderTargetBackend::OpenGL)
            target.platformTriple = "desktop-opengl";
        else if (backend == ShaderTargetBackend::Metal)
            target.platformTriple = "macos-arm64-metal";
        target.intermediateEnvironment = backend == ShaderTargetBackend::Null || backend == ShaderTargetBackend::D3D12
                                             ? ShaderIntermediateEnvironment::None
                                             : ShaderIntermediateEnvironment::Vulkan13SpirV16;
        target.tools = std::move(tools);
        request.targets = {std::move(target)};
        return request;
    }

    [[nodiscard]] ShaderCompilerToolIdentity FixtureIdentity(const ShaderCompilerTool tool) {
        using enum ShaderCompilerTool;
        std::string_view name;
        switch (tool) {
            case Dxc:
                name = "dxc";
                break;
            case SpirvTools:
                name = "spirv-val";
                break;
            case SpirvCross:
                name = "spirv-cross";
                break;
            case AppleMetal:
                name = "apple-metal";
                break;
            case DxilValidator:
                name = "dxil-validator";
                break;
        }
        return {tool, "fixture-v1", ComputeSha256(std::as_bytes(std::span{name.data(), name.size()}))};
    }

    [[nodiscard]] ShaderCompilerToolchainConfiguration FixtureConfiguration(const TemporaryDirectory &temporary) {
        ShaderCompilerToolchainConfiguration configuration;
        configuration.hostPlatform = "contract-test-host";
        configuration.scratchRoot = temporary.Path() / "scratch";
        configuration.verifiedCatalog = std::make_shared<TestToolCatalog>();
        for (const auto [tool, name] : {
                 std::pair{ShaderCompilerTool::Dxc, "dxc"},
                 std::pair{ShaderCompilerTool::SpirvTools, "spirv-val"},
                 std::pair{ShaderCompilerTool::SpirvCross, "spirv-cross"},
                 std::pair{ShaderCompilerTool::AppleMetal, "apple-metal"},
                 std::pair{ShaderCompilerTool::DxilValidator, "dxil-validator"},
             }) {
            const std::filesystem::path executable = temporary.Path() / name;
            {
                std::ofstream output(executable, std::ios::binary);
                output << name;
            }
            const std::vector<std::uint8_t> bytes = Read(executable);
            const Sha256Digest digest = ComputeSha256(std::as_bytes(std::span{bytes}));
            configuration.tools.emplace_back(ShaderCompilerToolIdentity{tool, "fixture-v1", digest}, executable, digest);
        }
        return configuration;
    }

    void RequireFixtureRoute(const ExternalShaderCompilerAdapter &adapter, const ShaderTargetBackend backend,
                             const ShaderPayloadFormat format, std::vector<ShaderCompilerToolIdentity> tools) {
        auto request = Request(backend, format, std::move(tools));
        request.targets.front().emitDebugInformation = true;
        const auto result = CompileShaderTargets(request, adapter, {});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().artifacts.size() == 1);
        CHECK_FALSE(result.Value().artifacts.front().payload.empty());
        CHECK_FALSE(result.Value().artifacts.front().debugPayload.empty());
        CHECK_FALSE(result.Value().artifacts.front().diagnostics.empty());
    }

    [[nodiscard]] Result<ExternalShaderCompilerAdapter> CreateEmptyAdapter(const TemporaryDirectory &temporary,
                                                                           std::shared_ptr<IExternalProcessRunner> processes) {
        ShaderCompilerToolchainConfiguration configuration;
        configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
        configuration.scratchRoot = temporary.Path() / "scratch";
        return ExternalShaderCompilerAdapter::Create(std::move(configuration), std::move(processes));
    }

    void RequireRepeatable(const ShaderCompilationRequest &request, const IShaderCompilerAdapter &adapter) {
        const auto first = CompileShaderTargets(request, adapter, {});
        REQUIRE(first.HasValue());
        const auto second = CompileShaderTargets(request, adapter, {});
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().artifacts.size() == 1U);
        CHECK(first.Value().artifacts.front().payload == second.Value().artifacts.front().payload);
        CHECK(first.Value().artifacts.front().artifactKey == second.Value().artifacts.front().artifactKey);
    }
}  // namespace

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

namespace {
    Application::ShaderBuildRequest BuildRequest(const TemporaryDirectory &temporary) {
        Application::ShaderBuildRequest request;
        request.compilation = Request(ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60,
                                      {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::DxilValidator)});
        request.sources = {{request.compilation.manifest.sourceIdentity, temporary.Path() / "original shader.hlsl"},
                           {"include/portable_constants.hlsli", temporary.Path() / "original include.hlsli"}};
        request.operationId = 37;
        return request;
    }

    std::shared_ptr<const IShaderCompilerAdapter> BuildAdapter(const TemporaryDirectory &temporary,
                                                               const std::shared_ptr<FixtureProcessRunner> &processes) {
        auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
        REQUIRE(adapter.HasValue());
        return std::make_shared<ExternalShaderCompilerAdapter>(std::move(adapter).Value());
    }

    std::future<Result<ShaderCompilationBatch>> CompileWorker(Application::ShaderBuildService &service,
                                                              Application::ShaderBuildRequest request,
                                                              CancellationToken cancellation = {}) {
        return std::async(std::launch::async, [&service, request = std::move(request), cancellation]() mutable {
            return service.Compile(std::move(request), cancellation);
        });
    }

    void RequireTerminal(const BuildOutputSnapshot &snapshot, const BuildOutputResult result) {
        CHECK(std::ranges::count_if(snapshot.records, [](const auto &record) {
            return record.result != BuildOutputResult::None;
        }) == 1);
        REQUIRE_FALSE(snapshot.records.empty());
        CHECK(snapshot.records.back().result == result);
        const auto session = snapshot.records.back().sessionId;
        REQUIRE(session);
        CHECK(session->IsValid());
        for (const auto &record : snapshot.records) {
            CHECK(record.timestampUtc.time_since_epoch() > std::chrono::system_clock::duration::zero());
            CHECK(record.sessionId == session);
            CHECK(record.operationId == 37);
        }
    }
}  // namespace

TEST_CASE("Production shader diagnostics reach the host output store with navigable warnings", "[shader][build_output]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>(FixtureProcessRunner::Output::Warning);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    auto result = CompileWorker(service, BuildRequest(temporary)).get();
    REQUIRE(result.HasValue());
    auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, BuildOutputResult::Succeeded);
    const auto warning = std::ranges::find_if(snapshot->records, [](const auto &record) {
        return record.toolCode == "W1234";
    });
    REQUIRE(warning != snapshot->records.end());
    CHECK(warning->severity == DiagnosticSeverity::Warning);
    CHECK(warning->result == BuildOutputResult::None);
    CHECK(warning->code.Value() == "render.shader_compiler.source");
    REQUIRE(warning->source);
    CHECK(warning->source->absolutePath == (temporary.Path() / "original shader.hlsl").string());
    CHECK(warning->source->line == 2);
    CHECK(warning->source->column == 3);
    CHECK(warning->stage.find("shader/compile") != std::string::npos);
    CHECK(warning->stage.find("entry=VertexMain") != std::string::npos);
    CHECK(warning->stage.find("revision=1") != std::string::npos);
    CHECK(warning->stage.find("build=sha256:") != std::string::npos);
    CHECK(warning->message.find(temporary.Path().string()) == std::string::npos);
}

TEST_CASE("Shader failures retain native codes include locations and one terminal", "[shader][build_output]") {
    TemporaryDirectory temporary;
    const auto mode = GENERATE(FixtureProcessRunner::Output::Failure, FixtureProcessRunner::Output::IncludeFailure,
                               FixtureProcessRunner::Output::Timeout);
    auto processes = std::make_shared<FixtureProcessRunner>(mode);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    auto result = CompileWorker(service, BuildRequest(temporary)).get();
    REQUIRE(result.HasError());
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, mode == FixtureProcessRunner::Output::Timeout ? BuildOutputResult::TimedOut : BuildOutputResult::Failed);
    if (mode != FixtureProcessRunner::Output::Timeout) {
        const auto diagnostic = std::ranges::find_if(snapshot->records, [](const auto &record) {
            return record.toolCode == "X3000";
        });
        REQUIRE(diagnostic != snapshot->records.end());
        CHECK(diagnostic->severity == DiagnosticSeverity::Error);
        REQUIRE(diagnostic->source);
        CHECK(diagnostic->source->absolutePath ==
              (temporary.Path() / (mode == FixtureProcessRunner::Output::Failure ? "original shader.hlsl" : "original include.hlsli"))
                  .string());
        CHECK(diagnostic->source->line == 2);
        CHECK(diagnostic->source->column == 3);
        CHECK(diagnostic->code.Value() ==
              (mode == FixtureProcessRunner::Output::Failure ? "render.shader_compiler.source" : "render.shader_compiler.include"));
    }
}

TEST_CASE("Shader output limits are visible and unmapped diagnostics remain non-navigable", "[shader][build_output]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>(FixtureProcessRunner::Output::Flood);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    auto request = BuildRequest(temporary);
    request.sources.clear();
    request.limits.maximumDiagnosticsPerTarget = 2;
    request.limits.maximumDiagnosticMessageBytes = 64;
    auto result = CompileWorker(service, std::move(request)).get();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().artifacts.front().diagnostics.size() == 2);
    CHECK(result.Value().artifacts.front().diagnostics.back().truncated);
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, BuildOutputResult::Succeeded);
    CHECK(std::ranges::any_of(snapshot->records, [](const auto &record) {
        return record.message.find("truncated") != std::string::npos;
    }));
    CHECK(std::ranges::none_of(snapshot->records, [](const auto &record) {
        return record.source.has_value();
    }));
}

TEST_CASE("Shader producer enforces owner affinity typed missing tools and source admission", "[shader][build_output]") {
    TemporaryDirectory temporary;
    BuildOutputStore output{128};
    Application::ShaderBuildService service{{}, output};
    RequireError(service.Compile(BuildRequest(temporary)), Application::ShaderBuildErrors::OwnerThread);
    CHECK_FALSE(output.SnapshotIfChanged(0));
    auto missing = CompileWorker(service, BuildRequest(temporary)).get();
    RequireError(missing, ShaderCompilerPipelineErrors::ToolMissing);
    auto request = BuildRequest(temporary);
    request.sources.front().absolutePath = "relative shader.hlsl";
    RequireError(CompileWorker(service, std::move(request)).get(), Application::ShaderBuildErrors::InvalidSources);
    service.Shutdown();
    RequireError(CompileWorker(service, BuildRequest(temporary)).get(), Application::ShaderBuildErrors::Closed);
}

TEST_CASE("Shader cancellation stays live and shutdown drains concurrent compiler calls", "[shader][build_output]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>(FixtureProcessRunner::Output::WaitForCancellation);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    CancellationSource parent;
    auto first = CompileWorker(service, BuildRequest(temporary), parent.Token());
    auto secondRequest = BuildRequest(temporary);
    secondRequest.operationId = 38;
    auto second = CompileWorker(service, std::move(secondRequest));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (processes->started < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    REQUIRE(processes->started == 2);
    parent.RequestCancellation();
    REQUIRE(first.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(first.get().HasError());
    service.Shutdown();
    REQUIRE(second.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(second.get().HasError());
    service.Shutdown();
    auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    std::vector<BuildOutputSessionId> sessions;
    for (const auto &record : snapshot->records) {
        if (record.result != BuildOutputResult::None) {
            CHECK(record.result == BuildOutputResult::Cancelled);
            REQUIRE(record.sessionId);
            sessions.push_back(*record.sessionId);
        }
    }
    REQUIRE(sessions.size() == 2);
    CHECK(sessions[0] != sessions[1]);
    CHECK(snapshot->records.front().operationId != snapshot->records.back().operationId);
}

TEST_CASE("Shader producer rejects excess concurrent admission without allocating an output session", "[shader][build_output]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>(FixtureProcessRunner::Output::WaitForCancellation);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    std::vector<std::future<Result<ShaderCompilationBatch>>> workers;
    for (std::size_t index = 0; index < 8; ++index)
        workers.push_back(CompileWorker(service, BuildRequest(temporary)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (processes->started < 8 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    REQUIRE(processes->started == 8);
    RequireError(CompileWorker(service, BuildRequest(temporary)).get(), Application::ShaderBuildErrors::Capacity);
    service.Shutdown();
    for (auto &worker : workers)
        REQUIRE(worker.get().HasError());
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    CHECK(std::ranges::count_if(snapshot->records, [](const auto &record) {
        return record.result != BuildOutputResult::None;
    }) == 8);
}

TEST_CASE("Shader adapter allocation failure produces one failed terminal and releases admission", "[shader][build_output]") {
    class AllocatingAdapter final : public IShaderCompilerAdapter {
    public:
        Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &, const CancellationToken &) const override {
            throw std::bad_alloc{};
        }
    };

    TemporaryDirectory temporary;
    BuildOutputStore output{128};
    Application::ShaderBuildService service{std::make_shared<AllocatingAdapter>(), output};
    auto result = CompileWorker(service, BuildRequest(temporary)).get();
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::AllocationFailed.domain,
                             ShaderCompilerPipelineErrors::AllocationFailed.code));
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, BuildOutputResult::Failed);
    service.Shutdown();
}

#ifndef _WIN32
TEST_CASE("Shader producer shutdown cancels and joins an actual native compiler process", "[shader][build_output][process]") {
    TemporaryDirectory temporary;
    auto configuration = FixtureConfiguration(temporary);
    auto &tool = configuration.tools.front();
    {
        std::ofstream executable(tool.executable, std::ios::binary | std::ios::trunc);
        executable << "#!/bin/sh\nprintf started > process-started\nexec /bin/sleep 30\n";
    }
    std::filesystem::permissions(tool.executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    const auto bytes = Read(tool.executable);
    tool.executableDigest = ComputeSha256(std::as_bytes(std::span{bytes}));
    tool.identity.buildDigest = tool.executableDigest;
    configuration.processTimeout = std::chrono::seconds{5};
    auto request = BuildRequest(temporary);
    request.compilation.targets.front().tools.front() = tool.identity;
    auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), std::make_shared<NativeExternalProcessRunner>());
    REQUIRE(adapter.HasValue());
    BuildOutputStore output{128};
    Application::ShaderBuildService service{std::make_shared<ExternalShaderCompilerAdapter>(std::move(adapter).Value()), output};
    auto worker = CompileWorker(service, std::move(request));
    bool started = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!started && std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        std::filesystem::recursive_directory_iterator entries{temporary.Path(), error};
        const std::filesystem::recursive_directory_iterator end;
        while (!error && entries != end) {
            if (entries->path().filename() == "process-started")
                started = true;
            entries.increment(error);
        }
        if (!started)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(started);
    service.Shutdown();
    REQUIRE(worker.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    REQUIRE(worker.get().HasError());
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, BuildOutputResult::Cancelled);
    CHECK(std::filesystem::is_empty(temporary.Path() / "scratch"));
}
#endif
