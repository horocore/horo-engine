#pragma once

/**
 * @file ShaderCompilerPipeline.h
 * @brief Deterministic offline multi-target shader compilation orchestration contract.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/ShaderManifest.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Horo::Render {
    /** @brief Locked tool role participating in one target compilation route. */
    enum class ShaderCompilerTool : std::uint8_t {
        Dxc,
        SpirvTools,
        SpirvCross,
        AppleMetal,
        DxilValidator
    };

    /** @brief Exact release and build identity for one host-selected shader tool. */
    struct ShaderCompilerToolIdentity {
        ShaderCompilerTool tool{ShaderCompilerTool::Dxc};
        std::string release;
        Sha256Digest buildDigest;
    };

    /** @brief Explicit intermediate validation environment for a compilation route. */
    enum class ShaderIntermediateEnvironment : std::uint8_t {
        None,
        Vulkan13SpirV16
    };

    /** @brief Explicit optimization policy participating in artifact identity. */
    enum class ShaderOptimizationLevel : std::uint8_t {
        Disabled,
        Size,
        Performance
    };

    /** @brief Fully specified target route with no ambient compiler defaults. */
    struct ShaderCompilerTargetDescriptor {
        ShaderTargetRequirement requirement;
        std::string platformTriple;
        ShaderIntermediateEnvironment intermediateEnvironment{ShaderIntermediateEnvironment::None};
        ShaderOptimizationLevel optimization{ShaderOptimizationLevel::Performance};
        bool emitDebugInformation{false};
        bool enableFastMath{false};
        bool columnMajorMatrices{true};
        bool strictBufferLayout{true};
        bool disableAutomaticDepthRemap{true};
        std::vector<ShaderCompilerToolIdentity> tools;
    };

    /** @brief One admitted transitive source dependency and its immutable content identity. */
    struct ShaderCompilerDependency {
        std::string logicalPath;
        Sha256Digest digest;
    };

    /** @brief One canonical preprocessor definition supplied by the manifest/permutation owner. */
    struct ShaderCompilerDefine {
        std::string name;
        std::string value;
    };

    /** @brief Finite input/output envelope enforced around every adapter invocation. */
    struct ShaderCompilerLimits {
        std::size_t maximumSourceBytes{16U * 1024U * 1024U};
        std::size_t maximumDependencies{256};
        std::size_t maximumDefines{128};
        std::size_t maximumTargets{8};
        std::size_t maximumPayloadBytes{64U * 1024U * 1024U};
        std::size_t maximumDebugPayloadBytes{64U * 1024U * 1024U};
        std::size_t maximumDiagnosticsPerTarget{256};
        std::size_t maximumDiagnosticMessageBytes{8U * 1024U};
        std::size_t maximumIdentityBytes{256};
    };

    /** @brief Immutable owned source snapshot and complete requested target set. */
    struct ShaderCompilationRequest {
        ShaderManifest manifest;
        std::vector<std::uint8_t> source;
        std::vector<ShaderCompilerDependency> dependencies;
        std::vector<ShaderCompilerDefine> defines;
        std::vector<ShaderCompilerTargetDescriptor> targets;
    };

    /** @brief Stable diagnostic category available before source-map normalization. */
    enum class ShaderCompilerDiagnosticCategory : std::uint8_t {
        Source,
        Include,
        UnsupportedFeature,
        Toolchain,
        Validation
    };

    /** @brief Severity reported by a compiler, translator, validator, or pipeline check. */
    enum class ShaderCompilerDiagnosticSeverity : std::uint8_t {
        Information,
        Warning,
        Error
    };

    /** @brief Bounded source diagnostic emitted by one exact target route. */
    struct ShaderCompilerDiagnostic {
        ShaderCompilerDiagnosticCategory category{ShaderCompilerDiagnosticCategory::Source};
        ShaderCompilerDiagnosticSeverity severity{ShaderCompilerDiagnosticSeverity::Error};
        std::string sourceIdentity;
        std::uint32_t line{0};
        std::uint32_t column{0};
        std::string message;
        bool truncated{false};
    };

    /** @brief Opaque target output returned by a private concrete toolchain adapter. */
    struct ShaderCompilerAdapterOutput {
        ShaderTargetBackend backend{ShaderTargetBackend::Null};
        ShaderPayloadFormat payloadFormat{ShaderPayloadFormat::ValidationFixture};
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> debugPayload;
        std::vector<ShaderCompilerDiagnostic> diagnostics;
    };

    /** @brief One immutable invocation borrowed by the adapter for the duration of Compile. */
    struct ShaderCompilerInvocation {
        const ShaderManifest &manifest;
        const std::vector<std::uint8_t> &source;
        const std::vector<ShaderCompilerDependency> &dependencies;
        const std::vector<ShaderCompilerDefine> &defines;
        const ShaderCompilerTargetDescriptor &target;
        Sha256Digest artifactKey;
        ShaderCompilerLimits limits;
    };

    /** @brief Private-toolchain seam; implementations must not retain borrowed invocation data. */
    class IShaderCompilerAdapter {
    public:
        virtual ~IShaderCompilerAdapter() = default;

        /**
         * @brief Compiles one fully validated target without selecting paths, tools, or fallback routes.
         * @param invocation Immutable borrowed inputs and output bounds.
         * @param cancellation Cooperative cancellation owned by the calling host operation.
         * @return Opaque target payload and bounded diagnostics, or a typed adapter failure.
         */
        [[nodiscard]] virtual Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &invocation,
                                                                          const CancellationToken &cancellation) const = 0;
    };

    /** @brief One validated target artifact and its complete deterministic compatibility key. */
    struct CompiledShaderArtifact {
        ShaderTargetBackend backend{ShaderTargetBackend::Null};
        ShaderPayloadFormat payloadFormat{ShaderPayloadFormat::ValidationFixture};
        Sha256Digest artifactKey;
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> debugPayload;
        std::vector<ShaderCompilerDiagnostic> diagnostics;
    };

    /** @brief Atomic logical result for the complete requested target set. */
    struct ShaderCompilationBatch {
        Sha256Digest sourceDigest;
        std::vector<ShaderCompilerDependency> dependencies;
        std::vector<CompiledShaderArtifact> artifacts;
    };

    /**
     * @brief Compiles all requested targets synchronously through one host-owned adapter.
     * @details Validation and adapter calls run on the calling worker. No threads, paths, caches,
     * publication state, native renderer state, or shutdown responsibility are owned here. A failed,
     * cancelled, or invalid target discards the candidate batch and never silently falls back.
     * @param request Immutable owned source, dependencies, defines, manifest, and target descriptors.
     * @param adapter Host-selected private toolchain adapter.
     * @param cancellation Cooperative operation cancellation checked before and between targets.
     * @param limits Finite input/output envelope.
     * @return Complete ordered artifact batch, or a stable ShaderCompilerPipelineErrors failure.
     */
    [[nodiscard]] Result<ShaderCompilationBatch> CompileShaderTargets(const ShaderCompilationRequest &request,
                                                                      const IShaderCompilerAdapter &adapter,
                                                                      const CancellationToken &cancellation,
                                                                      const ShaderCompilerLimits &limits = {});
}  // namespace Horo::Render
