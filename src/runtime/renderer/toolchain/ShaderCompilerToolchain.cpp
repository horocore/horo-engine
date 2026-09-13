#include "ShaderCompilerToolchain.h"

#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "ShaderCompilerToolchainSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::size_t HardMaximumToolBinaryBytes = 512U * 1024U * 1024U;
        constexpr std::size_t HardMaximumProcessOutputBytes = 8U * 1024U * 1024U;
        constexpr auto MaximumProcessTimeout = std::chrono::minutes{30};
        using ShaderCompilerToolchainDetail::IsDxil;
        using ShaderCompilerToolchainDetail::IsGlsl410;
        using ShaderCompilerToolchainDetail::IsSpirV;
        using ShaderCompilerToolchainDetail::MakeToolDiagnostic;
        using ShaderCompilerToolchainDetail::PackageStages;
        using ShaderCompilerToolchainDetail::ReadBoundedFile;
        using ShaderCompilerToolchainDetail::SanitizeLine;
        using ShaderCompilerToolchainDetail::StageName;
        using ShaderCompilerToolchainDetail::StageProfile;
        using ShaderCompilerToolchainDetail::ToolArtifactRecord;
        using ShaderCompilerToolchainDetail::WriteFile;

        // Distribution archives and executables were retrieved from the named upstream source and
        // reviewed on 2026-09-13. The companion lock document records runtime dependencies and licenses.
        constexpr std::array ApprovedTools{
            ApprovedShaderCompilerTool{ShaderCompilerTool::Dxc, "linux-x86_64-ubuntu-26.04", "v1.9.2607",
                                       "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/"
                                       "linux_dxc_2026_07_29.x86_x64.tar.gz",
                                       "sha256:55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54",
                                       "sha256:b1bfa493d5c780b94c20b8b5f5aed50d1c4d03339cd55f496bd223eedeec1734", "NCSA-and-MIT"},
            ApprovedShaderCompilerTool{ShaderCompilerTool::SpirvTools, "linux-x86_64-ubuntu-26.04", "ubuntu-2026.1-1",
                                       "https://archive.ubuntu.com/ubuntu/pool/universe/s/spirv-tools/spirv-tools_2026.1-1_amd64.deb",
                                       "sha256:24e972ed4f2e92ada6f64b32ff40550fda02038385656736af871ca3dcb2b867",
                                       "sha256:85367fefdb7e93ae45654255ac2b7f8dc7056b6df78a6fdeb03ce395c7477239", "Apache-2.0"},
            ApprovedShaderCompilerTool{ShaderCompilerTool::SpirvCross, "linux-x86_64-ubuntu-26.04", "ubuntu-2021.01.15+1.4.335.0-1",
                                       "https://archive.ubuntu.com/ubuntu/pool/universe/s/spirv-cross/"
                                       "spirv-cross_2021.01.15+1.4.335.0-1_amd64.deb",
                                       "sha256:50d11b7efc263240d04b015fecfd419377a4a3e2a9cb59387f2229aae03e4e7f",
                                       "sha256:335caee5ce86daefc3dee5e13100c2118a1a1cccb183a8c6df817090e0cbb976", "Apache-2.0"},
            ApprovedShaderCompilerTool{ShaderCompilerTool::DxilValidator, "linux-x86_64-ubuntu-26.04", "v1.9.2607",
                                       "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/"
                                       "linux_dxc_2026_07_29.x86_x64.tar.gz",
                                       "sha256:55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54",
                                       "sha256:87cc9c1e459a7d6a0dc52c7b319d627d0f6078a2be95621e5a98b3cf12f255cb", "NCSA-and-MIT"},
        };

        [[nodiscard]] bool IsSafeIdentity(const std::string_view value) noexcept {
            return !value.empty() && value.size() <= 128U && std::ranges::all_of(value, [](const char character) {
                const auto byte = static_cast<unsigned char>(character);
                return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '.' ||
                       byte == '_' || byte == '-' || byte == '+';
            });
        }

        [[nodiscard]] std::optional<Sha256Digest> ParseLockedDigest(const std::string_view value) {
            auto parsed = ParseSha256(value);
            if (parsed.HasError())
                return std::nullopt;
            return parsed.Value();
        }

        [[nodiscard]] const ApprovedShaderCompilerTool *FindBuiltInApproval(const std::string_view hostPlatform,
                                                                            const ShaderCompilerToolIdentity &identity) {
            const auto found = std::ranges::find_if(ApprovedTools, [&](const ApprovedShaderCompilerTool &entry) {
                const auto archiveDigest = ParseLockedDigest(entry.archiveSha256);
                return archiveDigest.has_value() && entry.hostPlatform == hostPlatform && entry.tool == identity.tool &&
                       entry.release == identity.release && *archiveDigest == identity.buildDigest;
            });
            return found == ApprovedTools.end() ? nullptr : &*found;
        }

        [[nodiscard]] bool IsApproved(const ShaderCompilerToolchainConfiguration &configuration,
                                      const ShaderCompilerToolInstallation &tool) {
            if (configuration.verifiedCatalog)
                return configuration.verifiedCatalog->Approves(configuration.hostPlatform, tool.identity, tool.executableDigest);
            const ApprovedShaderCompilerTool *approved = FindBuiltInApproval(configuration.hostPlatform, tool.identity);
            if (approved == nullptr)
                return false;
            const auto approvedExecutable = ParseLockedDigest(approved->executableSha256);
            return approvedExecutable.has_value() && *approvedExecutable == tool.executableDigest;
        }

        class ScratchDirectory final {
        public:
            explicit ScratchDirectory(std::filesystem::path path) : path_(std::move(path)) {}

            ~ScratchDirectory() noexcept {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }

            [[nodiscard]] const std::filesystem::path &Path() const noexcept {
                return path_;
            }

        private:
            std::filesystem::path path_;
        };

        class CompilerRouteContext final {
        public:
            CompilerRouteContext(const ShaderCompilerToolchainConfiguration &configuration, IExternalProcessRunner &processes,
                                 const ShaderCompilerInvocation &invocation, const CancellationToken &cancellation,
                                 const ScratchDirectory &scratch, const std::filesystem::path &sourcePath)
                : configuration_(configuration), processes_(processes), invocation_(invocation), cancellation_(cancellation),
                  scratch_(scratch), sourcePath_(sourcePath) {}

            [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile() {
                if (invocation_.target.requirement.backend == ShaderTargetBackend::Null)
                    return CompileNullRoute();

                const ShaderCompilerToolInstallation *dxc = Lookup(ShaderCompilerTool::Dxc);
                if (dxc == nullptr)
                    return Failure(ShaderCompilerPipelineErrors::ToolMissing);

                try {
                    payloadStages_.reserve(invocation_.manifest.entryPoints.size());
                    debugStages_.reserve(invocation_.manifest.entryPoints.size());
                } catch (const std::bad_alloc &) {
                    return Failure(ShaderCompilerPipelineErrors::ToolOutputInvalid);
                }

                for (std::size_t index = 0; index < invocation_.manifest.entryPoints.size(); ++index) {
                    const ShaderEntryPoint &entry = invocation_.manifest.entryPoints[index];
                    Result<void> compiled = invocation_.target.requirement.backend == ShaderTargetBackend::D3D12
                                                ? CompileD3D12Route(*dxc, entry, index)
                                                : CompileSpirvRoute(*dxc, entry, index);
                    if (compiled.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(std::move(compiled).ErrorValue());
                }
                return PackageOutput();
            }

        private:
            [[nodiscard]] Result<ShaderCompilerAdapterOutput> Failure(const ErrorCodeDescriptor &error) const {
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(error));
            }

            [[nodiscard]] const ShaderCompilerToolInstallation *Lookup(const ShaderCompilerTool tool) const {
                const auto expected = std::ranges::find_if(invocation_.target.tools, [tool](const ShaderCompilerToolIdentity &identity) {
                    return identity.tool == tool;
                });
                if (expected == invocation_.target.tools.end())
                    return nullptr;
                const auto installed = std::ranges::find_if(configuration_.tools, [&](const ShaderCompilerToolInstallation &candidate) {
                    return candidate.identity.tool == tool && candidate.identity.release == expected->release &&
                           candidate.identity.buildDigest == expected->buildDigest;
                });
                return installed == configuration_.tools.end() ? nullptr : &*installed;
            }

            [[nodiscard]] Result<void> Run(const ShaderCompilerToolInstallation &tool, std::vector<std::string> arguments) {
                if (cancellation_.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
                ExternalProcessRequest request;
                request.executable = tool.executable.string();
                request.arguments = std::move(arguments);
                request.workingDirectory = scratch_.Path();
                request.environment.base = ProcessEnvironmentBase::Replace;
                request.timeout = configuration_.processTimeout;
                request.maximumLineBytes =
                    std::min(invocation_.limits.maximumDiagnosticMessageBytes, configuration_.maximumProcessOutputBytes);
                request.onOutput = [&](ProcessOutputLine line) {
                    CaptureDiagnostic(std::move(line));
                };
                auto result = processes_.Run(request, cancellation_);
                if (result.HasError())
                    return Result<void>::Failure(
                        WrapError(ShaderCompilerPipelineErrors::ToolProcessFailed, std::move(result).ErrorValue()));
                if (result.Value().reason == ProcessTerminationReason::Cancelled || cancellation_.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
                if (result.Value().reason == ProcessTerminationReason::Exited && result.Value().exitCode == 0)
                    return Result<void>::Success();
                return Result<void>::Failure(ProcessFailure());
            }

            void CaptureDiagnostic(ProcessOutputLine line) {
                if (diagnostics_.size() >= invocation_.limits.maximumDiagnosticsPerTarget ||
                    diagnosticBytes_ >= configuration_.maximumProcessOutputBytes)
                    return;
                line.text = SanitizeLine(std::move(line.text), scratch_.Path(), sourcePath_);
                const std::size_t remaining = configuration_.maximumProcessOutputBytes - diagnosticBytes_;
                if (line.text.size() > remaining) {
                    line.text.resize(remaining);
                    line.truncated = true;
                }
                diagnosticBytes_ += line.text.size();
                if (!line.text.empty())
                    diagnostics_.push_back(
                        MakeToolDiagnostic(std::move(line.text), line.truncated, invocation_.manifest.sourceIdentity, line.stream));
            }

            [[nodiscard]] Error ProcessFailure() const {
                Error failure = MakeError(ShaderCompilerPipelineErrors::ToolProcessFailed);
                try {
                    for (const ShaderCompilerDiagnostic &diagnostic : diagnostics_) {
                        failure.diagnostics.push_back(
                            {.code = DiagnosticCode{diagnostic.category == ShaderCompilerDiagnosticCategory::Source
                                                        ? "render.shader_compiler.source"
                                                        : "render.shader_compiler.toolchain"},
                             .severity = diagnostic.severity == ShaderCompilerDiagnosticSeverity::Error     ? DiagnosticSeverity::Error
                                         : diagnostic.severity == ShaderCompilerDiagnosticSeverity::Warning ? DiagnosticSeverity::Warning
                                                                                                            : DiagnosticSeverity::Note,
                             .message = diagnostic.message,
                             .location = {diagnostic.sourceIdentity, diagnostic.line, diagnostic.column}});
                    }
                } catch (const std::bad_alloc &) {
                    failure.diagnostics.clear();
                }
                return failure;
            }

            [[nodiscard]] std::vector<std::string> DxcArguments(const ShaderEntryPoint &entry, const bool spirv,
                                                                const std::filesystem::path &output) const {
                std::vector<std::string>
                    arguments{"-nologo", "-HV", "2021", "-Ges", "-Zpc", "-T", std::string{StageProfile(entry.stage)}, "-E", entry.name};
                arguments.push_back(invocation_.target.enableFastMath ? "-ffinite-math-only" : "-Gis");
                arguments.push_back(invocation_.target.optimization == ShaderOptimizationLevel::Disabled ? "-Od"
                                    : invocation_.target.optimization == ShaderOptimizationLevel::Size   ? "-O1"
                                                                                                         : "-O3");
                for (const ShaderCompilerDefine &define : invocation_.defines) {
                    arguments.emplace_back("-D");
                    arguments.push_back(define.name + "=" + define.value);
                }
                if (spirv)
                    arguments.insert(arguments.end(), {"-spirv", "-fspv-target-env=vulkan1.3", "-fvk-use-gl-layout"});
                arguments.insert(arguments.end(), {"-Fo", output.string(), sourcePath_.string()});
                return arguments;
            }

            [[nodiscard]] Result<void> CompileSpirvRoute(const ShaderCompilerToolInstallation &dxc, const ShaderEntryPoint &entry,
                                                         const std::size_t index) {
                const std::string stem = "stage-" + std::to_string(index);
                const std::filesystem::path spirvPath = scratch_.Path() / (stem + ".spv");
                if (auto compiled = Run(dxc, DxcArguments(entry, true, spirvPath)); compiled.HasError())
                    return compiled;
                const ShaderCompilerToolInstallation *validator = Lookup(ShaderCompilerTool::SpirvTools);
                if (validator == nullptr)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                if (auto validated = Run(*validator, {"--target-env", "vulkan1.3", spirvPath.string()}); validated.HasError())
                    return validated;
                auto spirv =
                    ReadBoundedFile(spirvPath, invocation_.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (spirv.HasError())
                    return Result<void>::Failure(std::move(spirv).ErrorValue());
                if (!IsSpirV(spirv.Value()))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));

                const ShaderTargetBackend backend = invocation_.target.requirement.backend;
                Result<void> routed = backend == ShaderTargetBackend::Vulkan   ? StoreVulkan(entry, std::move(spirv).Value())
                                      : backend == ShaderTargetBackend::OpenGL ? CompileOpenGLRoute(entry, stem, spirvPath)
                                                                               : CompileMetalRoute(entry, stem, spirvPath);
                if (routed.HasError())
                    return routed;
                return CompileSpirvDebug(dxc, entry, stem);
            }

            [[nodiscard]] Result<void> StoreVulkan(const ShaderEntryPoint &entry, std::vector<std::uint8_t> spirv) {
                payloadStages_.push_back({entry.stage, entry.name, std::move(spirv)});
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> CompileOpenGLRoute(const ShaderEntryPoint &entry, const std::string &stem,
                                                          const std::filesystem::path &spirvPath) {
                const ShaderCompilerToolInstallation *translator = Lookup(ShaderCompilerTool::SpirvCross);
                if (translator == nullptr)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                const std::filesystem::path nativePath = scratch_.Path() / (stem + ".native");
                if (auto translated =
                        Run(*translator, {spirvPath.string(), "--output", nativePath.string(), "--entry", entry.name, "--stage",
                                          std::string{StageName(entry.stage)}, "--version", "410", "--no-es", "--no-420pack-extension"});
                    translated.HasError())
                    return translated;
                auto native =
                    ReadBoundedFile(nativePath, invocation_.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (native.HasError())
                    return Result<void>::Failure(std::move(native).ErrorValue());
                if (!IsGlsl410(native.Value()))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                payloadStages_.push_back({entry.stage, entry.name, std::move(native).Value()});
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> CompileMetalRoute(const ShaderEntryPoint &entry, const std::string &stem,
                                                         const std::filesystem::path &spirvPath) {
                const ShaderCompilerToolInstallation *translator = Lookup(ShaderCompilerTool::SpirvCross);
                const ShaderCompilerToolInstallation *metal = Lookup(ShaderCompilerTool::AppleMetal);
                if (translator == nullptr || metal == nullptr)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                const std::filesystem::path nativePath = scratch_.Path() / (stem + ".native");
                if (auto translated = Run(*translator, {spirvPath.string(), "--output", nativePath.string(), "--entry", entry.name,
                                                        "--stage", std::string{StageName(entry.stage)}, "--msl", "--msl-version", "20400"});
                    translated.HasError())
                    return translated;
                auto native =
                    ReadBoundedFile(nativePath, invocation_.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (native.HasError())
                    return Result<void>::Failure(std::move(native).ErrorValue());
                const std::filesystem::path airPath = scratch_.Path() / (stem + ".air");
                if (auto compiled = Run(*metal, {"-sdk", "macosx", "metal", "-std=macos-metal2.4", "-mmacosx-version-min=14.0", "-c",
                                                 nativePath.string(), "-o", airPath.string()});
                    compiled.HasError())
                    return compiled;
                const std::filesystem::path libraryPath = scratch_.Path() / (stem + ".metallib");
                if (auto linked = Run(*metal, {"-sdk", "macosx", "metallib", airPath.string(), "-o", libraryPath.string()});
                    linked.HasError())
                    return linked;
                auto library =
                    ReadBoundedFile(libraryPath, invocation_.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (library.HasError())
                    return Result<void>::Failure(std::move(library).ErrorValue());
                if (library.Value().size() < 4U || library.Value()[0] != 'M' || library.Value()[1] != 'T' || library.Value()[2] != 'L' ||
                    library.Value()[3] != 'B')
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                payloadStages_.push_back({entry.stage, entry.name, std::move(library).Value()});
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> CompileSpirvDebug(const ShaderCompilerToolInstallation &dxc, const ShaderEntryPoint &entry,
                                                         const std::string &stem) {
                if (!invocation_.target.emitDebugInformation)
                    return Result<void>::Success();
                const std::filesystem::path debugPath = scratch_.Path() / (stem + ".debug");
                std::vector<std::string> arguments = DxcArguments(entry, true, debugPath);
                const auto optimization = std::ranges::find_if(arguments, [](const std::string &argument) {
                    return argument == "-O1" || argument == "-O3";
                });
                if (optimization != arguments.end())
                    *optimization = "-Od";
                arguments.insert(arguments.end() - 3, {"-Zi", "-Qembed_debug"});
                if (auto compiled = Run(dxc, std::move(arguments)); compiled.HasError())
                    return compiled;
                auto debug = ReadBoundedFile(debugPath, invocation_.limits.maximumDebugPayloadBytes,
                                             ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (debug.HasError())
                    return Result<void>::Failure(std::move(debug).ErrorValue());
                if (!IsSpirV(debug.Value()))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                debugStages_.push_back({entry.stage, entry.name, std::move(debug).Value()});
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> CompileD3D12Route(const ShaderCompilerToolInstallation &dxc, const ShaderEntryPoint &entry,
                                                         const std::size_t index) {
                const std::string stem = "stage-" + std::to_string(index);
                const std::filesystem::path nativePath = scratch_.Path() / (stem + ".native");
                if (auto compiled = Run(dxc, DxcArguments(entry, false, nativePath)); compiled.HasError())
                    return compiled;
                const ShaderCompilerToolInstallation *validator = Lookup(ShaderCompilerTool::DxilValidator);
                if (validator == nullptr)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                if (auto validated = Run(*validator, {nativePath.string()}); validated.HasError())
                    return validated;
                auto native =
                    ReadBoundedFile(nativePath, invocation_.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (native.HasError())
                    return Result<void>::Failure(std::move(native).ErrorValue());
                if (!IsDxil(native.Value()))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                payloadStages_.push_back({entry.stage, entry.name, std::move(native).Value()});
                return CompileD3D12Debug(dxc, entry, stem);
            }

            [[nodiscard]] Result<void> CompileD3D12Debug(const ShaderCompilerToolInstallation &dxc, const ShaderEntryPoint &entry,
                                                         const std::string &stem) {
                if (!invocation_.target.emitDebugInformation)
                    return Result<void>::Success();
                const std::filesystem::path debugPath = scratch_.Path() / (stem + ".debug.dxil");
                const std::filesystem::path pdbPath = scratch_.Path() / (stem + ".pdb");
                std::vector<std::string> arguments = DxcArguments(entry, false, debugPath);
                const auto optimization = std::ranges::find_if(arguments, [](const std::string &argument) {
                    return argument == "-O1" || argument == "-O3";
                });
                if (optimization != arguments.end())
                    *optimization = "-Od";
                arguments.insert(arguments.end() - 3, {"-Zi", "-Fd", pdbPath.string()});
                if (auto compiled = Run(dxc, std::move(arguments)); compiled.HasError())
                    return compiled;
                auto pdb =
                    ReadBoundedFile(pdbPath, invocation_.limits.maximumDebugPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (pdb.HasError())
                    return Result<void>::Failure(std::move(pdb).ErrorValue());
                debugStages_.push_back({entry.stage, entry.name, std::move(pdb).Value()});
                return Result<void>::Success();
            }

            [[nodiscard]] Result<ShaderCompilerAdapterOutput> CompileNullRoute() {
                std::vector<ToolArtifactRecord> records;
                try {
                    records.reserve(invocation_.manifest.entryPoints.size());
                    for (const ShaderEntryPoint &entry : invocation_.manifest.entryPoints) {
                        std::vector<std::uint8_t> bytes;
                        bytes.insert(bytes.end(), invocation_.artifactKey.bytes.begin(), invocation_.artifactKey.bytes.end());
                        bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
                        records.push_back({entry.stage, entry.name, std::move(bytes)});
                    }
                } catch (const std::bad_alloc &) {
                    return Failure(ShaderCompilerPipelineErrors::ToolOutputInvalid);
                }
                auto payload = PackageStages(invocation_.target.requirement.backend, invocation_.target.requirement.payloadFormat, records,
                                             invocation_.limits.maximumPayloadBytes);
                if (payload.HasError())
                    return Result<ShaderCompilerAdapterOutput>::Failure(std::move(payload).ErrorValue());
                return Result<ShaderCompilerAdapterOutput>::Success({invocation_.target.requirement.backend,
                                                                     invocation_.target.requirement.payloadFormat,
                                                                     std::move(payload).Value(),
                                                                     {},
                                                                     std::move(diagnostics_)});
            }

            [[nodiscard]] Result<ShaderCompilerAdapterOutput> PackageOutput() {
                auto payload = PackageStages(invocation_.target.requirement.backend, invocation_.target.requirement.payloadFormat,
                                             payloadStages_, invocation_.limits.maximumPayloadBytes);
                if (payload.HasError())
                    return Result<ShaderCompilerAdapterOutput>::Failure(std::move(payload).ErrorValue());
                std::vector<std::uint8_t> debugPayload;
                if (!debugStages_.empty()) {
                    auto packagedDebug = PackageStages(invocation_.target.requirement.backend, invocation_.target.requirement.payloadFormat,
                                                       debugStages_, invocation_.limits.maximumDebugPayloadBytes);
                    if (packagedDebug.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(std::move(packagedDebug).ErrorValue());
                    debugPayload = std::move(packagedDebug).Value();
                }
                return Result<ShaderCompilerAdapterOutput>::Success(
                    {invocation_.target.requirement.backend, invocation_.target.requirement.payloadFormat, std::move(payload).Value(),
                     std::move(debugPayload), std::move(diagnostics_)});
            }

            const ShaderCompilerToolchainConfiguration &configuration_;
            IExternalProcessRunner &processes_;
            const ShaderCompilerInvocation &invocation_;
            const CancellationToken &cancellation_;
            const ScratchDirectory &scratch_;
            const std::filesystem::path &sourcePath_;
            std::vector<ToolArtifactRecord> payloadStages_;
            std::vector<ToolArtifactRecord> debugStages_;
            std::vector<ShaderCompilerDiagnostic> diagnostics_;
            std::size_t diagnosticBytes_{0};
        };
    }  // namespace

    struct ExternalShaderCompilerAdapterState final {
        ShaderCompilerToolchainConfiguration configuration;
        std::shared_ptr<IExternalProcessRunner> processes;
        std::atomic_uint64_t nextInvocation{1};
    };

    /** @copydoc ApprovedShaderCompilerTools */
    std::span<const ApprovedShaderCompilerTool> ApprovedShaderCompilerTools() noexcept {
        return ApprovedTools;
    }

    ExternalShaderCompilerAdapter::ExternalShaderCompilerAdapter(std::shared_ptr<ExternalShaderCompilerAdapterState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc ExternalShaderCompilerAdapter::~ExternalShaderCompilerAdapter */
    ExternalShaderCompilerAdapter::~ExternalShaderCompilerAdapter() noexcept = default;

    /** @copydoc ExternalShaderCompilerAdapter::ExternalShaderCompilerAdapter */
    ExternalShaderCompilerAdapter::ExternalShaderCompilerAdapter(ExternalShaderCompilerAdapter &&) noexcept = default;

    /** @copydoc ExternalShaderCompilerAdapter::operator= */
    ExternalShaderCompilerAdapter &ExternalShaderCompilerAdapter::operator=(ExternalShaderCompilerAdapter &&) noexcept = default;

    /** @copydoc ExternalShaderCompilerAdapter::Create */
    Result<ExternalShaderCompilerAdapter> ExternalShaderCompilerAdapter::Create(ShaderCompilerToolchainConfiguration configuration,
                                                                                std::shared_ptr<IExternalProcessRunner> processes) {
        if (!IsSafeIdentity(configuration.hostPlatform) || !configuration.scratchRoot.is_absolute() ||
            configuration.processTimeout <= std::chrono::milliseconds::zero() || configuration.processTimeout > MaximumProcessTimeout ||
            configuration.maximumToolBinaryBytes == 0 || configuration.maximumToolBinaryBytes > HardMaximumToolBinaryBytes ||
            configuration.maximumProcessOutputBytes == 0 || configuration.maximumProcessOutputBytes > HardMaximumProcessOutputBytes ||
            configuration.tools.size() > 5U || processes == nullptr)
            return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid));

        std::error_code error;
        std::filesystem::create_directories(configuration.scratchRoot, error);
        if (error || !std::filesystem::is_directory(configuration.scratchRoot, error))
            return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));

        for (std::size_t index = 0; index < configuration.tools.size(); ++index) {
            const auto &tool = configuration.tools[index];
            if (!tool.executable.is_absolute() || (index > 0 && configuration.tools[index - 1].identity.tool >= tool.identity.tool))
                return Result<ExternalShaderCompilerAdapter>::Failure(
                    MakeError(ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid));
            if (!IsApproved(configuration, tool))
                return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolNotApproved));
            auto executable =
                ReadBoundedFile(tool.executable, configuration.maximumToolBinaryBytes, ShaderCompilerPipelineErrors::ToolMissing);
            if (executable.HasError())
                return Result<ExternalShaderCompilerAdapter>::Failure(std::move(executable).ErrorValue());
            const Sha256Digest actual = ComputeSha256(std::as_bytes(std::span{executable.Value()}));
            if (actual != tool.executableDigest)
                return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolDigestMismatch));
        }

        try {
            auto state = std::make_shared<ExternalShaderCompilerAdapterState>();
            state->configuration = std::move(configuration);
            state->processes = std::move(processes);
            return Result<ExternalShaderCompilerAdapter>::Success(ExternalShaderCompilerAdapter(std::move(state)));
        } catch (const std::bad_alloc &) {
            return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::AllocationFailed));
        }
    }

    /** @copydoc ExternalShaderCompilerAdapter::Compile */
    Result<ShaderCompilerAdapterOutput> ExternalShaderCompilerAdapter::Compile(const ShaderCompilerInvocation &invocation,
                                                                               const CancellationToken &cancellation) const {
        if (!state_)
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid));
        if (cancellation.IsCancellationRequested())
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));

        const std::uint64_t sequence = state_->nextInvocation.fetch_add(1, std::memory_order_relaxed);
        if (sequence == 0)
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        std::filesystem::path scratchPath = state_->configuration.scratchRoot / ("shader-" + std::to_string(sequence));
        std::error_code ioError;
        if (!std::filesystem::create_directory(scratchPath, ioError) || ioError)
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        ScratchDirectory scratch(std::move(scratchPath));
        const std::filesystem::path sourcePath = scratch.Path() / "source.hlsl";
        if (auto written = WriteFile(sourcePath, invocation.source); written.HasError())
            return Result<ShaderCompilerAdapterOutput>::Failure(std::move(written).ErrorValue());
        for (const ShaderCompilerDependency &dependency : invocation.dependencies) {
            const std::filesystem::path dependencyPath = scratch.Path() / dependency.logicalPath;
            std::filesystem::create_directories(dependencyPath.parent_path(), ioError);
            if (ioError)
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
            if (auto written = WriteFile(dependencyPath, dependency.content); written.HasError())
                return Result<ShaderCompilerAdapterOutput>::Failure(std::move(written).ErrorValue());
        }

        return CompilerRouteContext{state_->configuration, *state_->processes, invocation, cancellation, scratch, sourcePath}.Compile();
    }
}  // namespace Horo::Render
