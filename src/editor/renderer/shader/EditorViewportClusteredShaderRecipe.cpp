#include "EditorViewportShaderRecipe.h"
#include "Horo/Runtime/Render/ShaderManifestErrors.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    namespace {
        using namespace Render;

        void AddClusterUniforms(ShaderManifest &manifest) {
            std::erase_if(manifest.parameters, [](const ShaderParameter &parameter) {
                return parameter.id.value >= 5 && parameter.id.value <= 8;
            });
            for (std::uint32_t id = 16; id <= 19; ++id)
                manifest.parameters.push_back({{id}, {1}, ShaderValueType::Float32, 1, 4, 1});
            std::ranges::sort(manifest.parameters, {}, [](const ShaderParameter &parameter) {
                return parameter.id.value;
            });
        }

        void AddClusterStorage(ShaderManifest &manifest) {
            using enum Horo::Render::ShaderValueType;
            for (std::uint32_t id = 4; id <= 6; ++id)
                manifest.bindings.push_back(
                    {{id}, ShaderResourceKind::StorageBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
            const std::array lightParameters{ShaderParameter{{100}, {4}, Float32, 1, 4, 1}, ShaderParameter{{101}, {4}, Float32, 1, 3, 1},
                                             ShaderParameter{{102}, {4}, Uint32, 1, 1, 1},  ShaderParameter{{103}, {4}, Float32, 1, 4, 1},
                                             ShaderParameter{{104}, {4}, Float32, 1, 2, 1}, ShaderParameter{{105}, {4}, Uint32, 1, 1, 1},
                                             ShaderParameter{{106}, {4}, Uint32, 1, 1, 1}};
            manifest.parameters.insert(manifest.parameters.end(), lightParameters.begin(), lightParameters.end());
            for (std::uint32_t id = 110; id <= 113; ++id)
                manifest.parameters.push_back({{id}, {5}, Uint32, 1, 1, 1});
            manifest.parameters.push_back({{120}, {6}, Uint32, 1, 1, 1});
        }
    }  // namespace

    /** @copydoc EditorViewportClusteredShaderSource */
    std::string_view EditorViewportClusteredShaderSource(const EditorViewportShaderProgram program) noexcept {
        return program == EditorViewportShaderProgram::Lit ? "shaders/editor/viewport_clustered_lit.hlsl"
                                                           : EditorViewportShaderSource(program);
    }

    /** @copydoc MakeEditorViewportClusteredShaderManifest */
    Result<Render::ShaderManifest> MakeEditorViewportClusteredShaderManifest(const EditorViewportShaderProgram program,
                                                                             const Render::ShaderTargetRequirement &target) {
        if (target.backend != Render::ShaderTargetBackend::Metal)
            return Result<Render::ShaderManifest>::Failure(MakeError(Render::ShaderManifestErrors::UnsupportedTarget));
        auto result = MakeEditorViewportShaderManifest(program, target);
        if (result.HasError() || program != EditorViewportShaderProgram::Lit)
            return result;
        auto manifest = std::move(result).Value();
        manifest.sourceIdentity = "engine/editor/viewport_clustered_lit.hlsl";
        manifest.sourceRevision = 3;
        AddClusterUniforms(manifest);
        AddClusterStorage(manifest);
        if (const auto valid = Render::ValidateShaderManifest(manifest); valid.HasError())
            return Result<Render::ShaderManifest>::Failure(valid.ErrorValue());
        return Result<Render::ShaderManifest>::Success(std::move(manifest));
    }

    /** @copydoc EditorViewportClusteredShaderReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorViewportClusteredShaderReflectionNames(
        const EditorViewportShaderProgram program) {
        auto names = EditorViewportShaderReflectionNames(program);
        if (program != EditorViewportShaderProgram::Lit)
            return names;
        auto &uniform = names.front();
        std::erase_if(uniform.parameters, [](const Render::ShaderReflectionParameterName &parameter) {
            return parameter.id.value >= 5 && parameter.id.value <= 8;
        });
        uniform.parameters.insert(uniform.parameters.end(), {{{16}, "ClusterDimensions"},
                                                             {{17}, "ClusterDepth"},
                                                             {{18}, "ViewportDimensions"},
                                                             {{19}, "ShadowIdentity"}});
        names.push_back({{4},
                         "ClusterLights",
                         {{{100}, "positionRange"},
                          {{101}, "direction"},
                          {{102}, "kind"},
                          {{103}, "colorIntensity"},
                          {{104}, "cones"},
                          {{105}, "identityLow"},
                          {{106}, "identityHigh"}}});
        names.push_back({{5}, "ClusterMembership", {{{110}, "offset"}, {{111}, "count"}, {{112}, "omitted"}, {{113}, "reserved"}}});
        names.push_back({{6}, "ClusterReferences", {{{120}, "_m0"}}});
        return names;
    }
}  // namespace Horo::Editor
