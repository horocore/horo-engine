/** @file
 * @brief Shared verified-tool and bounded-process fixtures for shader compiler and producer tests.
 */
#pragma once

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

namespace Horo::Tests::ShaderCompilerFixture {
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
            if (auto outcome = DiagnosticOutcome(request, token))
                return std::move(*outcome);
            if (request.onOutput)
                request.onOutput(
                    {ProcessOutputStream::StandardOutput, (request.workingDirectory / "source.hlsl").string() + ":2:3: compiled", false});

            const std::filesystem::path executable{request.executable};
            const std::string role = executable.filename().string();
            if (role == "spirv-val" || role == "dxil-validator")
                return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 0});

            if (role == "dxc") {
                EmitDxcPayload(request);
            } else if (role == "spirv-cross") {
                const bool metal = std::ranges::find(request.arguments, "--msl") != request.arguments.end();
                const std::string text = metal ? "#include <metal_stdlib>" : "#version 410\nvoid main(){}";
                Write(OutputAfter(request, "--output"), {text.begin(), text.end()});
            } else if (role == "apple-metal") {
                const bool library = std::ranges::find(request.arguments, "metallib") != request.arguments.end();
                Write(OutputAfter(request, "-o"),
                      library ? std::vector<std::uint8_t>{'M', 'T', 'L', 'B'} : std::vector<std::uint8_t>{'A', 'I', 'R'});
            }
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 0});
        }

    private:
        /** @brief Bound the cancellation fixture so a regression cannot leave an orphaned worker. */
        static Result<ExternalProcessResult> WaitForCancellation(const CancellationToken &token) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!token.IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            return Result<ExternalProcessResult>::Success(
                {token.IsCancellationRequested() ? ProcessTerminationReason::Cancelled : ProcessTerminationReason::TimedOut, -1});
        }

        /** @brief Emit the selected diagnostic fixture before normal tool payload generation. */
        std::optional<Result<ExternalProcessResult>> DiagnosticOutcome(const ExternalProcessRequest &request,
                                                                       const CancellationToken &token) const {
            if (output_ == Output::WaitForCancellation) {
                return WaitForCancellation(token);
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
            return std::nullopt;
        }

        /** @brief Require the fixture command to provide the output path used by its verified tool role. */
        static std::filesystem::path OutputAfter(const ExternalProcessRequest &request, const std::string_view option) {
            const auto found = std::ranges::find(request.arguments, option);
            REQUIRE(found != request.arguments.end());
            REQUIRE(std::next(found) != request.arguments.end());
            return *std::next(found);
        }

        /** @brief Materialize the deterministic DXC fixture payload and optional debug sidecar. */
        static void EmitDxcPayload(const ExternalProcessRequest &request) {
            const bool spirv = std::ranges::find(request.arguments, "-spirv") != request.arguments.end();
            std::vector<std::uint8_t> bytes(spirv ? 20U : 32U);
            if (spirv)
                bytes = {0x03, 0x02, 0x23, 0x07, 0, 0x06, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            else
                bytes[0] = 'D', bytes[1] = 'X', bytes[2] = 'B', bytes[3] = 'C';
            Write(OutputAfter(request, "-Fo"), bytes);
            if (const auto pdbOption = std::ranges::find(request.arguments, "-Fd"); pdbOption != request.arguments.end())
                Write(*std::next(pdbOption), {'P', 'D', 'B'});
        }

        Output output_;

        static void Write(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
    };

    [[nodiscard]] inline std::vector<std::uint8_t> Read(const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        const auto size = stream.tellg();
        stream.seekg(0);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
        return bytes;
    }

    [[nodiscard]] inline ShaderTargetRequirement Requirement(const ShaderTargetBackend backend, const ShaderPayloadFormat format) {
        ShaderTargetRequirement requirement;
        requirement.backend = backend;
        requirement.payloadFormat = format;
        requirement.descriptorVersion = 1;
        requirement.interfaceSchemaVersion = 1;
        requirement.maximumBindings = 16;
        requirement.maximumInlineConstantBytes = 128;
        return requirement;
    }

    [[nodiscard]] inline const ApprovedShaderCompilerTool &Approved(const ShaderCompilerTool tool) {
        const auto catalog = ApprovedShaderCompilerTools();
        const auto found = std::ranges::find_if(catalog, [tool](const ApprovedShaderCompilerTool &entry) {
            return entry.hostPlatform == "linux-x86_64-ubuntu-26.04" && entry.tool == tool;
        });
        REQUIRE(found != catalog.end());
        return *found;
    }

    [[nodiscard]] inline ShaderCompilerToolIdentity Identity(const ShaderCompilerTool tool) {
        const auto &approved = Approved(tool);
        auto digest = ParseSha256(approved.archiveSha256);
        REQUIRE(digest.HasValue());
        return {tool, std::string{approved.release}, digest.Value()};
    }

    [[nodiscard]] inline std::optional<ShaderCompilerToolInstallation> Installation(const ShaderCompilerTool tool, const char *variable) {
        const char *path = std::getenv(variable);
        if (path == nullptr || *path == '\0')
            return std::nullopt;
        const auto &approved = Approved(tool);
        auto executableDigest = ParseSha256(approved.executableSha256);
        REQUIRE(executableDigest.HasValue());
        return ShaderCompilerToolInstallation{Identity(tool), std::filesystem::path{path}, executableDigest.Value()};
    }

    [[nodiscard]] inline ShaderCompilationRequest Request(const ShaderTargetBackend backend, const ShaderPayloadFormat format,
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

    [[nodiscard]] inline ShaderCompilerToolIdentity FixtureIdentity(const ShaderCompilerTool tool) {
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

    [[nodiscard]] inline ShaderCompilerToolchainConfiguration FixtureConfiguration(const TemporaryDirectory &temporary) {
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

    inline void RequireFixtureRoute(const ExternalShaderCompilerAdapter &adapter, const ShaderTargetBackend backend,
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

    [[nodiscard]] inline Result<ExternalShaderCompilerAdapter> CreateEmptyAdapter(const TemporaryDirectory &temporary,
                                                                                  std::shared_ptr<IExternalProcessRunner> processes) {
        ShaderCompilerToolchainConfiguration configuration;
        configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
        configuration.scratchRoot = temporary.Path() / "scratch";
        return ExternalShaderCompilerAdapter::Create(std::move(configuration), std::move(processes));
    }

    inline void RequireRepeatable(const ShaderCompilationRequest &request, const IShaderCompilerAdapter &adapter) {
        const auto first = CompileShaderTargets(request, adapter, {});
        REQUIRE(first.HasValue());
        const auto second = CompileShaderTargets(request, adapter, {});
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().artifacts.size() == 1U);
        CHECK(first.Value().artifacts.front().payload == second.Value().artifacts.front().payload);
        CHECK(first.Value().artifacts.front().artifactKey == second.Value().artifacts.front().artifactKey);
    }
}  // namespace Horo::Tests::ShaderCompilerFixture
