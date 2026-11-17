#pragma once
/** @file EditorViewportShaderRecipe.h @brief Private canonical source contracts shared by the actual viewport consumers and host. */
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <string_view>

namespace Horo::Editor {
    /** @brief Stable complete-family roles; the application request uses the matching strong Horo program identity. */
    enum class EditorViewportShaderProgram : std::uint32_t {
        Lit = 1,
        Grid = 2,
        Shadow = 3
    };
    /** @brief Returns a fixed assets-root-relative HLSL path, empty for an invalid role. */
    [[nodiscard]] std::string_view EditorViewportShaderSource(EditorViewportShaderProgram program) noexcept;
    /** @brief Builds one source manifest for an explicitly admitted GL/Metal target, without selecting tools or inventing reflection. */
    [[nodiscard]] Result<Render::ShaderManifest> MakeEditorViewportShaderManifest(EditorViewportShaderProgram program,
                                                                                  const Render::ShaderTargetRequirement &target);
    /** @brief Authored binding/member names consumed with actual captured tool reflection; no offsets or native slots are supplied. */
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorViewportShaderReflectionNames(EditorViewportShaderProgram program);
    /** @brief Returns the source of an actual clustered family role; grid and shadow retain their existing canonical sources. */
    [[nodiscard]] std::string_view EditorViewportClusteredShaderSource(EditorViewportShaderProgram program) noexcept;
    /** @brief Builds a Metal clustered lit/storage or existing grid/shadow manifest without selecting native tools. */
    [[nodiscard]] Result<Render::ShaderManifest> MakeEditorViewportClusteredShaderManifest(EditorViewportShaderProgram program,
                                                                                           const Render::ShaderTargetRequirement &target);
    /** @brief Maps authored fields to actual target evidence; native slots/strides must come from the compiler output. */
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorViewportClusteredShaderReflectionNames(
        EditorViewportShaderProgram program);
}  // namespace Horo::Editor
