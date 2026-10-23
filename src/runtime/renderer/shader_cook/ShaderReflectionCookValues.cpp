#include "ShaderReflectionCookJson.h"

#include <algorithm>

namespace Horo::Render::ShaderReflectionCookDetail {
    /** @copydoc ReflectionJson */
    Json ReflectionJson(const NormalizedShaderReflection &reflection) {
        Json bindings = Json::array(), parameters = Json::array(), interfaces = Json::array(), mappings = Json::array();
        for (const auto &value : reflection.bindings)
            bindings.push_back({value.id.value, value.kind, value.access, value.arrayCount, value.stages, value.active});
        for (const auto &value : reflection.parameters)
            parameters.push_back({value.id.value, value.binding.value, value.type, value.rows, value.columns, value.arrayCount,
                                  value.byteOffset, value.arrayStride, value.matrixStride, value.columnMajor, value.active});
        for (const auto &value : reflection.stageInterface)
            interfaces.push_back(
                {value.stage, value.direction, value.location, value.type, value.rows, value.columns, value.arrayCount, value.nativeName});
        for (const auto &value : reflection.targetBindings)
            mappings.push_back({value.id.value, value.generatedHelperIndex, value.nativeSpace, value.nativeBinding, value.nativeName,
                                value.pairedSampler.value, value.active});
        return {{"backend", reflection.backend},
                {"schema", reflection.interfaceSchemaVersion},
                {"interface", FormatSha256(reflection.interfaceCompatibility.digest)},
                {"bindings", bindings},
                {"parameters", parameters},
                {"interfaces", interfaces},
                {"mappings", mappings}};
    }

    /** @copydoc ReadReflection */
    NormalizedShaderReflection ReadReflection(const Json &json) {
        NormalizedShaderReflection reflection;
        reflection.backend = json.at("backend").get<ShaderTargetBackend>();
        reflection.interfaceSchemaVersion = json.at("schema").get<std::uint32_t>();
        const auto digest = ParseSha256(json.at("interface").get<std::string>());
        if (digest.HasValue())
            reflection.interfaceCompatibility.digest = digest.Value();
        if (json.at("bindings").size() > 128 || json.at("parameters").size() > 512 || json.at("interfaces").size() > 256 ||
            json.at("mappings").size() > 256)
            return {};
        for (const auto &v : json.at("bindings"))
            reflection.bindings.push_back({{v.at(0).get<std::uint32_t>()},
                                           v.at(1).get<ShaderResourceKind>(),
                                           v.at(2).get<ShaderResourceAccess>(),
                                           v.at(3).get<std::uint32_t>(),
                                           v.at(4).get<ShaderStageVisibility>(),
                                           v.at(5).get<bool>()});
        for (const auto &v : json.at("parameters"))
            reflection.parameters.push_back({{v.at(0).get<std::uint32_t>()},
                                             {v.at(1).get<std::uint32_t>()},
                                             v.at(2).get<ShaderValueType>(),
                                             v.at(3).get<std::uint8_t>(),
                                             v.at(4).get<std::uint8_t>(),
                                             v.at(5).get<std::uint32_t>(),
                                             v.at(6).get<std::uint32_t>(),
                                             v.at(7).get<std::uint32_t>(),
                                             v.at(8).get<std::uint32_t>(),
                                             v.at(9).get<bool>(),
                                             v.at(10).get<bool>()});
        for (const auto &v : json.at("interfaces"))
            reflection.stageInterface.push_back({v.at(0).get<ShaderStage>(), v.at(1).get<ShaderInterfaceDirection>(),
                                                 v.at(2).get<std::uint32_t>(), v.at(3).get<ShaderValueType>(), v.at(4).get<std::uint8_t>(),
                                                 v.at(5).get<std::uint8_t>(), v.at(6).get<std::uint32_t>(), v.at(7).get<std::string>()});
        for (const auto &v : json.at("mappings"))
            reflection.targetBindings.push_back({{v.at(0).get<std::uint32_t>()},
                                                 v.at(1).get<std::uint32_t>(),
                                                 v.at(2).get<std::uint32_t>(),
                                                 v.at(3).get<std::uint32_t>(),
                                                 v.at(4).get<std::string>(),
                                                 {v.at(5).get<std::uint32_t>()},
                                                 v.at(6).get<bool>()});
        return reflection;
    }

    /** @copydoc ArtifactJson */
    Json ArtifactJson(const ReflectedShaderArtifact &artifact, const bool includeKey) {
        Json json{{"layoutVersion", artifact.layoutVersion},
                  {"compiledKey", FormatSha256(artifact.compiledArtifactKey)},
                  {"payloadDigest", FormatSha256(artifact.payloadDigest)},
                  {"tool",
                   {artifact.reflectionTool.tool, artifact.reflectionTool.release, FormatSha256(artifact.reflectionTool.buildDigest)}},
                  {"reflection", ReflectionJson(artifact.reflection)}};
        if (includeKey)
            json["cookedKey"] = FormatSha256(artifact.cookedArtifactKey);
        return json;
    }

    /** @copydoc ValidEnvelope */
    bool ValidEnvelope(const ReflectedShaderArtifact &artifact) {
        const auto hasDigest = [](const Sha256Digest &digest) {
            return std::ranges::any_of(digest.bytes, [](auto value) {
                return value != 0;
            });
        };
        const ShaderReflectionLimits limits;
        return artifact.layoutVersion == 1 && artifact.reflectionTool.tool == ShaderCompilerTool::SpirvCross &&
               !artifact.reflectionTool.release.empty() && artifact.reflectionTool.release.size() <= 256 &&
               hasDigest(artifact.compiledArtifactKey) && hasDigest(artifact.payloadDigest) &&
               hasDigest(artifact.reflectionTool.buildDigest) && artifact.reflection.backend == ShaderTargetBackend::Vulkan &&
               hasDigest(artifact.reflection.interfaceCompatibility.digest) && artifact.reflection.sourceMap.empty() &&
               artifact.reflection.bindings.size() <= limits.maximumBindings &&
               artifact.reflection.parameters.size() <= limits.maximumParameters &&
               artifact.reflection.stageInterface.size() <= limits.maximumStageInterfaceVariables &&
               artifact.reflection.targetBindings.size() <= limits.maximumTargetBindingEntries;
    }

    /** @copydoc Identity */
    Sha256Digest Identity(const ReflectedShaderArtifact &artifact) {
        const auto canonical = ArtifactJson(artifact, false).dump();
        return ComputeSha256(std::as_bytes(std::span{canonical}));
    }
}  // namespace Horo::Render::ShaderReflectionCookDetail
