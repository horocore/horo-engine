#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "ShaderCompilerArtifactKey.h"
#include "ShaderValidationSupport.h"

#include <algorithm>
#include <array>
#include <format>
#include <new>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>

namespace Horo::Render {
    namespace {
        using ShaderValidationDetail::SameTargetRequirement;
        constexpr std::size_t HardMaximumSourceBytes = 64U * 1024U * 1024U;
        constexpr std::size_t HardMaximumDependencies = 1'024;
        constexpr std::size_t HardMaximumDependencyBytes = 16U * 1024U * 1024U;
        constexpr std::size_t HardMaximumTotalDependencyBytes = 128U * 1024U * 1024U;
        constexpr std::size_t HardMaximumDefines = 512;
        constexpr std::size_t HardMaximumTargets = 8;
        constexpr std::size_t HardMaximumPayloadBytes = 256U * 1024U * 1024U;
        constexpr std::size_t HardMaximumDiagnosticsPerTarget = 1'024;
        constexpr std::size_t HardMaximumDiagnosticMessageBytes = 64U * 1024U;
        constexpr std::size_t HardMaximumIdentityBytes = 512;

        template <typename ValueT> [[nodiscard]] constexpr bool IsWithinBound(const ValueT value, const ValueT maximum) noexcept {
            return value > 0 && value <= maximum;
        }

        [[nodiscard]] bool IsValidLimits(const ShaderCompilerLimits &limits) noexcept {
            return IsWithinBound(limits.maximumSourceBytes, HardMaximumSourceBytes) &&
                   IsWithinBound(limits.maximumDependencies, HardMaximumDependencies) &&
                   IsWithinBound(limits.maximumDependencyBytes, HardMaximumDependencyBytes) &&
                   IsWithinBound(limits.maximumTotalDependencyBytes, HardMaximumTotalDependencyBytes) &&
                   IsWithinBound(limits.maximumDefines, HardMaximumDefines) && IsWithinBound(limits.maximumTargets, HardMaximumTargets) &&
                   IsWithinBound(limits.maximumPayloadBytes, HardMaximumPayloadBytes) &&
                   IsWithinBound(limits.maximumDebugPayloadBytes, HardMaximumPayloadBytes) &&
                   IsWithinBound(limits.maximumDiagnosticsPerTarget, HardMaximumDiagnosticsPerTarget) &&
                   IsWithinBound(limits.maximumDiagnosticMessageBytes, HardMaximumDiagnosticMessageBytes) &&
                   IsWithinBound(limits.maximumIdentityBytes, HardMaximumIdentityBytes);
        }

        [[nodiscard]] bool HasNonZeroDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const std::uint8_t byte) {
                return byte != 0;
            });
        }

        [[nodiscard]] bool IsValidIdentity(const std::string &value, const std::size_t maximumBytes) noexcept {
            return ShaderValidationDetail::IsValidIdentity(value, maximumBytes, true);
        }

        [[nodiscard]] bool IsValidLogicalPath(const std::string &value, const std::size_t maximumBytes) noexcept {
            if (!IsValidIdentity(value, maximumBytes) || value.front() == '/' || value.back() == '/')
                return false;
            std::size_t segmentStart = 0;
            while (segmentStart < value.size()) {
                if (const std::size_t separator = value.find('/', segmentStart); separator == std::string::npos) {
                    const std::string_view segment{value.data() + segmentStart, value.size() - segmentStart};
                    return !segment.empty() && segment != "." && segment != "..";
                } else {
                    if (const std::string_view segment{value.data() + segmentStart, separator - segmentStart};
                        segment.empty() || segment == "." || segment == "..")
                        return false;
                    segmentStart = separator + 1U;
                }
            }
            return true;
        }

        [[nodiscard]] bool IsValidDefineName(const std::string &value, const std::size_t maximumBytes) noexcept {
            if (value.empty() || value.size() > maximumBytes)
                return false;
            const auto validFirst = [](const unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
            };
            return validFirst(static_cast<unsigned char>(value.front())) && std::ranges::all_of(value, [&](const char character) {
                const auto byte = static_cast<unsigned char>(character);
                return validFirst(byte) || (byte >= '0' && byte <= '9');
            });
        }

        [[nodiscard]] bool IsValidDefineValue(const std::string &value, const std::size_t maximumBytes) noexcept {
            return value.size() <= maximumBytes && std::ranges::all_of(value, [](const char character) {
                const auto byte = static_cast<unsigned char>(character);
                return byte >= 0x20U && byte <= 0x7eU;
            });
        }

        [[nodiscard]] std::span<const ShaderCompilerTool> RequiredTools(const ShaderTargetBackend backend) noexcept {
            using enum ShaderCompilerTool;
            static constexpr std::array<ShaderCompilerTool, 0> NullTools{};
            static constexpr std::array VulkanTools{Dxc, SpirvTools};
            static constexpr std::array OpenGlTools{Dxc, SpirvTools, SpirvCross};
            static constexpr std::array MetalTools{Dxc, SpirvTools, SpirvCross, AppleMetal};
            static constexpr std::array D3d12Tools{Dxc, DxilValidator};
            using enum ShaderTargetBackend;
            switch (backend) {
                case Null:
                    return NullTools;
                case OpenGL:
                    return OpenGlTools;
                case Vulkan:
                    return VulkanTools;
                case Metal:
                    return MetalTools;
                case D3D12:
                    return D3d12Tools;
            }
            return {};
        }

        [[nodiscard]] bool HasCanonicalToolchain(const ShaderCompilerTargetDescriptor &target,
                                                 const ShaderCompilerLimits &limits) noexcept {
            const auto expected = RequiredTools(target.requirement.backend);
            if (target.tools.size() != expected.size())
                return false;
            for (std::size_t index = 0; index < target.tools.size(); ++index) {
                const ShaderCompilerToolIdentity &identity = target.tools[index];
                if (identity.tool != expected[index] || !IsValidIdentity(identity.release, limits.maximumIdentityBytes) ||
                    !HasNonZeroDigest(identity.buildDigest))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool HasSupportedRoute(const ShaderCompilerTargetDescriptor &target) noexcept {
            if (!target.columnMajorMatrices || !target.strictBufferLayout || !target.disableAutomaticDepthRemap)
                return false;
            const auto backend = target.requirement.backend;
            const auto format = target.requirement.payloadFormat;
            const auto environment = target.intermediateEnvironment;
            using enum ShaderTargetBackend;
            using enum ShaderPayloadFormat;
            constexpr auto NoIntermediate = ShaderIntermediateEnvironment::None;
            constexpr auto VulkanIntermediate = ShaderIntermediateEnvironment::Vulkan13SpirV16;
            switch (backend) {
                case Null:
                    return format == ValidationFixture && environment == NoIntermediate;
                case OpenGL:
                    return format == Glsl410 && environment == VulkanIntermediate;
                case Vulkan:
                    return format == SpirV16 && environment == VulkanIntermediate;
                case Metal:
                    return format == MetalLibrary24 && environment == VulkanIntermediate;
                case D3D12:
                    return format == Dxil60 && environment == NoIntermediate;
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateDependencies(const ShaderCompilationRequest &request, const ShaderCompilerLimits &limits) {
            std::size_t totalDependencyBytes = 0;
            for (std::size_t index = 0; index < request.dependencies.size(); ++index) {
                const auto &dependency = request.dependencies[index];
                if (!IsValidLogicalPath(dependency.logicalPath, limits.maximumIdentityBytes) || !HasNonZeroDigest(dependency.digest) ||
                    dependency.content.size() > limits.maximumDependencyBytes ||
                    dependency.content.size() > limits.maximumTotalDependencyBytes - totalDependencyBytes ||
                    ComputeSha256(std::as_bytes(std::span{dependency.content})) != dependency.digest)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
                totalDependencyBytes += dependency.content.size();
                if (index > 0 && request.dependencies[index - 1].logicalPath >= dependency.logicalPath)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::NonCanonicalInput));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDefines(const ShaderCompilationRequest &request, const ShaderCompilerLimits &limits) {
            for (std::size_t index = 0; index < request.defines.size(); ++index) {
                const auto &define = request.defines[index];
                if (!IsValidDefineName(define.name, limits.maximumIdentityBytes) ||
                    !IsValidDefineValue(define.value, limits.maximumIdentityBytes))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
                if (index > 0 && request.defines[index - 1].name >= define.name)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::NonCanonicalInput));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateTargets(const ShaderCompilationRequest &request, const ShaderCompilerLimits &limits) {
            for (std::size_t index = 0; index < request.targets.size(); ++index) {
                const auto &target = request.targets[index];
                if (!SameTargetRequirement(target.requirement, request.manifest.targets[index]) ||
                    !IsValidIdentity(target.platformTriple, limits.maximumIdentityBytes))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
                if (index > 0 && request.targets[index - 1].requirement.backend >= target.requirement.backend)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::NonCanonicalInput));
                if (!HasSupportedRoute(target))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::UnsupportedTarget));
                if (!HasCanonicalToolchain(target, limits))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::UnpinnedToolchain));
            }
            return Result<void>::Success();
        }

        /** @brief Bounds explicit authored names before adapter execution or identity serialization. */
        [[nodiscard]] bool ValidReflectionRequest(const ShaderCompilationRequest &request, const ShaderCompilerLimits &limits) {
            if (!request.reflection)
                return true;
            const auto &reflection = *request.reflection;
            if (reflection.tool.tool != ShaderCompilerTool::SpirvCross || !HasNonZeroDigest(reflection.tool.buildDigest) ||
                !IsValidIdentity(reflection.tool.release, limits.maximumIdentityBytes) || reflection.names.size() > 128 ||
                reflection.names.size() != request.manifest.bindings.size())
                return false;
            std::size_t parameters = 0;
            for (const auto &binding : reflection.names) {
                if (!binding.id.IsValid() || !IsValidIdentity(binding.name, limits.maximumIdentityBytes) ||
                    binding.parameters.size() > 512 - parameters ||
                    std::ranges::count(reflection.names, binding.id, &ShaderReflectionBindingName::id) != 1 ||
                    std::ranges::count(reflection.names, binding.name, &ShaderReflectionBindingName::name) != 1 ||
                    std::ranges::find(request.manifest.bindings, binding.id, &ShaderResourceBinding::id) ==
                        request.manifest.bindings.end() ||
                    binding.parameters.size() !=
                        static_cast<std::size_t>(std::ranges::count(request.manifest.parameters, binding.id, &ShaderParameter::binding)))
                    return false;
                parameters += binding.parameters.size();
                for (const auto &parameter : binding.parameters) {
                    const auto declared = std::ranges::find(request.manifest.parameters, parameter.id, &ShaderParameter::id);
                    if (!parameter.id.IsValid() || !IsValidIdentity(parameter.name, limits.maximumIdentityBytes) ||
                        std::ranges::count(binding.parameters, parameter.id, &ShaderReflectionParameterName::id) != 1 ||
                        std::ranges::count(binding.parameters, parameter.name, &ShaderReflectionParameterName::name) != 1 ||
                        declared == request.manifest.parameters.end() || declared->binding != binding.id)
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] Result<void> ValidateRequest(const ShaderCompilationRequest &request, const ShaderCompilerLimits &limits) {
            if (request.source.empty() || request.source.size() > limits.maximumSourceBytes || request.targets.empty() ||
                request.targets.size() > limits.maximumTargets || request.targets.size() != request.manifest.targets.size() ||
                request.dependencies.size() > limits.maximumDependencies || request.defines.size() > limits.maximumDefines ||
                !ValidReflectionRequest(request, limits))
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));

            if (const auto manifestResult = ValidateShaderManifest(request.manifest); manifestResult.HasError())
                return Result<void>::Failure(WrapError(ShaderCompilerPipelineErrors::InvalidRequest, manifestResult.ErrorValue()));
            if (auto result = ValidateDependencies(request, limits); result.HasError())
                return result;
            if (auto result = ValidateDefines(request, limits); result.HasError())
                return result;
            return ValidateTargets(request, limits);
        }

        /** @brief Rejects fabricated or incomplete source coordinates independently of diagnostic text. */
        [[nodiscard]] bool IsValidDiagnosticLocation(const ShaderCompilerDiagnostic &diagnostic,
                                                     const ShaderCompilerLimits &limits) noexcept {
            return (diagnostic.sourceIdentity.empty() || IsValidLogicalPath(diagnostic.sourceIdentity, limits.maximumIdentityBytes)) &&
                   (diagnostic.line != 0 || diagnostic.column == 0) &&
                   (!diagnostic.sourceIdentity.empty() || (diagnostic.line == 0 && diagnostic.column == 0));
        }

        [[nodiscard]] bool IsValidDiagnostic(const ShaderCompilerDiagnostic &diagnostic, const ShaderCompilerLimits &limits) noexcept {
            const auto category = static_cast<std::underlying_type_t<ShaderCompilerDiagnosticCategory>>(diagnostic.category);
            const auto severity = static_cast<std::underlying_type_t<ShaderCompilerDiagnosticSeverity>>(diagnostic.severity);
            return category <= static_cast<std::underlying_type_t<ShaderCompilerDiagnosticCategory>>(
                                   ShaderCompilerDiagnosticCategory::Validation) &&
                   severity <=
                       static_cast<std::underlying_type_t<ShaderCompilerDiagnosticSeverity>>(ShaderCompilerDiagnosticSeverity::Error) &&
                   IsValidDiagnosticLocation(diagnostic, limits) && !diagnostic.message.empty() &&
                   diagnostic.message.size() <= limits.maximumDiagnosticMessageBytes &&
                   (!diagnostic.toolCode || IsValidIdentity(*diagnostic.toolCode, limits.maximumIdentityBytes));
        }

        /** @brief Matches supplied stage/entry provenance to the admitted manifest. */
        [[nodiscard]] bool HasMatchingEntry(const ShaderCompilerDiagnosticContext &context, const ShaderManifest &manifest) {
            if (!context.shaderStage)
                return context.entryPoint.empty();
            return std::ranges::any_of(manifest.entryPoints, [&](const auto &entry) {
                return entry.stage == *context.shaderStage && entry.name == context.entryPoint;
            });
        }

        /** @brief Checks the exact tool role, release and digest against the host-admitted target. */
        [[nodiscard]] bool HasMatchingTool(const ShaderCompilerDiagnosticContext &context, const ShaderCompilerTargetDescriptor &target) {
            return !context.tool || std::ranges::any_of(target.tools, [&](const auto &tool) {
                return tool.tool == context.tool->tool && tool.release == context.tool->release &&
                       tool.buildDigest == context.tool->buildDigest;
            });
        }

        /** @brief Validates provenance against the admitted invocation rather than trusting streamed adapter metadata. */
        [[nodiscard]] bool IsValidContext(const ShaderCompilerDiagnosticContext &context, const ShaderManifest &manifest,
                                          const ShaderCompilerTargetDescriptor &target, const ShaderCompilerLimits &limits) {
            if (static_cast<unsigned>(context.phase) > static_cast<unsigned>(ShaderCompilerPhase::DebugCompilation) ||
                context.backend != target.requirement.backend || context.sourceRevision != manifest.sourceRevision ||
                (!context.entryPoint.empty() && !IsValidIdentity(context.entryPoint, limits.maximumIdentityBytes)))
                return false;
            return HasMatchingEntry(context, manifest) && HasMatchingTool(context, target);
        }

        /** @brief Validates and bounds live output before it can reach the host store, including failed adapter routes. */
        class ValidatingSink final : public IShaderCompilerDiagnosticSink {
        public:
            ValidatingSink(IShaderCompilerDiagnosticSink &host, const ShaderManifest &manifest,
                           const ShaderCompilerTargetDescriptor &target, const ShaderCompilerLimits &limits)
                : host_(host), manifest_(manifest), target_(target), limits_(limits) {}

            Result<void> BeginPhase(const ShaderCompilerDiagnosticContext &context) override {
                if (failure_)
                    return Result<void>::Failure(*failure_);
                // Each entry has at most seven compiler phases; repeated/unbounded adapter callbacks are rejected.
                if (++phases_ > 7U * manifest_.entryPoints.size() + 1U || !IsValidContext(context, manifest_, target_, limits_))
                    return Reject();
                return Remember(host_.BeginPhase(context));
            }

            Result<void> Publish(const ShaderCompilerDiagnostic &diagnostic) override {
                if (failure_)
                    return Result<void>::Failure(*failure_);
                if (published_ >= limits_.maximumDiagnosticsPerTarget || !IsValidDiagnostic(diagnostic, limits_) ||
                    (diagnostic.context && !IsValidContext(*diagnostic.context, manifest_, target_, limits_)))
                    return Reject();
                ++published_;
                return Remember(host_.Publish(diagnostic));
            }

            [[nodiscard]] std::size_t Published() const noexcept {
                return published_;
            }

            [[nodiscard]] const std::optional<Error> &Failure() const noexcept {
                return failure_;
            }

        private:
            Result<void> Reject() {
                return Remember(Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidAdapterOutput)));
            }

            Result<void> Remember(const Result<void> &result) {
                if (!failure_ && result.HasError())
                    failure_ = result.ErrorValue();
                return failure_ ? Result<void>::Failure(*failure_) : result;
            }

            IShaderCompilerDiagnosticSink &host_;
            const ShaderManifest &manifest_;
            const ShaderCompilerTargetDescriptor &target_;
            const ShaderCompilerLimits &limits_;
            std::size_t published_{};
            std::size_t phases_{};
            std::optional<Error> failure_;
        };

        [[nodiscard]] bool IsValidOutput(const ShaderCompilerAdapterOutput &output, const ShaderCompilerTargetDescriptor &target,
                                         const ShaderCompilerLimits &limits) noexcept {
            return output.backend == target.requirement.backend && output.payloadFormat == target.requirement.payloadFormat &&
                   !output.payload.empty() && output.payload.size() <= limits.maximumPayloadBytes &&
                   output.debugPayload.size() <= limits.maximumDebugPayloadBytes &&
                   (target.emitDebugInformation || output.debugPayload.empty()) &&
                   output.diagnostics.size() <= limits.maximumDiagnosticsPerTarget &&
                   std::ranges::all_of(output.diagnostics, [&](const ShaderCompilerDiagnostic &diagnostic) {
                return IsValidDiagnostic(diagnostic, limits);
            });
        }

        [[nodiscard]] Result<ShaderCompilerAdapterOutput> InvokeAdapter(const IShaderCompilerAdapter &adapter,
                                                                        const ShaderCompilerInvocation &invocation,
                                                                        const CancellationToken &cancellation) {
            const std::string context = std::format("Shader compiler adapter failed target backend {}.",
                                                    static_cast<std::uint8_t>(invocation.target.requirement.backend));
            try {
                auto output = adapter.Compile(invocation, cancellation);
                if (output.HasError())
                    return Result<ShaderCompilerAdapterOutput>::Failure(
                        WrapError(ShaderCompilerPipelineErrors::AdapterFailure, std::move(output).ErrorValue(), context));
                return output;
            } catch (const std::bad_alloc &) {
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::AllocationFailed));
            } catch (...) {  // NOSONAR(cpp:S1181) Private/native toolchain adapters must not leak exceptions across the typed API.
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::AdapterFailure, context));
            }
        }

        /** @brief Checks returned diagnostics against the same bounds and provenance enforced on live output. */
        [[nodiscard]] bool IsValidReturnedOutput(const ShaderCompilerAdapterOutput &output, const ShaderCompilationRequest &request,
                                                 const ShaderCompilerTargetDescriptor &target, const ShaderCompilerLimits &limits,
                                                 const std::optional<ValidatingSink> &stream) {
            if (!IsValidOutput(output, target, limits))
                return false;
            if (stream && stream->Published() != 0 && stream->Published() != output.diagnostics.size())
                return false;
            return std::ranges::all_of(output.diagnostics, [&](const auto &diagnostic) {
                return !diagnostic.context || IsValidContext(*diagnostic.context, request.manifest, target, limits);
            });
        }

        /** @brief Forwards owned output only when the adapter did not publish a live sequence. */
        [[nodiscard]] Result<void> ForwardOwnedDiagnostics(const ShaderCompilerAdapterOutput &output,
                                                           std::optional<ValidatingSink> &stream) {
            if (!stream || stream->Published() != 0)
                return Result<void>::Success();
            for (const auto &diagnostic : output.diagnostics)
                if (auto published = stream->Publish(diagnostic); published.HasError())
                    return published;
            return Result<void>::Success();
        }

        /** @brief Preserves sink rejection, adapter failures and cancellation before admitting a returned candidate. */
        [[nodiscard]] Result<ShaderCompilerAdapterOutput> AdmitAdapterOutput(Result<ShaderCompilerAdapterOutput> output,
                                                                             const ShaderCompilationRequest &request,
                                                                             const ShaderCompilerTargetDescriptor &target,
                                                                             const ShaderCompilerLimits &limits,
                                                                             const CancellationToken &cancellation,
                                                                             std::optional<ValidatingSink> &stream) {
            if (stream && stream->Failure())
                return Result<ShaderCompilerAdapterOutput>::Failure(*stream->Failure());
            if (output.HasError())
                return output;
            if (cancellation.IsCancellationRequested())
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
            if (!IsValidReturnedOutput(output.Value(), request, target, limits, stream))
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidAdapterOutput));
            if (auto forwarded = ForwardOwnedDiagnostics(output.Value(), stream); forwarded.HasError())
                return Result<ShaderCompilerAdapterOutput>::Failure(forwarded.ErrorValue());
            return output;
        }

        /** @brief Begins validation of a selected target through the optional host stream. */
        [[nodiscard]] Result<void> BeginTarget(std::optional<ValidatingSink> &stream, const ShaderCompilationRequest &request,
                                               const ShaderCompilerTargetDescriptor &target) {
            if (!stream)
                return Result<void>::Success();
            return stream->BeginPhase(
                {ShaderCompilerPhase::PipelineValidation, target.requirement.backend, request.manifest.sourceRevision, {}, {}, {}});
        }

        /** @brief Rejects missing or foreign cook records before publishing the immutable target artifact. */
        [[nodiscard]] bool ValidReflectedOutput(const CompiledShaderArtifact &output, const ShaderCompilationRequest &request,
                                                const ShaderCompilerTargetDescriptor &target) {
            if (!request.reflection)
                return !output.reflection;
            if (!output.reflection)
                return false;
            const auto &record = *output.reflection;
            const auto &tool = request.reflection->tool;
            if (ValidateReflectedShaderArtifact(output, record).HasError() || record.reflectionTool.tool != tool.tool ||
                record.reflectionTool.release != tool.release || record.reflectionTool.buildDigest != tool.buildDigest ||
                !HasNonZeroDigest(record.cookedArtifactKey))
                return false;
            const auto &reflection = record.reflection;
            const ShaderReflectionCandidate candidate{reflection.backend,    reflection.interfaceSchemaVersion, reflection.bindings,
                                                      reflection.parameters, reflection.stageInterface,         reflection.targetBindings,
                                                      reflection.sourceMap};
            const auto normalized = NormalizeShaderReflection(request.manifest, target.requirement, candidate);
            return !normalized.HasError() && normalized.Value().interfaceCompatibility == reflection.interfaceCompatibility;
        }

        /** @brief Owns one target key, borrowed stream and candidate until its complete output validates. */
        [[nodiscard]] Result<CompiledShaderArtifact> CompileTarget(const ShaderCompilationRequest &request,
                                                                   const ShaderCompilerTargetDescriptor &target,
                                                                   const IShaderCompilerAdapter &adapter,
                                                                   const CancellationToken &cancellation,
                                                                   const ShaderCompilerLimits &limits, const Sha256Digest &sourceDigest,
                                                                   IShaderCompilerDiagnosticSink *diagnostics) {
            auto key = ShaderCompilerDetail::BuildArtifactKey(request, target, sourceDigest);
            if (key.HasError())
                return Result<CompiledShaderArtifact>::Failure(key.ErrorValue());
            std::optional<ValidatingSink> stream;
            if (diagnostics)
                stream.emplace(*diagnostics, request.manifest, target, limits);
            if (auto started = BeginTarget(stream, request, target); started.HasError())
                return Result<CompiledShaderArtifact>::Failure(started.ErrorValue());
            const ShaderCompilerInvocation invocation{request.manifest,
                                                      request.source,
                                                      request.dependencies,
                                                      request.defines,
                                                      target,
                                                      key.Value(),
                                                      limits,
                                                      stream ? &*stream : nullptr,
                                                      request.reflection ? &*request.reflection : nullptr};
            auto output =
                AdmitAdapterOutput(InvokeAdapter(adapter, invocation, cancellation), request, target, limits, cancellation, stream);
            if (output.HasError())
                return Result<CompiledShaderArtifact>::Failure(std::move(output).ErrorValue());
            auto value = std::move(output).Value();
            CompiledShaderArtifact artifact{value.backend,
                                            value.payloadFormat,
                                            key.Value(),
                                            std::move(value.payload),
                                            std::move(value.debugPayload),
                                            std::move(value.diagnostics),
                                            std::move(value.reflection)};
            if (!ValidReflectedOutput(artifact, request, target))
                return Result<CompiledShaderArtifact>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidAdapterOutput));
            return Result<CompiledShaderArtifact>::Success(std::move(artifact));
        }
    }  // namespace

    /** @copydoc CompileShaderTargets */
    Result<ShaderCompilationBatch> CompileShaderTargets(const ShaderCompilationRequest &request, const IShaderCompilerAdapter &adapter,
                                                        const CancellationToken &cancellation, const ShaderCompilerLimits &limits,
                                                        IShaderCompilerDiagnosticSink *diagnostics) {
        if (!IsValidLimits(limits))
            return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidLimits));
        if (const auto validation = ValidateRequest(request, limits); validation.HasError())
            return Result<ShaderCompilationBatch>::Failure(validation.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));

        const Sha256Digest sourceDigest = ComputeSha256(std::as_bytes(std::span{request.source}));
        try {
            ShaderCompilationBatch batch{sourceDigest, {}, {}};
            batch.dependencies.reserve(request.dependencies.size());
            for (const ShaderCompilerDependency &dependency : request.dependencies)
                batch.dependencies.emplace_back(dependency.logicalPath, dependency.digest);
            batch.artifacts.reserve(request.targets.size());
            for (const ShaderCompilerTargetDescriptor &target : request.targets) {
                if (cancellation.IsCancellationRequested())
                    return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
                auto artifact = CompileTarget(request, target, adapter, cancellation, limits, sourceDigest, diagnostics);
                if (artifact.HasError())
                    return Result<ShaderCompilationBatch>::Failure(std::move(artifact).ErrorValue());
                batch.artifacts.push_back(std::move(artifact).Value());
            }
            return Result<ShaderCompilationBatch>::Success(std::move(batch));
        } catch (const std::bad_alloc &) {
            return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
