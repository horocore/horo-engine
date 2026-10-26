#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "ShaderReflectionArtifactJson.h"

namespace Horo::Render {
    using namespace ShaderReflectionCookDetail;

    namespace {
        /** @brief Revalidates final cooked reflection against the runtime manifest and target contract. */
        Result<NormalizedShaderReflection> NormalizeCookedReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                     const NormalizedShaderReflection &reflection) {
            const auto &value = reflection;
            const ShaderReflectionCandidate candidate{value.backend,
                                                      value.interfaceSchemaVersion,
                                                      value.bindings,
                                                      value.parameters,
                                                      value.stageInterface,
                                                      value.targetBindings,
                                                      {}};
            return NormalizeShaderReflection(manifest, target, candidate);
        }
    }  // namespace

    /** @copydoc CookReflectedShaderArtifact */
    Result<ReflectedShaderArtifact> CookReflectedShaderArtifact(const CompiledShaderArtifact &compiled,
                                                                const ShaderCompilerToolIdentity &reflectionTool,
                                                                NormalizedShaderReflection reflection, const std::uint32_t layoutVersion) {
        try {
            ReflectedShaderArtifact artifact{layoutVersion,
                                             compiled.artifactKey,
                                             ComputeSha256(std::as_bytes(std::span{compiled.payload})),
                                             reflectionTool,
                                             std::move(reflection),
                                             {}};
            if (!ValidEnvelope(artifact))
                return Result<ReflectedShaderArtifact>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            artifact.cookedArtifactKey = Identity(artifact);
            if (const auto valid = ValidateReflectedShaderArtifact(compiled, artifact); valid.HasError())
                return Result<ReflectedShaderArtifact>::Failure(valid.ErrorValue());
            return Result<ReflectedShaderArtifact>::Success(std::move(artifact));
        } catch (const std::bad_alloc &) {
            return Result<ReflectedShaderArtifact>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }

    /** @copydoc ValidateReflectedShaderArtifact */
    Result<void> ValidateReflectedShaderArtifact(const CompiledShaderArtifact &compiled, const ReflectedShaderArtifact &artifact) {
        try {
            if (const bool supported =
                    (compiled.backend == ShaderTargetBackend::Vulkan && compiled.payloadFormat == ShaderPayloadFormat::SpirV16) ||
                    (compiled.backend == ShaderTargetBackend::Metal && compiled.payloadFormat == ShaderPayloadFormat::MetalLibrary24);
                !supported || !ValidEnvelope(artifact) || compiled.backend != artifact.reflection.backend ||
                compiled.artifactKey != artifact.compiledArtifactKey || compiled.payload.empty() ||
                compiled.payload.size() > 64U * 1024U * 1024U ||
                ComputeSha256(std::as_bytes(std::span{compiled.payload})) != artifact.payloadDigest ||
                DecodeShaderArtifactStages(compiled.payload, compiled.backend, compiled.payloadFormat, 64U * 1024U * 1024U).HasError() ||
                artifact.cookedArtifactKey != Identity(artifact))
                return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }

    /** @copydoc EncodeReflectedShaderArtifact */
    Result<std::vector<std::uint8_t>> EncodeReflectedShaderArtifact(const ReflectedShaderArtifact &artifact) {
        try {
            if (!ValidEnvelope(artifact) || artifact.cookedArtifactKey != Identity(artifact))
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            const auto text = ArtifactJson(artifact, true).dump();
            if (text.size() > MaximumMetadataBytes)
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderReflectionErrors::InvalidLimits));
            return Result<std::vector<std::uint8_t>>::Success({text.begin(), text.end()});
        } catch (const std::bad_alloc &) {
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }

    /** @copydoc DecodeReflectedShaderArtifact */
    Result<ReflectedShaderArtifact> DecodeReflectedShaderArtifact(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                  const std::span<const std::uint8_t> payload,
                                                                  const std::span<const std::uint8_t> metadata) {
        using Outcome = Result<ReflectedShaderArtifact>;
        if (metadata.empty() || metadata.size() > MaximumMetadataBytes || payload.empty() || payload.size() > 64U * 1024U * 1024U)
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidLimits));
        try {
            Foundation::JsonParseGuard guard{16};
            const auto json = guard.Parse(metadata.begin(), metadata.end());
            if (guard.HasDuplicate() || guard.IsTooDeep() || json.is_discarded())
                return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            ReflectedShaderArtifact artifact;
            artifact.layoutVersion = json.at("layoutVersion").get<std::uint32_t>();
            const auto compiled = ParseSha256(json.at("compiledKey").get<std::string>());
            const auto digest = ParseSha256(json.at("payloadDigest").get<std::string>());
            const auto tool = ParseSha256(json.at("tool").at(2).get<std::string>());
            const auto cooked = ParseSha256(json.at("cookedKey").get<std::string>());
            if (compiled.HasError() || digest.HasError() || tool.HasError() || cooked.HasError())
                return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            artifact.compiledArtifactKey = compiled.Value();
            artifact.payloadDigest = digest.Value();
            artifact.reflectionTool = {json.at("tool").at(0).get<ShaderCompilerTool>(), json.at("tool").at(1).get<std::string>(),
                                       tool.Value()};
            artifact.reflection = ReadReflection(json.at("reflection"));
            artifact.cookedArtifactKey = cooked.Value();
            if (!ValidEnvelope(artifact) || artifact.payloadDigest != ComputeSha256(std::as_bytes(payload)) ||
                artifact.cookedArtifactKey != Identity(artifact) ||
                DecodeShaderArtifactStages(payload, target.backend, target.payloadFormat, 64U * 1024U * 1024U).HasError())
                return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            const auto normalized = NormalizeCookedReflection(manifest, target, artifact.reflection);
            if (normalized.HasError())
                return Outcome::Failure(normalized.ErrorValue());
            artifact.reflection = normalized.Value();
            if (const auto canonical = EncodeReflectedShaderArtifact(artifact);
                canonical.HasError() || !std::ranges::equal(metadata, canonical.Value()))
                return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            return Outcome::Success(std::move(artifact));
        } catch (const Json::exception &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
