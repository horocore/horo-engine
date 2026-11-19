#include "EditorVulkanViewportShader.h"

#include "EditorViewportShaderRecipe.h"
#include "Horo/Runtime/Render/MultiviewShaderParameters.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

namespace Horo::Editor {
    /** @copydoc EditorVulkanViewportShaderTarget */
    Render::ShaderTargetRequirement EditorVulkanViewportShaderTarget() {
        return {.backend = Render::ShaderTargetBackend::Vulkan,
                .payloadFormat = Render::ShaderPayloadFormat::SpirV16,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 16,
                .maximumInlineConstantBytes = 128};
    }

    /** @copydoc EditorVulkanViewportShaderManifest */
    Render::ShaderManifest EditorVulkanViewportShaderManifest() {
        using namespace Render;
        return {.schemaVersion = 1,
                .sourceIdentity = "engine/editor/viewport.hlsl",
                .sourceRevision = 4,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1},
                              ShaderResourceKind::UniformBuffer,
                              ShaderResourceAccess::ReadOnly,
                              1,
                              ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment},
                             {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}},
                .parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{2}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{3}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{4}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{5}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{6}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{7}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{8}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{9}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{10}, {1}, ShaderValueType::Float32, 1, 4, 3},
                               {{11}, {1}, ShaderValueType::Float32, 4, 4, 4},
                               {{12}, {1}, ShaderValueType::Float32, 1, 4, 4},
                               {{13}, {1}, ShaderValueType::Float32, 1, 4, 4},
                               {{14}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{15}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{16}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{20}, {1}, ShaderValueType::Float32, 1, 4, 1}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanViewportReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanViewportReflectionNames() {
        return {{{1},
                 "type.ViewportParameters",
                 {{{1}, "Mvp"},
                  {{2}, "NormalToWorld"},
                  {{3}, "ModelToWorld"},
                  {{4}, "Tint"},
                  {{5}, "CameraAndLightCount"},
                  {{6}, "LightPositionKind"},
                  {{7}, "LightDirectionRange"},
                  {{8}, "LightColorIntensity"},
                  {{9}, "LightCone"},
                  {{10}, "OverlayColors"},
                  {{11}, "ShadowViewProjection"},
                  {{12}, "ShadowSplits"},
                  {{13}, "ShadowAtlasScaleBias"},
                  {{14}, "ShadowSettings"},
                  {{15}, "ShadowBiasAndTexel"},
                  {{16}, "ShadowCameraForward"},
                  {{20}, "BaseColor"}}},
                {{2}, "DirectionalShadowAtlas", {}},
                {{3}, "DirectionalShadowSampler", {}}};
    }

    namespace {
        /** @brief Validates the real mesh vertex ABI used by either viewport program. */
        Result<void> ValidateViewportMeshLayout(const Render::NormalizedShaderReflection &reflection) {
            bool position{}, normal{};
            for (const auto &attribute : reflection.stageInterface) {
                if (attribute.stage != Render::ShaderStage::Vertex || attribute.direction != Render::ShaderInterfaceDirection::Input)
                    continue;
                const auto columns = attribute.location == 2 ? 2 : 3;
                if (attribute.location > 2 || attribute.type != Render::ShaderValueType::Float32 || attribute.rows != 1 ||
                    attribute.columns != columns || attribute.arrayCount != 1)
                    return Result<void>::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
                position = position || attribute.location == 0;
                normal = normal || attribute.location == 1;
            }
            // UV may disappear after optimization. Position and normal drive both shipped mesh recipes.
            if (!position || !normal)
                return Result<void>::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
            return Result<void>::Success();
        }

        /** @brief Checks the exact stage names encoded in the payload as well as its reflected interface. */
        Result<void> ValidateViewportStageEntries(const Render::ShaderManifest &manifest, const Render::ShaderTargetRequirement &target,
                                                  std::span<const std::uint8_t> payload) {
            const auto stages = Render::DecodeShaderArtifactStages(payload, target.backend, target.payloadFormat, 16U * 1024U * 1024U);
            if (stages.HasError())
                return Result<void>::Failure(stages.ErrorValue());
            if (stages.Value().stageCount != manifest.entryPoints.size())
                return Result<void>::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
            for (std::size_t index = 0; index < manifest.entryPoints.size(); ++index) {
                const auto &expected = manifest.entryPoints[index];
                const auto &native = stages.Value().stages[index];
                if (native.stage != expected.stage || native.entryPoint != expected.name)
                    return Result<void>::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
            }
            return Result<void>::Success();
        }

        /** @brief Decodes bound artifacts without duplicating mesh layout policy across the two owned programs. */
        template <typename Cook>
        Result<Cook> LoadViewportProgram(const Render::ShaderManifest &manifest, std::span<const std::uint8_t> payload,
                                         std::span<const std::uint8_t> metadata) {
            auto reflection = Render::DecodeReflectedShaderArtifact(manifest, EditorVulkanViewportShaderTarget(), payload, metadata);
            if (reflection.HasError())
                return Result<Cook>::Failure(reflection.ErrorValue());
            if (auto valid = ValidateViewportMeshLayout(reflection.Value().reflection); valid.HasError())
                return Result<Cook>::Failure(valid.ErrorValue());
            return Result<Cook>::Success({{payload.begin(), payload.end()}, std::move(reflection).Value()});
        }
    }  // namespace

    /** @copydoc LoadEditorVulkanViewportCook */
    Result<EditorVulkanViewportCook> LoadEditorVulkanViewportCook(std::span<const std::uint8_t> payload,
                                                                  std::span<const std::uint8_t> metadata) {
        return LoadViewportProgram<EditorVulkanViewportCook>(EditorVulkanViewportShaderManifest(), payload, metadata);
    }

    /** @copydoc EditorVulkanSceneLinearShaderManifest */
    Render::ShaderManifest EditorVulkanSceneLinearShaderManifest() {
        auto manifest = EditorVulkanViewportShaderManifest();
        manifest.entryPoints.back().name = "PSSceneLinear";
        return manifest;
    }

    /** @copydoc EditorVulkanSceneLinearPipelineState */
    Render::RenderGraphicsPipelineState EditorVulkanSceneLinearPipelineState() noexcept {
        return {.cull = Render::RenderCullMode::None,
                .colorFormat = Render::RenderTextureFormat::Rgba16Float,
                .depthFormat = Render::RenderTextureFormat::Depth32Float};
    }

    /** @copydoc LoadEditorVulkanSceneLinearCook */
    Result<EditorVulkanSceneLinearCook> LoadEditorVulkanSceneLinearCook(std::span<const std::uint8_t> payload,
                                                                        std::span<const std::uint8_t> metadata) {
        using namespace Render;
        using Outcome = Result<EditorVulkanSceneLinearCook>;
        const auto manifest = EditorVulkanSceneLinearShaderManifest();
        const auto target = EditorVulkanViewportShaderTarget();
        if (const auto stages = ValidateViewportStageEntries(manifest, target, payload); stages.HasError())
            return Outcome::Failure(stages.ErrorValue());
        return LoadViewportProgram<EditorVulkanSceneLinearCook>(manifest, payload, metadata);
    }

    /** @copydoc EditorVulkanTemporalInputShaderManifest */
    Render::ShaderManifest EditorVulkanTemporalInputShaderManifest(EditorVulkanTemporalInput input) {
        using namespace Render;
        using enum ShaderValueType;
        const bool motion = input == EditorVulkanTemporalInput::Motion;
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity = "assets/shaders/editor/viewport_temporal_inputs.hlsl",
                                .sourceRevision = 1,
                                .entryPoints = {{ShaderStage::Vertex, "VSMain"},
                                                {ShaderStage::Fragment, motion ? "PSMotion" : "PSLinearDepth"}},
                                .bindings = {{{1},
                                              ShaderResourceKind::UniformBuffer,
                                              ShaderResourceAccess::ReadOnly,
                                              1,
                                              ShaderStageVisibility::Vertex}},
                                .parameters = {{{1}, {1}, Float32, 4, 4, 1}, {{2}, {1}, Float32, 4, 4, 1}},
                                .targets = {EditorVulkanViewportShaderTarget()}};
        if (motion) {
            manifest.parameters.push_back({{3}, {1}, Float32, 4, 4, 1});
            manifest.parameters.push_back({{4}, {1}, Uint32, 1, 1, 1});
        }
        return manifest;
    }

    /** @copydoc EditorVulkanTemporalInputReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanTemporalInputReflectionNames(EditorVulkanTemporalInput input) {
        if (input == EditorVulkanTemporalInput::Motion)
            return {{{1},
                     "type.ViewportTemporalParameters",
                     {{{1}, "RasterLocalToClip"}, {{2}, "CurrentLocalToClip"}, {{3}, "PreviousLocalToClip"}, {{4}, "HasPrevious"}}}};
        return {{{1}, "type.ViewportTemporalParameters", {{{1}, "RasterLocalToClip"}, {{2}, "LocalToView"}}}};
    }

    /** @copydoc ValidateEditorVulkanTemporalInputCook */
    Result<Render::ReflectedShaderArtifact> ValidateEditorVulkanTemporalInputCook(EditorVulkanTemporalInput input,
                                                                                  std::span<const std::uint8_t> payload,
                                                                                  std::span<const std::uint8_t> metadata) {
        using namespace Render;
        using Outcome = Result<ReflectedShaderArtifact>;
        const auto manifest = EditorVulkanTemporalInputShaderManifest(input);
        const auto target = EditorVulkanViewportShaderTarget();
        if (const auto stages = ValidateViewportStageEntries(manifest, target, payload); stages.HasError())
            return Outcome::Failure(stages.ErrorValue());
        auto result = DecodeReflectedShaderArtifact(manifest, target, payload, metadata);
        if (result.HasError())
            return result;
        const auto exact = [](const auto &attribute, unsigned columns) {
            return attribute.location == 0 && attribute.type == ShaderValueType::Float32 && attribute.rows == 1 &&
                   attribute.columns == columns && attribute.arrayCount == 1;
        };
        unsigned vertex{}, fragment{};
        const unsigned columns = input == EditorVulkanTemporalInput::Motion ? 2U : 1U;
        for (const auto &attribute : result.Value().reflection.stageInterface) {
            if (attribute.stage == ShaderStage::Vertex && attribute.direction == ShaderInterfaceDirection::Input) {
                if (!exact(attribute, 3))
                    return Outcome::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
                ++vertex;
            }
            if (attribute.stage == ShaderStage::Fragment && attribute.direction == ShaderInterfaceDirection::Output) {
                if (!exact(attribute, columns))
                    return Outcome::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
                ++fragment;
            }
        }
        if (vertex != 1 || fragment != 1)
            return Outcome::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
        return result;
    }

    Render::ShaderManifest EditorVulkanShadowShaderManifest() {
        return MakeEditorViewportShaderManifest(EditorViewportShaderProgram::Shadow, EditorVulkanViewportShaderTarget()).Value();
    }

    std::vector<Render::ShaderReflectionBindingName> EditorVulkanShadowReflectionNames() {
        return EditorViewportShaderReflectionNames(EditorViewportShaderProgram::Shadow);
    }

    Result<EditorVulkanShadowCook> LoadEditorVulkanShadowCook(std::span<const std::uint8_t> payload,
                                                              std::span<const std::uint8_t> metadata) {
        auto reflection = Render::DecodeReflectedShaderArtifact(EditorVulkanShadowShaderManifest(), EditorVulkanViewportShaderTarget(),
                                                                payload, metadata);
        if (reflection.HasError())
            return Result<EditorVulkanShadowCook>::Failure(reflection.ErrorValue());
        return Result<EditorVulkanShadowCook>::Success({{payload.begin(), payload.end()}, std::move(reflection).Value()});
    }

    /** @copydoc EditorVulkanMultiviewShaderManifest */
    Render::ShaderManifest EditorVulkanMultiviewShaderManifest() {
        using enum Horo::Render::ShaderValueType;
        using namespace Render;
        return {.schemaVersion = 1,
                .sourceIdentity = "assets/shaders/runtime/multiview_mesh.hlsl",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex}},
                .parameters = {{{1}, {1}, Float32, 4, 4, 32}, {{2}, {1}, Float32, 4, 4, 1}, {{3}, {1}, Uint32, 1, 1, 32}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanMultiviewReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewReflectionNames() {
        return {{{1}, "type.MultiviewParameters", {{{1}, "WorldToClip"}, {{2}, "LocalToWorld"}, {{3}, "ViewLayers"}}}};
    }

    /** @copydoc LoadEditorVulkanMultiviewCook */
    Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewCook(const std::span<const std::uint8_t> payload,
                                                                    const std::span<const std::uint8_t> metadata) {
        using Outcome = Result<EditorVulkanMultiviewCook>;
        auto reflection = Render::DecodeReflectedShaderArtifact(EditorVulkanMultiviewShaderManifest(), EditorVulkanViewportShaderTarget(),
                                                                payload, metadata);
        if (reflection.HasError())
            return Outcome::Failure(reflection.ErrorValue());
        bool position{};
        for (const auto &attribute : reflection.Value().reflection.stageInterface) {
            if (attribute.stage != Render::ShaderStage::Vertex || attribute.direction != Render::ShaderInterfaceDirection::Input)
                continue;
            // The canonical shader consumes position; unused MeshVertex normal/UV inputs may be optimized out.
            if (attribute.location != 0 || attribute.type != Render::ShaderValueType::Float32 || attribute.rows != 1 ||
                attribute.columns != 3 || attribute.arrayCount != 1)
                return Outcome::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
            position = true;
        }
        if (!position)
            return Outcome::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
        const auto layout = Render::PrepareMaterialBindingLayout(reflection.Value().reflection);
        if (layout.HasError())
            return Outcome::Failure(layout.ErrorValue());
        const auto bytes = Render::MultiviewShaderParameterBytes(layout.Value(), 2);
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        if (bytes.Value() == 0 || bytes.Value() > 4096)
            return Outcome::Failure(MakeError(Render::ShaderReflectionErrors::LayoutMismatch));
        return Outcome::Success({{payload.begin(), payload.end()}, std::move(reflection).Value()});
    }

    /** @copydoc EditorVulkanMultiviewMotionShaderManifest */
    Render::ShaderManifest EditorVulkanMultiviewMotionShaderManifest() {
        using namespace Render;
        using namespace Render::MultiviewShaderParameterIds;
        return {.schemaVersion = 1,
                .sourceIdentity = "assets/shaders/runtime/multiview_motion.hlsl",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{Buffer, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex}},
                .parameters = {{CurrentLocalToClip, Buffer, ShaderValueType::Float32, 4, 4, MultiviewExtractionLimits::HardMaximumViews},
                               {PreviousLocalToClip, Buffer, ShaderValueType::Float32, 4, 4, MultiviewExtractionLimits::HardMaximumViews}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanMultiviewMotionReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewMotionReflectionNames() {
        using namespace Render::MultiviewShaderParameterIds;
        return {{Buffer,
                 "type.MultiviewMotionParameters",
                 {{CurrentLocalToClip, "CurrentLocalToClip"}, {PreviousLocalToClip, "PreviousLocalToClip"}}}};
    }

    namespace {
        /** @brief Validates the shared multipass vertex/clip ABI and the selected float output width. */
        Result<EditorVulkanMultiviewCook> LoadMultiviewMotionProgram(const Render::ShaderManifest &manifest,
                                                                     const std::span<const std::uint8_t> payload,
                                                                     const std::span<const std::uint8_t> metadata, const bool coverage) {
            using namespace Render;
            using Outcome = Result<EditorVulkanMultiviewCook>;
            auto reflected = DecodeReflectedShaderArtifact(manifest, EditorVulkanViewportShaderTarget(), payload, metadata);
            if (reflected.HasError())
                return Outcome::Failure(reflected.ErrorValue());
            const auto &reflection = reflected.Value().reflection;
            const auto matches = [&reflection](const ShaderStage stage, const ShaderInterfaceDirection direction,
                                               const std::uint32_t location, const std::uint32_t columns,
                                               const ShaderValueType type = ShaderValueType::Float32) {
                const auto found = std::ranges::find_if(reflection.stageInterface, [=](const auto &attribute) {
                    return attribute.stage == stage && attribute.direction == direction && attribute.location == location;
                });
                return found != reflection.stageInterface.end() && found->type == type && found->rows == 1 && found->columns == columns &&
                       found->arrayCount == 1;
            };
            using enum ShaderInterfaceDirection;
            if (reflection.stageInterface.size() != (coverage ? 8 : 6) || !matches(ShaderStage::Vertex, Input, 0, 3) ||
                !matches(ShaderStage::Vertex, Output, 0, 4) || !matches(ShaderStage::Vertex, Output, 1, 4) ||
                !matches(ShaderStage::Fragment, Input, 0, 4) || !matches(ShaderStage::Fragment, Input, 1, 4) ||
                !matches(ShaderStage::Fragment, Output, 0, coverage ? 1 : 4))
                return Outcome::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
            if (coverage && (!matches(ShaderStage::Vertex, Output, 2, 1, ShaderValueType::Uint32) ||
                             !matches(ShaderStage::Fragment, Input, 2, 1, ShaderValueType::Uint32)))
                return Outcome::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
            const auto layout = PrepareMaterialBindingLayout(reflection);
            if (layout.HasError())
                return Outcome::Failure(layout.ErrorValue());
            const auto bytes = MultiviewMotionParameterBytes(layout.Value(), MultiviewExtractionLimits::HardMaximumViews);
            if (bytes.HasError())
                return Outcome::Failure(bytes.ErrorValue());
            constexpr auto blockBytes = MultiviewExtractionLimits::HardMaximumViews * 2 * sizeof(Math::Mat4);
            if (bytes.Value() != blockBytes)
                return Outcome::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
            return Outcome::Success({{payload.begin(), payload.end()}, std::move(reflected).Value()});
        }
    }  // namespace

    /** @copydoc LoadEditorVulkanMultiviewMotionCook */
    Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewMotionCook(const std::span<const std::uint8_t> payload,
                                                                          const std::span<const std::uint8_t> metadata) {
        return LoadMultiviewMotionProgram(EditorVulkanMultiviewMotionShaderManifest(), payload, metadata, false);
    }

    /** @copydoc EditorVulkanMultiviewCoverageShaderManifest */
    Render::ShaderManifest EditorVulkanMultiviewCoverageShaderManifest() {
        using namespace Render;
        auto manifest = EditorVulkanMultiviewMotionShaderManifest();
        manifest.entryPoints.front().name = "VSCoverage";
        manifest.entryPoints.back().name = "PSCoverage";
        for (const auto binding : MultiviewShaderParameterIds::PreviousEyeDepthBindings)
            manifest.bindings.push_back(
                {binding, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
        return manifest;
    }

    /** @copydoc EditorVulkanMultiviewCoverageReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewCoverageReflectionNames() {
        auto names = EditorVulkanMultiviewMotionReflectionNames();
        const auto &bindings = Render::MultiviewShaderParameterIds::PreviousEyeDepthBindings;
        names.push_back({bindings[0], "PreviousLeftDepth", {}});
        names.push_back({bindings[1], "PreviousRightDepth", {}});
        return names;
    }

    /** @copydoc LoadEditorVulkanMultiviewCoverageCook */
    Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewCoverageCook(const std::span<const std::uint8_t> payload,
                                                                            const std::span<const std::uint8_t> metadata) {
        using namespace Render;
        using Outcome = Result<EditorVulkanMultiviewCook>;
        const auto manifest = EditorVulkanMultiviewCoverageShaderManifest();
        if (const auto stages = ValidateViewportStageEntries(manifest, EditorVulkanViewportShaderTarget(), payload); stages.HasError())
            return Outcome::Failure(stages.ErrorValue());
        auto cook = LoadMultiviewMotionProgram(manifest, payload, metadata, true);
        if (cook.HasError())
            return cook;
        const auto &bindings = cook.Value().metadata.reflection.bindings;
        for (const auto binding : MultiviewShaderParameterIds::PreviousEyeDepthBindings) {
            const auto depth = std::ranges::find(bindings, binding, &ShaderReflectedBinding::id);
            if (depth == bindings.end() || !depth->active || depth->stages != ShaderStageVisibility::Fragment)
                return Outcome::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
        }
        return cook;
    }
}  // namespace Horo::Editor
