#include "ShaderCompilerRoute.h"

#include "ExposureGpuReflection.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "LightCullingMetalReflection.h"
#include "ShaderCompilerToolchainSupport.h"
#include "VfxGpuReflection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Render::ShaderCompilerToolchainDetail {
    namespace {
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

        [[nodiscard]] bool IsMetalLibrary(const std::span<const std::uint8_t> bytes) noexcept {
            constexpr std::array<std::uint8_t, 4> magic{'M', 'T', 'L', 'B'};
            return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
        }

        class CompilerRouteContext final {
        public:
            CompilerRouteContext(const ShaderCompilerToolchainConfiguration &configuration, IExternalProcessRunner &processes,
                                 const ShaderCompilerInvocation &invocation, const CancellationToken &cancellation,
                                 const std::filesystem::path &scratch, const std::filesystem::path &sourcePath)
                : configuration_(configuration), processes_(processes), invocation_(invocation), cancellation_(cancellation),
                  scratch_(scratch), sourcePath_(sourcePath) {}

            [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile() {
                if (invocation_.target.requirement.backend == ShaderTargetBackend::Null)
                    return CompileNullRoute();

                const ShaderCompilerToolInstallation *dxc = Lookup(ShaderCompilerTool::Dxc);
                if (dxc == nullptr)
                    return Failure(ShaderCompilerPipelineErrors::ToolMissing);

                try {
                    const auto admitted = AdmitRasterCookInterface(invocation_);
                    if (admitted.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(admitted.ErrorValue());
                    vfxInterface_ = admitted.Value();
                    registerInterface_ = UsesRegisterRasterReflection(invocation_);
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
                return installed == configuration_.tools.end() ? nullptr : std::to_address(installed);
            }

            [[nodiscard]] Result<void> Run(const ShaderCompilerToolInstallation &tool, std::vector<std::string> arguments,
                                           const ShaderCompilerPhase phase, const ShaderEntryPoint &entry) {
                if (cancellation_.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
                diagnosticContext_ = {phase,
                                      invocation_.target.requirement.backend,
                                      invocation_.manifest.sourceRevision,
                                      entry.stage,
                                      entry.name,
                                      tool.identity};
                if (invocation_.diagnostics != nullptr) {
                    if (auto admitted = invocation_.diagnostics->BeginPhase(diagnosticContext_); admitted.HasError())
                        return admitted;
                }
                ExternalProcessRequest request;
                request.executable = tool.executable.string();
                request.arguments = std::move(arguments);
                request.workingDirectory = scratch_;
                request.environment.base = ProcessEnvironmentBase::Replace;
                request.timeout = configuration_.processTimeout;
                request.maximumLineBytes =
                    std::min(invocation_.limits.maximumDiagnosticMessageBytes, configuration_.maximumProcessOutputBytes);
                request.onOutput = [this](ProcessOutputLine line) {
                    CaptureDiagnostic(std::move(line));
                };
                auto result = processes_.Run(request, cancellation_);
                return CompleteProcess(std::move(result));
            }

            /** @brief Preserve diagnostic rejection before interpreting process completion. */
            [[nodiscard]] Result<void> CompleteProcess(Result<ExternalProcessResult> result) {
                using enum ProcessTerminationReason;
                if (diagnosticFailure_)
                    return Result<void>::Failure(std::move(*diagnosticFailure_));
                if (result.HasError())
                    return Result<void>::Failure(
                        WrapError(ShaderCompilerPipelineErrors::ToolProcessFailed, std::move(result).ErrorValue()));
                if (result.Value().reason == Cancelled || cancellation_.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
                if (result.Value().reason == TimedOut)
                    return Result<void>::Failure(WrapError(ShaderCompilerPipelineErrors::ToolTimedOut, ProcessFailure()));
                if (result.Value().reason == Exited && result.Value().exitCode == 0)
                    return Result<void>::Success();
                return Result<void>::Failure(ProcessFailure());
            }

            struct DiagnosticOrigin {
                std::filesystem::path path;
                std::string identity;
                bool include{};
            };

            /** @brief Resolve only declared source and include identities within the invocation scratch directory. */
            [[nodiscard]] DiagnosticOrigin ResolveDiagnosticOrigin(const std::string_view text) const {
                DiagnosticOrigin origin{sourcePath_, invocation_.manifest.sourceIdentity, false};
                for (const auto &dependency : invocation_.dependencies) {
                    const auto path = scratch_ / dependency.logicalPath;
                    if (text.starts_with(path.string() + ":") || text.starts_with(dependency.logicalPath + ":")) {
                        origin.path = path;
                        origin.identity = dependency.logicalPath;
                        origin.include = true;
                        break;
                    }
                }
                return origin;
            }

            /** @brief Reserve capacity for one visible truncation record before admitting ordinary output. */
            [[nodiscard]] std::optional<std::size_t> DiagnosticOutputBudget() {
                const std::string marker = "Compiler diagnostic output truncated.";
                const std::size_t markerBytes =
                    std::min({marker.size(), invocation_.limits.maximumDiagnosticMessageBytes, configuration_.maximumProcessOutputBytes});
                const std::size_t outputBudget = configuration_.maximumProcessOutputBytes - markerBytes;
                if (diagnostics_.size() + 1U >= invocation_.limits.maximumDiagnosticsPerTarget || diagnosticBytes_ >= outputBudget) {
                    outputTruncated_ = true;
                    ShaderCompilerDiagnostic diagnostic;
                    diagnostic.category = ShaderCompilerDiagnosticCategory::Toolchain;
                    diagnostic.severity = ShaderCompilerDiagnosticSeverity::Warning;
                    diagnostic.message = marker.substr(0, markerBytes);
                    diagnostic.truncated = true;
                    diagnostic.context = diagnosticContext_;
                    PublishDiagnostic(std::move(diagnostic));
                    return std::nullopt;
                }
                return outputBudget;
            }

            void CaptureDiagnostic(ProcessOutputLine line) {
                if (diagnosticFailure_ || outputTruncated_)
                    return;
                const auto outputBudget = DiagnosticOutputBudget();
                if (!outputBudget.has_value())
                    return;
                const auto origin = ResolveDiagnosticOrigin(line.text);
                line.text = SanitizeLine(std::move(line.text), scratch_, origin.path);
                if (line.text.size() > invocation_.limits.maximumDiagnosticMessageBytes) {
                    line.text.resize(invocation_.limits.maximumDiagnosticMessageBytes);
                    line.truncated = true;
                }
                if (const std::size_t remaining = *outputBudget - diagnosticBytes_; line.text.size() > remaining) {
                    line.text.resize(remaining);
                    line.truncated = true;
                }
                diagnosticBytes_ += line.text.size();
                if (line.text.empty())
                    return;
                auto diagnostic = MakeToolDiagnostic(std::move(line.text), line.truncated, origin.identity, line.stream);
                if (origin.include && diagnostic.category == ShaderCompilerDiagnosticCategory::Source)
                    diagnostic.category = ShaderCompilerDiagnosticCategory::Include;
                diagnostic.context = diagnosticContext_;
                PublishDiagnostic(std::move(diagnostic));
            }

            void PublishDiagnostic(ShaderCompilerDiagnostic diagnostic) {
                if (invocation_.diagnostics != nullptr) {
                    if (auto published = invocation_.diagnostics->Publish(diagnostic); published.HasError()) {
                        diagnosticFailure_ = std::move(published).ErrorValue();
                        return;
                    }
                }
                diagnostics_.push_back(std::move(diagnostic));
            }

            [[nodiscard]] Error ProcessFailure() const {
                Error failure = MakeError(ShaderCompilerPipelineErrors::ToolProcessFailed);
                try {
                    for (const ShaderCompilerDiagnostic &diagnostic : diagnostics_) {
                        DiagnosticSeverity severity = DiagnosticSeverity::Note;
                        if (diagnostic.severity == ShaderCompilerDiagnosticSeverity::Error)
                            severity = DiagnosticSeverity::Error;
                        else if (diagnostic.severity == ShaderCompilerDiagnosticSeverity::Warning)
                            severity = DiagnosticSeverity::Warning;
                        failure.diagnostics.emplace_back(
                            Diagnostic{.code = DiagnosticCode{diagnostic.category == ShaderCompilerDiagnosticCategory::Source
                                                                  ? "render.shader_compiler.source"
                                                                  : "render.shader_compiler.toolchain"},
                                       .severity = severity,
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
                arguments.emplace_back(invocation_.target.enableFastMath ? "-ffinite-math-only" : "-Gis");
                std::string_view optimization = "-O3";
                if (invocation_.target.optimization == ShaderOptimizationLevel::Disabled)
                    optimization = "-Od";
                else if (invocation_.target.optimization == ShaderOptimizationLevel::Size)
                    optimization = "-O1";
                arguments.emplace_back(optimization);
                for (const ShaderCompilerDefine &define : invocation_.defines) {
                    arguments.emplace_back("-D");
                    arguments.emplace_back(std::format("{}={}", define.name, define.value));
                }
                if (spirv)
                    arguments.insert(arguments.end(),
                                     {"-spirv", "-fspv-target-env=" + std::string{IntermediateEnvironment()}, "-fvk-use-gl-layout"});
                arguments.insert(arguments.end(), {"-Fo", output.string(), sourcePath_.string()});
                return arguments;
            }

            [[nodiscard]] Result<void> CompileSpirvRoute(const ShaderCompilerToolInstallation &dxc, const ShaderEntryPoint &entry,
                                                         const std::size_t index) {
                const std::string stem = std::format("stage-{}", index);
                const std::filesystem::path spirvPath = scratch_ / (stem + ".spv");
                if (auto compiled = Run(dxc, DxcArguments(entry, true, spirvPath), ShaderCompilerPhase::SourceCompilation, entry);
                    compiled.HasError())
                    return compiled;
                const ShaderCompilerToolInstallation *validator = Lookup(ShaderCompilerTool::SpirvTools);
                if (validator == nullptr)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                if (auto validated = Run(*validator, {"--target-env", std::string{IntermediateEnvironment()}, spirvPath.string()},
                                         ShaderCompilerPhase::IntermediateValidation, entry);
                    validated.HasError())
                    return validated;
                auto spirv =
                    ReadBoundedFile(spirvPath, invocation_.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (spirv.HasError())
                    return Result<void>::Failure(std::move(spirv).ErrorValue());
                if (!IsSpirV(spirv.Value()))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));

                if (auto routed = RouteSpirv(entry, stem, spirvPath, std::move(spirv).Value()); routed.HasError())
                    return routed;
                return CompileDxcDebug(dxc, entry, scratch_ / (stem + ".debug"), true, {"-Zi", "-Qembed_debug"});
            }

            /** @brief Retains exactly the binary fed to Cross for selected readonly input proof; fragment never overwrites vertex evidence.
             */
            bool RetainsVfxSpirv(const ShaderEntryPoint &entry) const {
                if (entry.stage == ShaderStage::Compute)
                    return true;
                return entry.stage == ShaderStage::Vertex && invocation_.reflection &&
                       ValidateVfxGpuMetalReflection(invocation_.manifest, invocation_.target.requirement, invocation_.reflection->names)
                           .HasValue();
            }

            [[nodiscard]] Result<void> RouteSpirv(const ShaderEntryPoint &entry, const std::string &stem,
                                                  const std::filesystem::path &spirvPath, std::vector<std::uint8_t> spirv) {
                using enum ShaderTargetBackend;
                if (invocation_.target.requirement.backend == Vulkan)
                    return StoreVulkan(entry, std::move(spirv));
                if (invocation_.target.requirement.backend == OpenGL)
                    return CompileOpenGLRoute(entry, stem, spirvPath);
                if (invocation_.target.requirement.backend == Metal) {
                    if (RetainsVfxSpirv(entry))
                        validatedReadonlySpirv_ = std::move(spirv);
                    return CompileMetalRoute(entry, stem, spirvPath);
                }
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::UnsupportedTarget));
            }

            [[nodiscard]] Result<void> StoreVulkan(const ShaderEntryPoint &entry, std::vector<std::uint8_t> spirv) {
                payloadStages_.emplace_back(entry.stage, entry.name, std::move(spirv));
                return Result<void>::Success();
            }

            template <typename ValidatorT>
            [[nodiscard]] Result<std::vector<std::uint8_t>> ReadValidatedPayload(const std::filesystem::path &path,
                                                                                 const std::size_t maximumBytes,
                                                                                 ValidatorT validator) const {
                auto payload = ReadBoundedFile(path, maximumBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (payload.HasError())
                    return payload;
                if (!validator(payload.Value()))
                    return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                return payload;
            }

            [[nodiscard]] Result<void> TranslateSpirv(const ShaderEntryPoint &entry, const std::filesystem::path &spirvPath,
                                                      const std::filesystem::path &nativePath, const bool metal) {
                const ShaderCompilerToolInstallation *translator = Lookup(ShaderCompilerTool::SpirvCross);
                if (translator == nullptr)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                auto arguments = RasterTranslationArguments(entry, spirvPath, nativePath, metal, vfxInterface_, registerInterface_);
                if (metal && invocation_.reflection &&
                    ValidateExposureGpuMetalReflection(invocation_.manifest, invocation_.target.requirement, invocation_.reflection->names)
                        .HasValue())
                    arguments.emplace_back("--msl-decoration-binding");
                if (registerInterface_) {
                    if (auto prepared = PrepareRegisterTranslation(entry, spirvPath, metal, arguments); prepared.HasError())
                        return prepared;
                }
                return Run(*translator, std::move(arguments), ShaderCompilerPhase::Translation, entry);
            }

            /** @brief Captures same-operation intermediate evidence once before native translation. */
            Result<void> CaptureIntermediateReflection(const ShaderEntryPoint &entry, const std::filesystem::path &spirvPath) {
                const auto *cross = Lookup(ShaderCompilerTool::SpirvCross);
                if (auto valid =
                        ValidateRasterReflection(invocation_.manifest, invocation_.target.requirement, *invocation_.reflection, cross);
                    valid.HasError())
                    return valid;
                const auto path = spirvPath.string() + ".reflection.json";
                if (auto reflected =
                        Run(*cross, {spirvPath.string(), "--reflect", "--output", path}, ShaderCompilerPhase::Translation, entry);
                    reflected.HasError())
                    return reflected;
                auto json = ReadBoundedFile(path, 1024U * 1024U, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (json.HasError())
                    return Result<void>::Failure(json.ErrorValue());
                reflectionStages_.push_back({entry.stage, entry.name, {json.Value().begin(), json.Value().end()}});
                return Result<void>::Success();
            }

            /** @brief Renames only actual intermediate varying locations so GLSL410 stages link by canonical names. */
            Result<void> PrepareRegisterTranslation(const ShaderEntryPoint &entry, const std::filesystem::path &spirvPath, const bool metal,
                                                    std::vector<std::string> &arguments) {
                if (auto captured = CaptureIntermediateReflection(entry, spirvPath); captured.HasError())
                    return captured;
                if (metal)
                    return Result<void>::Success();
                const auto locations = RegisterRasterVaryingLocations(reflectionStages_.back());
                if (locations.HasError())
                    return Result<void>::Failure(locations.ErrorValue());
                for (const auto location : locations.Value())
                    arguments.insert(arguments.end(), {"--rename-interface-variable", entry.stage == ShaderStage::Vertex ? "out" : "in",
                                                       std::to_string(location), "horo_raster_varying_" + std::to_string(location)});
                return Result<void>::Success();
            }

            /** @brief Uses the explicit descriptor for compilation, validation and debug output without changing other routes. */
            [[nodiscard]] std::string_view IntermediateEnvironment() const noexcept {
                return invocation_.target.intermediateEnvironment == ShaderIntermediateEnvironment::Vulkan12SpirV15 ? "vulkan1.2"
                                                                                                                    : "vulkan1.3";
            }

            [[nodiscard]] Result<void> CompileOpenGLRoute(const ShaderEntryPoint &entry, const std::string &stem,
                                                          const std::filesystem::path &spirvPath) {
                const std::filesystem::path nativePath = scratch_ / (stem + ".native");
                if (auto translated = TranslateSpirv(entry, spirvPath, nativePath, false); translated.HasError())
                    return translated;
                if (auto captured = CaptureNativeReflection(entry, stem, spirvPath, nativePath); captured.HasError())
                    return captured;
                if (auto native = ReadValidatedPayload(nativePath, invocation_.limits.maximumPayloadBytes, IsGlsl410); native.HasError()) {
                    return Result<void>::Failure(std::move(native).ErrorValue());
                } else {
                    payloadStages_.emplace_back(entry.stage, entry.name, std::move(native).Value());
                }
                return Result<void>::Success();
            }

            /** @brief Captures actual locked Cross reflection and native source from the same stage translation before native compilation.
             */
            [[nodiscard]] Result<void> CaptureNativeReflection(const ShaderEntryPoint &entry, const std::string &stem,
                                                               const std::filesystem::path &spirvPath,
                                                               const std::filesystem::path &nativePath) {
                if (!invocation_.reflection)
                    return Result<void>::Success();
                if (registerInterface_) {
                    auto native = ReadBoundedFile(nativePath, 1024U * 1024U, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                    if (native.HasError())
                        return Result<void>::Failure(native.ErrorValue());
                    translations_.emplace_back(native.Value().begin(), native.Value().end());
                    return Result<void>::Success();
                }
                const auto *cross = Lookup(ShaderCompilerTool::SpirvCross);
                if (const auto valid =
                        ValidateRasterReflection(invocation_.manifest, invocation_.target.requirement, *invocation_.reflection, cross);
                    valid.HasError())
                    return valid;
                const auto reflectionPath = scratch_ / (stem + ".reflection.json");
                if (auto reflected = Run(*cross, {spirvPath.string(), "--reflect", "--output", reflectionPath.string()},
                                         ShaderCompilerPhase::Translation, entry);
                    reflected.HasError())
                    return reflected;
                auto json = ReadBoundedFile(reflectionPath, 1024U * 1024U, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                auto nativeSource = ReadBoundedFile(nativePath, 1024U * 1024U, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (json.HasError())
                    return Result<void>::Failure(json.ErrorValue());
                if (nativeSource.HasError())
                    return Result<void>::Failure(nativeSource.ErrorValue());
                reflectionStages_.push_back({entry.stage, entry.name, {json.Value().begin(), json.Value().end()}});
                translations_.emplace_back(nativeSource.Value().begin(), nativeSource.Value().end());
                return Result<void>::Success();
            }

            /** @brief Qualifies the exact canonical exposure input only after validated binary and native ABI proof. */
            [[nodiscard]] Result<void> PrepareExposureMetalSource(const std::filesystem::path &nativePath,
                                                                  std::span<const std::uint8_t> native) {
                const std::string source(native.begin(), native.end());
                auto qualified = QualifyExposureGpuMetalInputs(invocation_.manifest, source, validatedReadonlySpirv_);
                if (qualified.HasError())
                    return Result<void>::Failure(qualified.ErrorValue());
                const auto &text = qualified.Value();
                return WriteFile(nativePath, {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
            }

            /** @brief Preserves ordinary routes and qualifies only proven canonical compute inputs before actual Apple compilation. */
            [[nodiscard]] Result<void> PrepareMetalSource(const ShaderEntryPoint &entry, const std::filesystem::path &nativePath) {
                auto native =
                    ReadBoundedFile(nativePath, invocation_.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (native.HasError())
                    return Result<void>::Failure(native.ErrorValue());
                if (!invocation_.reflection)
                    return Result<void>::Success();
                if (ValidateExposureGpuMetalReflection(invocation_.manifest, invocation_.target.requirement, invocation_.reflection->names)
                        .HasValue())
                    return PrepareExposureMetalSource(nativePath, native.Value());
                if (ValidateVfxGpuMetalReflection(invocation_.manifest, invocation_.target.requirement, invocation_.reflection->names)
                        .HasValue()) {
                    const std::string source(native.Value().begin(), native.Value().end());
                    auto qualified = QualifyVfxGpuMetalInputs(invocation_.manifest, entry, source, validatedReadonlySpirv_);
                    if (qualified.HasError())
                        return Result<void>::Failure(qualified.ErrorValue());
                    const auto &text = qualified.Value();
                    return WriteFile(nativePath, {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
                }
                if (ValidateLightCullingMetalReflection(invocation_.manifest, invocation_.target.requirement, invocation_.reflection->names)
                        .HasError())
                    return Result<void>::Success();
                const std::string source(native.Value().begin(), native.Value().end());
                auto qualified = QualifyLightCullingMetalInputs(source, validatedReadonlySpirv_);
                if (qualified.HasError())
                    return Result<void>::Failure(qualified.ErrorValue());
                const auto &text = qualified.Value();
                return WriteFile(nativePath, {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
            }

            [[nodiscard]] Result<void> CompileMetalRoute(const ShaderEntryPoint &entry, const std::string &stem,
                                                         const std::filesystem::path &spirvPath) {
                const ShaderCompilerToolInstallation *metal = Lookup(ShaderCompilerTool::AppleMetal);
                if (metal == nullptr)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                const std::filesystem::path nativePath = scratch_ / (stem + ".native");
                if (auto translated = TranslateSpirv(entry, spirvPath, nativePath, true); translated.HasError())
                    return translated;
                if (auto prepared = PrepareMetalSource(entry, nativePath); prepared.HasError())
                    return prepared;
                if (auto reflected = CaptureNativeReflection(entry, stem, spirvPath, nativePath); reflected.HasError())
                    return reflected;
                const std::filesystem::path airPath = scratch_ / (stem + ".air");
                if (auto compiled = Run(*metal,
                                        {"-sdk", "macosx", "metal", "-std=macos-metal2.4", "-mmacosx-version-min=14.0",
                                         invocation_.target.enableFastMath ? "-ffast-math" : "-fno-fast-math", "-x", "metal", "-c",
                                         nativePath.string(), "-o", airPath.string()},
                                        ShaderCompilerPhase::NativeCompilation, entry);
                    compiled.HasError())
                    return compiled;
                const std::filesystem::path libraryPath = scratch_ / (stem + ".metallib");
                if (auto linked = Run(*metal, {"-sdk", "macosx", "metallib", airPath.string(), "-o", libraryPath.string()},
                                      ShaderCompilerPhase::NativeLink, entry);
                    linked.HasError())
                    return linked;
                auto library = ReadValidatedPayload(libraryPath, invocation_.limits.maximumPayloadBytes, IsMetalLibrary);
                if (library.HasError())
                    return Result<void>::Failure(std::move(library).ErrorValue());
                payloadStages_.emplace_back(entry.stage, entry.name, std::move(library).Value());
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> CompileDxcDebug(const ShaderCompilerToolInstallation &dxc, const ShaderEntryPoint &entry,
                                                       const std::filesystem::path &debugPath, const bool spirv,
                                                       std::vector<std::string> debugArguments) {
                if (!invocation_.target.emitDebugInformation)
                    return Result<void>::Success();
                std::vector<std::string> arguments = DxcArguments(entry, spirv, debugPath);
                if (const auto optimization = std::ranges::find_if(arguments,
                                                                   [](const std::string_view argument) {
                    return argument == "-O1" || argument == "-O3";
                });
                    optimization != arguments.end())
                    *optimization = "-Od";
                arguments.insert(arguments.end() - 3, std::make_move_iterator(debugArguments.begin()),
                                 std::make_move_iterator(debugArguments.end()));
                if (auto compiled = Run(dxc, std::move(arguments), ShaderCompilerPhase::DebugCompilation, entry); compiled.HasError())
                    return compiled;
                auto debug = spirv ? ReadValidatedPayload(debugPath, invocation_.limits.maximumDebugPayloadBytes, IsSpirV)
                                   : ReadBoundedFile(debugPath, invocation_.limits.maximumDebugPayloadBytes,
                                                     ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (debug.HasError())
                    return Result<void>::Failure(std::move(debug).ErrorValue());
                debugStages_.emplace_back(entry.stage, entry.name, std::move(debug).Value());
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> CompileD3D12Route(const ShaderCompilerToolInstallation &dxc, const ShaderEntryPoint &entry,
                                                         const std::size_t index) {
                const std::string stem = std::format("stage-{}", index);
                const std::filesystem::path nativePath = scratch_ / (stem + ".native");
                if (auto compiled = Run(dxc, DxcArguments(entry, false, nativePath), ShaderCompilerPhase::SourceCompilation, entry);
                    compiled.HasError())
                    return compiled;
                const ShaderCompilerToolInstallation *validator = Lookup(ShaderCompilerTool::DxilValidator);
                if (validator == nullptr)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                if (auto validated = Run(*validator, {nativePath.string()}, ShaderCompilerPhase::IntermediateValidation, entry);
                    validated.HasError())
                    return validated;
                auto native = ReadValidatedPayload(nativePath, invocation_.limits.maximumPayloadBytes, IsDxil);
                if (native.HasError())
                    return Result<void>::Failure(std::move(native).ErrorValue());
                payloadStages_.emplace_back(entry.stage, entry.name, std::move(native).Value());
                const std::filesystem::path debugPath = scratch_ / (stem + ".debug.dxil");
                const std::filesystem::path pdbPath = scratch_ / (stem + ".pdb");
                return CompileDxcDebug(dxc, entry, debugPath, false, {"-Zi", "-Fd", pdbPath.string()});
            }

            [[nodiscard]] Result<ShaderCompilerAdapterOutput> CompileNullRoute() {
                std::vector<ToolArtifactRecord> records;
                try {
                    records.reserve(invocation_.manifest.entryPoints.size());
                    for (const ShaderEntryPoint &entry : invocation_.manifest.entryPoints) {
                        std::vector<std::uint8_t> bytes;
                        bytes.insert(bytes.end(), invocation_.artifactKey.bytes.begin(), invocation_.artifactKey.bytes.end());
                        bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
                        records.emplace_back(entry.stage, entry.name, std::move(bytes));
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
                ShaderCompilerAdapterOutput output{invocation_.target.requirement.backend, invocation_.target.requirement.payloadFormat,
                                                   std::move(payload).Value(), std::move(debugPayload), std::move(diagnostics_)};
                if (invocation_.reflection &&
                    (output.backend == ShaderTargetBackend::Metal || output.backend == ShaderTargetBackend::OpenGL)) {
                    auto reflected =
                        NormalizeCapturedRasterReflection(invocation_, reflectionStages_, translations_, validatedReadonlySpirv_);
                    if (reflected.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(reflected.ErrorValue());
                    CompiledShaderArtifact compiled{output.backend,
                                                    output.payloadFormat,
                                                    invocation_.artifactKey,
                                                    std::move(output.payload),
                                                    {},
                                                    {}};
                    auto cooked = CookReflectedShaderArtifact(compiled, invocation_.reflection->tool, std::move(reflected).Value());
                    output.payload = std::move(compiled.payload);
                    if (cooked.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(cooked.ErrorValue());
                    output.reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(cooked).Value());
                }
                return Result<ShaderCompilerAdapterOutput>::Success(std::move(output));
            }

            const ShaderCompilerToolchainConfiguration &configuration_;
            IExternalProcessRunner &processes_;
            const ShaderCompilerInvocation &invocation_;
            bool vfxInterface_{false};
            bool registerInterface_{};
            const CancellationToken &cancellation_;
            const std::filesystem::path &scratch_;
            const std::filesystem::path &sourcePath_;
            std::vector<ToolArtifactRecord> payloadStages_;
            std::vector<ToolArtifactRecord> debugStages_;
            std::vector<ShaderCompilerDiagnostic> diagnostics_;
            std::vector<SpirvReflectionStage> reflectionStages_;
            std::vector<std::string> translations_;
            std::vector<std::uint8_t> validatedReadonlySpirv_;
            ShaderCompilerDiagnosticContext diagnosticContext_;
            bool outputTruncated_{};
            std::optional<Error> diagnosticFailure_;
            std::size_t diagnosticBytes_{0};
        };
    }  // namespace

    /** @copydoc CompileRoute */
    Result<ShaderCompilerAdapterOutput> CompileRoute(const ShaderCompilerToolchainConfiguration &configuration,
                                                     IExternalProcessRunner &processes, const ShaderCompilerInvocation &invocation,
                                                     const CancellationToken &cancellation, const std::filesystem::path &scratch,
                                                     const std::filesystem::path &sourcePath) {
        return CompilerRouteContext{configuration, processes, invocation, cancellation, scratch, sourcePath}.Compile();
    }
}  // namespace Horo::Render::ShaderCompilerToolchainDetail
