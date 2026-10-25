#include "ShaderCompilerArtifactKey.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <new>
#include <string_view>
#include <type_traits>

namespace Horo::Render::ShaderCompilerDetail {
    namespace {
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

        void AppendTargetIdentity(std::vector<std::byte> &bytes, const ShaderCompilerTargetDescriptor &target) {
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
        }

        /** @brief Adds explicit reflected cook inputs without changing keys for legacy unreflected requests. */
        void AppendReflectionIdentity(std::vector<std::byte> &bytes, const ShaderCompilerReflectionRequest &request) {
            AppendString(bytes, "horo.shader-reflection-request.v1");
            AppendInteger(bytes, static_cast<std::uint8_t>(request.tool.tool));
            AppendString(bytes, request.tool.release);
            AppendDigest(bytes, request.tool.buildDigest);
            AppendInteger(bytes, static_cast<std::uint32_t>(request.names.size()));
            for (const auto &binding : request.names) {
                AppendInteger(bytes, binding.id.value);
                AppendString(bytes, binding.name);
                AppendInteger(bytes, static_cast<std::uint32_t>(binding.parameters.size()));
                for (const auto &parameter : binding.parameters) {
                    AppendInteger(bytes, parameter.id.value);
                    AppendString(bytes, parameter.name);
                }
            }
        }

    }  // namespace

    /** @copydoc BuildArtifactKey */
    Result<Sha256Digest> BuildArtifactKey(const ShaderCompilationRequest &request, const ShaderCompilerTargetDescriptor &target,
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
            AppendTargetIdentity(bytes, target);
            if (request.reflection)
                AppendReflectionIdentity(bytes, *request.reflection);
            return Result<Sha256Digest>::Success(ComputeSha256(bytes));
        } catch (const std::bad_alloc &) {
            return Result<Sha256Digest>::Failure(MakeError(ShaderCompilerPipelineErrors::ArtifactIdentityUnavailable));
        }
    }

}  // namespace Horo::Render::ShaderCompilerDetail
