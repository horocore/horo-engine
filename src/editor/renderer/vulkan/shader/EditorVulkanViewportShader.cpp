#include "EditorVulkanViewportShader.h"

#include "EditorViewportShaderRecipe.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MultiviewShaderParameters.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "ShaderReflectionCook.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    /** @copydoc LoadEditorVulkanCoverageReductionCook */
    Result<EditorVulkanCoverageReductionCook> LoadEditorVulkanCoverageReductionCook(const std::span<const std::uint8_t> payload,
                                                                                    const std::span<const std::uint8_t> metadata) {
        using Outcome = Result<EditorVulkanCoverageReductionCook>;
        const auto valid = Render::ValidateStereoCoverageReductionCook(payload, metadata);
        if (valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        return Outcome::Success({{payload.begin(), payload.end()}});
    }

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

    /** @copydoc EditorVulkanCameraCrossFadeShaderManifest */
    Render::ShaderManifest EditorVulkanCameraCrossFadeShaderManifest() {
        using namespace Render;
        return {.schemaVersion = 1,
                .sourceIdentity = "assets/shaders/editor/viewport_cross_fade.hlsl",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}},
                .parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanCameraCrossFadeReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanCameraCrossFadeReflectionNames() {
        return {{{1}, "type.CameraCrossFadeParameters", {{{1}, "DestinationWeight"}}}, {{2}, "SourceColor", {}}};
    }

    namespace {
        /** @brief Loads a standalone fullscreen program with position input and one RGBA output. */
        template <typename Cook>
        Result<Cook> LoadFullscreenProgram(const Render::ShaderManifest &manifest, std::span<const std::uint8_t> payload,
                                           std::span<const std::uint8_t> metadata) {
            using namespace Render;
            using Outcome = Result<Cook>;
            const auto target = EditorVulkanViewportShaderTarget();
            if (const auto valid = ValidateViewportStageEntries(manifest, target, payload); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            auto decoded = DecodeReflectedShaderArtifact(manifest, target, payload, metadata);
            if (decoded.HasError())
                return Outcome::Failure(decoded.ErrorValue());
            const auto attributes = std::span{decoded.Value().reflection.stageInterface};
            const auto exact = [&](ShaderStage stage, ShaderInterfaceDirection direction, std::uint32_t columns) {
                return std::ranges::count_if(attributes, [=](const auto &value) {
                    return value.stage == stage && value.direction == direction && value.location == 0 && value.rows == 1 &&
                           value.columns == columns && value.type == ShaderValueType::Float32 && value.arrayCount == 1;
                }) == 1;
            };
            if (attributes.size() != 2 || !exact(ShaderStage::Vertex, ShaderInterfaceDirection::Input, 3) ||
                !exact(ShaderStage::Fragment, ShaderInterfaceDirection::Output, 4))
                return Outcome::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
            return Outcome::Success({{payload.begin(), payload.end()}, std::move(decoded).Value()});
        }
    }  // namespace

    /** @copydoc LoadEditorVulkanCameraCrossFadeCook */
    Result<EditorVulkanCameraCrossFadeCook> LoadEditorVulkanCameraCrossFadeCook(std::span<const std::uint8_t> payload,
                                                                                std::span<const std::uint8_t> metadata) {
        return LoadFullscreenProgram<EditorVulkanCameraCrossFadeCook>(EditorVulkanCameraCrossFadeShaderManifest(), payload, metadata);
    }

    /** @copydoc EditorVulkanXrComfortShaderManifest */
    Render::ShaderManifest EditorVulkanXrComfortShaderManifest() {
        using namespace Render;
        return {.schemaVersion = 1,
                .sourceIdentity = "assets/shaders/runtime/xr_comfort_vignette.hlsl",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}},
                .parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1}, {{2}, {1}, ShaderValueType::Float32, 1, 4, 1}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanXrComfortReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanXrComfortReflectionNames() {
        return {{{1}, "type.XrComfortParameters", {{{1}, "EyeRegion"}, {{2}, "Comfort"}}}};
    }

    /** @copydoc LoadEditorVulkanXrComfortCook */
    Result<EditorVulkanXrComfortCook> LoadEditorVulkanXrComfortCook(std::span<const std::uint8_t> payload,
                                                                    std::span<const std::uint8_t> metadata) {
        return LoadFullscreenProgram<EditorVulkanXrComfortCook>(EditorVulkanXrComfortShaderManifest(), payload, metadata);
    }

    /** @copydoc EditorVulkanWorldCanvasShaderManifest */
    Render::ShaderManifest EditorVulkanWorldCanvasShaderManifest() {
        using namespace Render;
        return {.schemaVersion = 1,
                .sourceIdentity = "assets/shaders/editor/viewport_world_canvas.hlsl",
                .sourceRevision = 2,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex},
                             {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}},
                .parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{2}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{3}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{4}, {1}, ShaderValueType::Float32, 1, 4, 1}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanWorldCanvasReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanWorldCanvasReflectionNames() {
        return {{{1},
                 "type.WorldCanvasParameters",
                 {{{1}, "ClipFromLocalX"}, {{2}, "ClipFromLocalY"}, {{3}, "ClipFromLocalZ"}, {{4}, "ClipFromLocalOrigin"}}},
                {{2}, "CanvasColor", {}},
                {{3}, "CanvasSampler", {}}};
    }

    namespace {
        /** @brief Validates vertex position/UV, the exact linked UV and one premultiplied color output. */
        bool ValidWorldCanvasInterface(const Render::NormalizedShaderReflection &reflection) {
            using namespace Render;
            using enum ShaderInterfaceDirection;

            struct Attribute final {
                ShaderStage stage;
                ShaderInterfaceDirection direction;
                std::uint32_t location, columns;
            };

            constexpr std::array expected{Attribute{ShaderStage::Vertex, Input, 0, 3}, Attribute{ShaderStage::Vertex, Input, 2, 2},
                                          Attribute{ShaderStage::Vertex, Output, 0, 2}, Attribute{ShaderStage::Fragment, Input, 0, 2},
                                          Attribute{ShaderStage::Fragment, Output, 0, 4}};
            return reflection.stageInterface.size() == expected.size() && std::ranges::all_of(expected, [&](const auto &role) {
                return std::ranges::count_if(reflection.stageInterface, [&](const auto &actual) {
                    return actual.stage == role.stage && actual.direction == role.direction && actual.location == role.location &&
                           actual.rows == 1 && actual.columns == role.columns && actual.type == ShaderValueType::Float32 &&
                           actual.arrayCount == 1;
                }) == 1;
            });
        }
    }  // namespace

    /** @copydoc LoadEditorVulkanWorldCanvasCook */
    Result<EditorVulkanWorldCanvasCook> LoadEditorVulkanWorldCanvasCook(const std::span<const std::uint8_t> payload,
                                                                        const std::span<const std::uint8_t> metadata) {
        using Outcome = Result<EditorVulkanWorldCanvasCook>;
        const auto manifest = EditorVulkanWorldCanvasShaderManifest();
        const auto target = EditorVulkanViewportShaderTarget();
        if (const auto valid = ValidateViewportStageEntries(manifest, target, payload); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        auto decoded = Render::DecodeReflectedShaderArtifact(manifest, target, payload, metadata);
        if (decoded.HasError())
            return Outcome::Failure(decoded.ErrorValue());
        if (!ValidWorldCanvasInterface(decoded.Value().reflection))
            return Outcome::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
        return Outcome::Success({{payload.begin(), payload.end()}, std::move(decoded).Value()});
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

    namespace {
        /** @brief Checks that one raster endpoint exposes exactly one attribute of the expected shape. */
        bool HasTemporalEndpoint(const Render::NormalizedShaderReflection &reflection, const Render::ShaderStage stage,
                                 const Render::ShaderInterfaceDirection direction, const unsigned columns) {
            const auto matches = [stage, direction](const auto &attribute) {
                return attribute.stage == stage && attribute.direction == direction;
            };
            const auto &attributes = reflection.stageInterface;
            const auto attribute = std::ranges::find_if(attributes, matches);
            return attribute != attributes.end() && attribute->location == 0 && attribute->type == Render::ShaderValueType::Float32 &&
                   attribute->rows == 1 && attribute->columns == columns && attribute->arrayCount == 1 &&
                   std::none_of(attribute + 1, attributes.end(), matches);
        }

        /** @brief Exercises the same reflected uniform block used to prepare a live guide draw. */
        Result<std::size_t> ValidateTemporalUniformLayout(const Render::NormalizedShaderReflection &reflection,
                                                          const EditorVulkanTemporalInput input) {
            const auto layout = Render::PrepareMaterialBindingLayout(reflection);
            if (layout.HasError())
                return Result<std::size_t>::Failure(layout.ErrorValue());
            constexpr auto identity = Math::Mat4::Identity();
            std::array<std::byte, 3 * sizeof(identity) + 16> uniform{};
            return input == EditorVulkanTemporalInput::Motion ? PackEditorVulkanTemporalMotion(layout.Value(), identity, {}, {}, uniform)
                                                              : PackEditorVulkanTemporalDepth(layout.Value(), identity, identity, uniform);
        }
    }  // namespace

    /** @copydoc ValidateEditorVulkanTemporalInputCook */
    Result<Render::ReflectedShaderArtifact> ValidateEditorVulkanTemporalInputCook(EditorVulkanTemporalInput input,
                                                                                  std::span<const std::uint8_t> payload,
                                                                                  std::span<const std::uint8_t> metadata) {
        using namespace Render;
        using Outcome = Result<ReflectedShaderArtifact>;
        if (const auto state = EditorVulkanTemporalInputPipelineState(input); state.HasError())
            return Outcome::Failure(state.ErrorValue());
        const auto manifest = EditorVulkanTemporalInputShaderManifest(input);
        const auto target = EditorVulkanViewportShaderTarget();
        if (const auto stages = ValidateViewportStageEntries(manifest, target, payload); stages.HasError())
            return Outcome::Failure(stages.ErrorValue());
        auto result = DecodeReflectedShaderArtifact(manifest, target, payload, metadata);
        if (result.HasError())
            return result;
        const auto &reflection = result.Value().reflection;
        if (const unsigned columns = input == EditorVulkanTemporalInput::Motion ? 2U : 1U;
            !HasTemporalEndpoint(reflection, ShaderStage::Vertex, ShaderInterfaceDirection::Input, 3) ||
            !HasTemporalEndpoint(reflection, ShaderStage::Fragment, ShaderInterfaceDirection::Output, columns))
            return Outcome::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
        if (const auto packed = ValidateTemporalUniformLayout(reflection, input); packed.HasError())
            return Outcome::Failure(packed.ErrorValue());
        return result;
    }

    /** @copydoc EditorVulkanTemporalInputPipelineState */
    Result<Render::RenderGraphicsPipelineState> EditorVulkanTemporalInputPipelineState(const EditorVulkanTemporalInput input) {
        using namespace Render;
        using Outcome = Result<RenderGraphicsPipelineState>;
        using enum EditorVulkanTemporalInput;
        using enum RenderTextureFormat;
        if (input != LinearDepth && input != Motion)
            return Outcome::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
        return Outcome::Success(
            {.cull = RenderCullMode::None, .colorFormat = input == LinearDepth ? R32Float : Rg32Float, .depthFormat = Depth32Float});
    }

    namespace {
        /** @brief Packs mathematical row-major components for the canonical reflected material writer. */
        std::array<float, 16> TemporalMatrix(const Math::Mat4 &matrix, const bool flipY = false) {
            std::array<float, 16> values;
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    values[static_cast<std::size_t>(row * 4 + column)] =
                        flipY && row == 1 ? -matrix.At(row, column) : matrix.At(row, column);
            return values;
        }
    }  // namespace

    /** @copydoc PackEditorVulkanTemporalDepth */
    Result<std::size_t> PackEditorVulkanTemporalDepth(const Render::MaterialBindingLayout &layout, const Math::Mat4 &rasterLocalToClip,
                                                      const Math::Mat4 &localToView, std::span<std::byte> destination) {
        using namespace Render;
        if (!Math::IsFinite(rasterLocalToClip) || !Math::IsFinite(localToView))
            return Result<std::size_t>::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
        const auto raster = TemporalMatrix(rasterLocalToClip);
        const auto view = TemporalMatrix(localToView);
        const std::array values{MaterialParameterValue{{1}, std::span<const float>{raster}},
                                MaterialParameterValue{{2}, std::span<const float>{view}}};
        return PackMaterialParameterBinding(layout, {1}, values, destination);
    }

    /** @copydoc PackEditorVulkanTemporalMotion */
    Result<std::size_t> PackEditorVulkanTemporalMotion(const Render::MaterialBindingLayout &layout, const Math::Mat4 &rasterLocalToClip,
                                                       const Render::RenderMotionCameraPair &camera,
                                                       const Render::RenderMotionObjectPair &object, std::span<std::byte> destination) {
        using namespace Render;
        const std::array<std::uint32_t, 1> history{camera.hasPrevious && object.hasPrevious ? 1U : 0U};
        const auto current = Math::Multiply(camera.currentUnjitteredViewProjection, object.currentLocalToWorld);
        const auto previous =
            history.front() ? Math::Multiply(camera.previousUnjitteredViewProjection, object.previousLocalToWorld) : current;
        if (!Math::IsFinite(rasterLocalToClip) || !Math::IsFinite(current) || !Math::IsFinite(previous))
            return Result<std::size_t>::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
        const auto raster = TemporalMatrix(rasterLocalToClip);
        const auto currentValues = TemporalMatrix(current, true);
        const auto previousValues = TemporalMatrix(previous, true);
        const std::array values{MaterialParameterValue{{1}, std::span<const float>{raster}},
                                MaterialParameterValue{{2}, std::span<const float>{currentValues}},
                                MaterialParameterValue{{3}, std::span<const float>{previousValues}},
                                MaterialParameterValue{{4}, std::span<const std::uint32_t>{history}}};
        return PackMaterialParameterBinding(layout, {1}, values, destination);
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

    /** @copydoc EditorVulkanMultiviewEnvironmentShaderManifest */
    Render::ShaderManifest EditorVulkanMultiviewEnvironmentShaderManifest() {
        using namespace Render;
        using namespace MultiviewShaderParameterIds;
        using enum ShaderValueType;
        auto manifest = EditorVulkanMultiviewShaderManifest();
        manifest.sourceIdentity = "assets/shaders/runtime/multiview_environment_mesh.hlsl";
        const std::array additions{ShaderParameter{NormalToWorld, Buffer, Float32, 4, 4, 1},
                                   ShaderParameter{EnvironmentAmbient, Buffer, Float32, 1, 4, 1},
                                   ShaderParameter{EnvironmentDirectional, Buffer, Float32, 1, 4, 1},
                                   ShaderParameter{EnvironmentDirection, Buffer, Float32, 1, 4, 1}};
        manifest.parameters.insert(manifest.parameters.end(), additions.begin(), additions.end());
        return manifest;
    }

    /** @copydoc EditorVulkanMultiviewEnvironmentReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewEnvironmentReflectionNames() {
        using namespace Render::MultiviewShaderParameterIds;
        return {{Buffer,
                 "type.MultiviewEnvironmentParameters",
                 {{WorldToClip, "WorldToClip"},
                  {LocalToWorld, "LocalToWorld"},
                  {ViewLayers, "ViewLayers"},
                  {NormalToWorld, "NormalToWorld"},
                  {EnvironmentAmbient, "EnvironmentAmbient"},
                  {EnvironmentDirectional, "EnvironmentDirectional"},
                  {EnvironmentDirection, "EnvironmentDirection"}}}};
    }

    /** @copydoc LoadEditorVulkanMultiviewEnvironmentCook */
    Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewEnvironmentCook(const std::span<const std::uint8_t> payload,
                                                                               const std::span<const std::uint8_t> metadata) {
        using namespace Render;
        using Outcome = Result<EditorVulkanMultiviewCook>;
        auto reflection = DecodeReflectedShaderArtifact(EditorVulkanMultiviewEnvironmentShaderManifest(),
                                                        EditorVulkanViewportShaderTarget(), payload, metadata);
        if (reflection.HasError())
            return Outcome::Failure(reflection.ErrorValue());
        if (const auto vertices = ValidateViewportMeshLayout(reflection.Value().reflection); vertices.HasError())
            return Outcome::Failure(vertices.ErrorValue());
        const auto layout = PrepareMaterialBindingLayout(reflection.Value().reflection);
        if (layout.HasError())
            return Outcome::Failure(layout.ErrorValue());
        const auto bytes = MultiviewEnvironmentParameterBytes(layout.Value(), 2);
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        if (bytes.Value() == 0 || bytes.Value() > 4096)
            return Outcome::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
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
