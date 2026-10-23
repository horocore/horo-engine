#pragma once
#include "JsonUtils.h"
#include "ShaderReflectionCook.h"

namespace Horo::Render::ShaderReflectionCookDetail {
    using Foundation::Json;
    constexpr std::size_t MaximumMetadataBytes = 1024U * 1024U;
    /** @brief Canonical bounded value representation; all layout metadata participates in the cooked identity. */
    [[nodiscard]] Json ReflectionJson(const NormalizedShaderReflection &reflection);
    [[nodiscard]] NormalizedShaderReflection ReadReflection(const Json &json);
    [[nodiscard]] Json ArtifactJson(const ReflectedShaderArtifact &artifact, bool includeKey);
    [[nodiscard]] bool ValidEnvelope(const ReflectedShaderArtifact &artifact);
    [[nodiscard]] Sha256Digest Identity(const ReflectedShaderArtifact &artifact);
}  // namespace Horo::Render::ShaderReflectionCookDetail
