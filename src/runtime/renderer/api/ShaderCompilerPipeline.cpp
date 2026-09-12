#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <algorithm>
#include <array>
#include <new>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>

namespace Horo::Render {
    namespace {
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

        [[nodiscard]] bool IsIdentityCharacter(const unsigned char value) noexcept {
            const bool alpha = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
            const bool digit = value >= '0' && value <= '9';
            return alpha || digit || value == '.' || value == '_' || value == '-' || value == '/' || value == '+';
        }

        [[nodiscard]] bool IsValidIdentity(const std::string &value, const std::size_t maximumBytes) noexcept {
            return !value.empty() && value.size() <= maximumBytes && std::ranges::all_of(value, [](const char character) {
                return IsIdentityCharacter(static_cast<unsigned char>(character));
            });
        }

        [[nodiscard]] bool IsValidLogicalPath(const std::string &value, const std::size_t maximumBytes) noexcept {
            if (!IsValidIdentity(value, maximumBytes) || value.front() == '/' || value.back() == '/')
                return false;
            std::size_t segmentStart = 0;
            while (segmentStart < value.size()) {
                const std::size_t separator = value.find('/', segmentStart);
                const std::string_view segment{value.data() + segmentStart,
                                               (separator == std::string::npos ? value.size() : separator) - segmentStart};
                if (segment.empty() || segment == "." || segment == "..")
                    return false;
                if (separator == std::string::npos)
                    break;
                segmentStart = separator + 1U;
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

        [[nodiscard]] constexpr bool SameRequirement(const ShaderTargetRequirement &left, const ShaderTargetRequirement &right) noexcept {
            return left.backend == right.backend && left.payloadFormat == right.payloadFormat &&
                   left.descriptorVersion == right.descriptorVersion && left.interfaceSchemaVersion == right.interfaceSchemaVersion &&
                   left.maximumBindings == right.maximumBindings && left.maximumInlineConstantBytes == right.maximumInlineConstantBytes &&
                   left.supportsCompute == right.supportsCompute && left.supportsStorageResources == right.supportsStorageResources;
        }

        [[nodiscard]] std::span<const ShaderCompilerTool> RequiredTools(const ShaderTargetBackend backend) noexcept {
            static constexpr std::array<ShaderCompilerTool, 0> NullTools{};
            static constexpr std::array VulkanTools{ShaderCompilerTool::Dxc, ShaderCompilerTool::SpirvTools};
            static constexpr std::array OpenGlTools{ShaderCompilerTool::Dxc, ShaderCompilerTool::SpirvTools,
                                                    ShaderCompilerTool::SpirvCross};
            static constexpr std::array MetalTools{ShaderCompilerTool::Dxc, ShaderCompilerTool::SpirvTools, ShaderCompilerTool::SpirvCross,
                                                   ShaderCompilerTool::AppleMetal};
            static constexpr std::array D3d12Tools{ShaderCompilerTool::Dxc, ShaderCompilerTool::DxilValidator};
            switch (backend) {
                case ShaderTargetBackend::Null:
                    return NullTools;
                case ShaderTargetBackend::OpenGL:
                    return OpenGlTools;
                case ShaderTargetBackend::Vulkan:
                    return VulkanTools;
                case ShaderTargetBackend::Metal:
                    return MetalTools;
                case ShaderTargetBackend::D3D12:
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
            switch (backend) {
                case ShaderTargetBackend::Null:
                    return format == ShaderPayloadFormat::ValidationFixture && environment == ShaderIntermediateEnvironment::None;
                case ShaderTargetBackend::OpenGL:
                    return format == ShaderPayloadFormat::Glsl410 && environment == ShaderIntermediateEnvironment::Vulkan13SpirV16;
                case ShaderTargetBackend::Vulkan:
                    return format == ShaderPayloadFormat::SpirV16 && environment == ShaderIntermediateEnvironment::Vulkan13SpirV16;
                case ShaderTargetBackend::Metal:
                    return format == ShaderPayloadFormat::MetalLibrary24 && environment == ShaderIntermediateEnvironment::Vulkan13SpirV16;
                case ShaderTargetBackend::D3D12:
                    return format == ShaderPayloadFormat::Dxil60 && environment == ShaderIntermediateEnvironment::None;
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateRequest(const ShaderCompilationRequest &request, const ShaderCompilerLimits &limits) {
            if (request.source.empty() || request.source.size() > limits.maximumSourceBytes || request.targets.empty() ||
                request.targets.size() > limits.maximumTargets || request.targets.size() != request.manifest.targets.size() ||
                request.dependencies.size() > limits.maximumDependencies || request.defines.size() > limits.maximumDefines)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));

            const auto manifestResult = ValidateShaderManifest(request.manifest);
            if (manifestResult.HasError())
                return Result<void>::Failure(WrapError(ShaderCompilerPipelineErrors::InvalidRequest, manifestResult.ErrorValue()));

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
            for (std::size_t index = 0; index < request.defines.size(); ++index) {
                const auto &define = request.defines[index];
                if (!IsValidDefineName(define.name, limits.maximumIdentityBytes) ||
                    !IsValidDefineValue(define.value, limits.maximumIdentityBytes))
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
                if (index > 0 && request.defines[index - 1].name >= define.name)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::NonCanonicalInput));
            }
            for (std::size_t index = 0; index < request.targets.size(); ++index) {
                const auto &target = request.targets[index];
                if (!SameRequirement(target.requirement, request.manifest.targets[index]) ||
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

        template <typename ValueT> void AppendInteger(std::vector<std::byte> &bytes, const ValueT value) {
            using UnsignedT = std::make_unsigned_t<ValueT>;
            std::uint64_t bits = static_cast<UnsignedT>(value);
            for (std::size_t index = 0; index < sizeof(UnsignedT); ++index) {
                bytes.push_back(static_cast<std::byte>(bits & 0xffU));
                bits >>= 8U;
            }
        }

        void AppendBool(std::vector<std::byte> &bytes, const bool value) {
            AppendInteger(bytes, static_cast<std::uint8_t>(value));
        }

        void AppendString(std::vector<std::byte> &bytes, const std::string_view value) {
            AppendInteger(bytes, static_cast<std::uint32_t>(value.size()));
            for (const char character : value)
                bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
        }

        void AppendDigest(std::vector<std::byte> &bytes, const Sha256Digest &digest) {
            for (const std::uint8_t byte : digest.bytes)
                bytes.push_back(static_cast<std::byte>(byte));
        }

        [[nodiscard]] Result<Sha256Digest> BuildArtifactKey(const ShaderCompilationRequest &request,
                                                            const ShaderCompilerTargetDescriptor &target,
                                                            const Sha256Digest &sourceDigest) {
            try {
                const auto interfaceId = ComputeShaderInterfaceCompatibilityId(request.manifest);
                if (interfaceId.HasError())
                    return Result<Sha256Digest>::Failure(
                        WrapError(ShaderCompilerPipelineErrors::ArtifactIdentityUnavailable, interfaceId.ErrorValue()));

                std::vector<std::byte> bytes;
                bytes.reserve(512U + request.dependencies.size() * 64U + request.defines.size() * 64U + target.tools.size() * 96U);
                AppendString(bytes, "horo.shader-artifact-key.v1.hlsl2021");
                AppendInteger(bytes, request.manifest.schemaVersion);
                AppendString(bytes, request.manifest.sourceIdentity);
                AppendInteger(bytes, request.manifest.sourceRevision);
                AppendDigest(bytes, interfaceId.Value().digest);
                AppendDigest(bytes, sourceDigest);
                AppendInteger(bytes, static_cast<std::uint32_t>(request.dependencies.size()));
                for (const auto &dependency : request.dependencies) {
                    AppendString(bytes, dependency.logicalPath);
                    AppendDigest(bytes, dependency.digest);
                }
                AppendInteger(bytes, static_cast<std::uint32_t>(request.defines.size()));
                for (const auto &define : request.defines) {
                    AppendString(bytes, define.name);
                    AppendString(bytes, define.value);
                }
                const auto &requirement = target.requirement;
                AppendInteger(bytes, static_cast<std::uint8_t>(requirement.backend));
                AppendInteger(bytes, static_cast<std::uint8_t>(requirement.payloadFormat));
                AppendInteger(bytes, requirement.descriptorVersion);
                AppendInteger(bytes, requirement.interfaceSchemaVersion);
                AppendInteger(bytes, requirement.maximumBindings);
                AppendInteger(bytes, requirement.maximumInlineConstantBytes);
                AppendBool(bytes, requirement.supportsCompute);
                AppendBool(bytes, requirement.supportsStorageResources);
                AppendString(bytes, target.platformTriple);
                AppendInteger(bytes, static_cast<std::uint8_t>(target.intermediateEnvironment));
                AppendInteger(bytes, static_cast<std::uint8_t>(target.optimization));
                AppendBool(bytes, target.emitDebugInformation);
                AppendBool(bytes, target.enableFastMath);
                AppendBool(bytes, target.columnMajorMatrices);
                AppendBool(bytes, target.strictBufferLayout);
                AppendBool(bytes, target.disableAutomaticDepthRemap);
                AppendInteger(bytes, static_cast<std::uint32_t>(target.tools.size()));
                for (const auto &tool : target.tools) {
                    AppendInteger(bytes, static_cast<std::uint8_t>(tool.tool));
                    AppendString(bytes, tool.release);
                    AppendDigest(bytes, tool.buildDigest);
                }
                return Result<Sha256Digest>::Success(ComputeSha256(bytes));
            } catch (const std::bad_alloc &) {
                return Result<Sha256Digest>::Failure(MakeError(ShaderCompilerPipelineErrors::ArtifactIdentityUnavailable));
            }
        }

        [[nodiscard]] bool IsValidDiagnostic(const ShaderCompilerDiagnostic &diagnostic, const ShaderCompilerLimits &limits) noexcept {
            const auto category = static_cast<std::underlying_type_t<ShaderCompilerDiagnosticCategory>>(diagnostic.category);
            const auto severity = static_cast<std::underlying_type_t<ShaderCompilerDiagnosticSeverity>>(diagnostic.severity);
            return category <= static_cast<std::underlying_type_t<ShaderCompilerDiagnosticCategory>>(
                                   ShaderCompilerDiagnosticCategory::Validation) &&
                   severity <=
                       static_cast<std::underlying_type_t<ShaderCompilerDiagnosticSeverity>>(ShaderCompilerDiagnosticSeverity::Error) &&
                   (diagnostic.sourceIdentity.empty() || IsValidLogicalPath(diagnostic.sourceIdentity, limits.maximumIdentityBytes)) &&
                   !diagnostic.message.empty() && diagnostic.message.size() <= limits.maximumDiagnosticMessageBytes &&
                   (diagnostic.line != 0 || diagnostic.column == 0) &&
                   (!diagnostic.sourceIdentity.empty() || (diagnostic.line == 0 && diagnostic.column == 0));
        }

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
            const std::string context = "Shader compiler adapter failed target backend " +
                                        std::to_string(static_cast<std::uint8_t>(invocation.target.requirement.backend)) + ".";
            try {
                auto output = adapter.Compile(invocation, cancellation);
                if (output.HasError())
                    return Result<ShaderCompilerAdapterOutput>::Failure(
                        WrapError(ShaderCompilerPipelineErrors::AdapterFailure, std::move(output).ErrorValue(), context));
                return output;
            } catch (...) {  // NOSONAR(cpp:S1181) Private/native toolchain adapters must not leak exceptions across the typed API.
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::AdapterFailure, context));
            }
        }
    }  // namespace

    /** @copydoc CompileShaderTargets */
    Result<ShaderCompilationBatch> CompileShaderTargets(const ShaderCompilationRequest &request, const IShaderCompilerAdapter &adapter,
                                                        const CancellationToken &cancellation, const ShaderCompilerLimits &limits) {
        if (!IsValidLimits(limits))
            return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidLimits));
        const auto validation = ValidateRequest(request, limits);
        if (validation.HasError())
            return Result<ShaderCompilationBatch>::Failure(validation.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));

        const Sha256Digest sourceDigest = ComputeSha256(std::as_bytes(std::span{request.source}));
        try {
            ShaderCompilationBatch batch{sourceDigest, {}, {}};
            batch.dependencies.reserve(request.dependencies.size());
            for (const ShaderCompilerDependency &dependency : request.dependencies)
                batch.dependencies.push_back({dependency.logicalPath, dependency.digest});
            batch.artifacts.reserve(request.targets.size());
            for (const ShaderCompilerTargetDescriptor &target : request.targets) {
                if (cancellation.IsCancellationRequested())
                    return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
                auto key = BuildArtifactKey(request, target, sourceDigest);
                if (key.HasError())
                    return Result<ShaderCompilationBatch>::Failure(key.ErrorValue());

                const ShaderCompilerInvocation invocation{request.manifest, request.source, request.dependencies, request.defines, target,
                                                          key.Value(),      limits};
                auto output = InvokeAdapter(adapter, invocation, cancellation);
                if (output.HasError())
                    return Result<ShaderCompilationBatch>::Failure(std::move(output).ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
                if (!IsValidOutput(output.Value(), target, limits))
                    return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidAdapterOutput));

                auto value = std::move(output).Value();
                batch.artifacts.push_back(CompiledShaderArtifact{value.backend, value.payloadFormat, key.Value(), std::move(value.payload),
                                                                 std::move(value.debugPayload), std::move(value.diagnostics)});
            }
            return Result<ShaderCompilationBatch>::Success(std::move(batch));
        } catch (const std::bad_alloc &) {
            return Result<ShaderCompilationBatch>::Failure(MakeError(ShaderCompilerPipelineErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
