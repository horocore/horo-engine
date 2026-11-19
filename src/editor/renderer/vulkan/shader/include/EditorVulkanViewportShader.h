#pragma once
/** @file EditorVulkanViewportShader.h
 * @brief Portable authored viewport interface and final-artifact loading shared by offline cook and runtime consumer. */
#include "Horo/Runtime/Render/RenderShaderResources.h"
#include "ShaderReflectionCook.h"

namespace Horo::Editor {
    /** @brief The closed stereo reduction package after its bound reflection is validated. */
    struct EditorVulkanCoverageReductionCook final {
        std::vector<std::uint8_t> payload;
    };

    /** @brief Verifies the complete cooked Vulkan reduction program against its same-operation sidecar. */
    [[nodiscard]] Result<EditorVulkanCoverageReductionCook> LoadEditorVulkanCoverageReductionCook(std::span<const std::uint8_t> payload,
                                                                                                  std::span<const std::uint8_t> metadata);

    [[nodiscard]] Render::ShaderTargetRequirement EditorVulkanViewportShaderTarget();
    [[nodiscard]] Render::ShaderManifest EditorVulkanViewportShaderManifest();
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanViewportReflectionNames();

    /** @brief Exact cooked program for the opaque RGBA8 desktop camera compositor. */
    struct EditorVulkanCameraCrossFadeCook final {
        std::vector<std::uint8_t> payload;
        Render::ReflectedShaderArtifact metadata;
    };

    /** @brief Returns the cross-fade program's exact vertex, source texture and weight contract. */
    [[nodiscard]] Render::ShaderManifest EditorVulkanCameraCrossFadeShaderManifest();
    /** @brief Returns compiler-visible names for the actual weight and unfiltered source texture. */
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanCameraCrossFadeReflectionNames();
    /** @brief Validates actual same-operation Vulkan shader bytes and reflected material interface.
     * @param payload Exact cooked Vulkan stages. @param metadata Canonical reflection from the same cook.
     * @return Owned compositor cook or typed payload/interface failure. */
    [[nodiscard]] Result<EditorVulkanCameraCrossFadeCook> LoadEditorVulkanCameraCrossFadeCook(std::span<const std::uint8_t> payload,
                                                                                              std::span<const std::uint8_t> metadata);

    /** @brief Separate actual cooked depth artifact from the canonical shared shadow source. */
    struct EditorVulkanShadowCook {
        std::vector<std::uint8_t> payload;
        Render::ReflectedShaderArtifact metadata;
    };

    [[nodiscard]] Render::ShaderManifest EditorVulkanShadowShaderManifest();
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanShadowReflectionNames();
    [[nodiscard]] Result<EditorVulkanShadowCook> LoadEditorVulkanShadowCook(std::span<const std::uint8_t> payload,
                                                                            std::span<const std::uint8_t> metadata);

    /** @brief Immutable ready-to-load Vulkan package; native shader/pipeline realization belongs to the frontend. */
    struct EditorVulkanViewportCook {
        std::vector<std::uint8_t> payload;
        Render::ReflectedShaderArtifact metadata;
        std::optional<EditorVulkanShadowCook> shadow;
        std::optional<EditorVulkanCameraCrossFadeCook> cameraCrossFade;
    };

    /** @brief Rejects altered/mismatched cooked payload/layout before any native shader object is created. */
    [[nodiscard]] Result<EditorVulkanViewportCook> LoadEditorVulkanViewportCook(std::span<const std::uint8_t> payload,
                                                                                std::span<const std::uint8_t> metadata);

    /** @brief Owning cold recipe for the opaque scene-only FP16 Vulkan raster program. */
    struct EditorVulkanSceneLinearCook final {
        std::vector<std::uint8_t> payload;
        Render::ReflectedShaderArtifact metadata;
    };

    /** @brief Returns the same scene mesh/light/shadow ABI with the exact unexposed scene fragment stage. */
    [[nodiscard]] Render::ShaderManifest EditorVulkanSceneLinearShaderManifest();

    /** @brief Returns the exact single-view FP16 color and native depth pipeline state for this program. */
    [[nodiscard]] Render::RenderGraphicsPipelineState EditorVulkanSceneLinearPipelineState() noexcept;

    /** @brief Loads only the actual scene-linear cook and its bound reflection.
     * @param payload Exact Vulkan stage payload from compilation.
     * @param metadata Canonical same-operation reflection.
     * @return Owned scene-only program or typed target/stage/layout mismatch.
     */
    [[nodiscard]] Result<EditorVulkanSceneLinearCook> LoadEditorVulkanSceneLinearCook(std::span<const std::uint8_t> payload,
                                                                                      std::span<const std::uint8_t> metadata);

    /** @brief Single-target desktop prepasses providing the exact native temporal input semantics. */
    enum class EditorVulkanTemporalInput : std::uint8_t {
        LinearDepth,
        Motion,
    };

    /** @brief Describes the selected prepass's separate float attachment and reflected matrix ABI. */
    [[nodiscard]] Render::ShaderManifest EditorVulkanTemporalInputShaderManifest(EditorVulkanTemporalInput input);
    /** @brief Names the selected prepass's exact compiler-reflected block and members. */
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanTemporalInputReflectionNames(
        EditorVulkanTemporalInput input);
    /** @brief Checks the selected compiler-bound prepass and its exact vertex/fragment attachment interface.
     * @param input Selected depth or motion producer. @param payload Actual stage bytes. @param metadata Bound reflection.
     * @return Exact admitted reflection or a typed target, stage, binding or layout failure. */
    [[nodiscard]] Result<Render::ReflectedShaderArtifact> ValidateEditorVulkanTemporalInputCook(EditorVulkanTemporalInput input,
                                                                                                std::span<const std::uint8_t> payload,
                                                                                                std::span<const std::uint8_t> metadata);

    /** @brief Exact canonical runtime multiview interface, consumed through the same compiled/reflected producer. */
    [[nodiscard]] Render::ShaderManifest EditorVulkanMultiviewShaderManifest();
    /** @brief Exact authored member names; offsets and strides always come from actual compiler reflection. */
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewReflectionNames();

    /** @brief Ready selected-Vulkan multipass program; no native realization or availability is implied. */
    struct EditorVulkanMultiviewCook final {
        std::vector<std::uint8_t> payload;
        Render::ReflectedShaderArtifact metadata;
    };

    /** @brief Validates actual immutable payload, canonical reflection and bounded multiview uniform ABI before realization.
     * @param payload Same compile-boundary SPIR-V package. @param metadata Its canonical immutable reflected record.
     * @return Exact accepted multipass program or original altered/target/interface/layout error; no reflected layout is invented.
     */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewCook(std::span<const std::uint8_t> payload,
                                                                                  std::span<const std::uint8_t> metadata);
    /** @brief Distinct offline-compiled shader interface for the optional one-color motion plus depth pass. */
    [[nodiscard]] Render::ShaderManifest EditorVulkanMultiviewMotionShaderManifest();
    /** @brief Exact compiler-reflected block/member names for motion input. */
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewMotionReflectionNames();
    /** @brief Admits authenticated float4 motion output and both historic/current clip-transform arrays.
     * @param payload Same-operation SPIR-V package. @param metadata Exact reflected sidecar bytes.
     * @return Owned cook or a typed target/interface/packing failure. Performs no device work.
     */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewMotionCook(std::span<const std::uint8_t> payload,
                                                                                        std::span<const std::uint8_t> metadata);

    /** @brief Per-fragment predecessor depth observations encoded exactly in a renderer-owned R32Float attachment. */
    enum class EditorVulkanReprojectionCoverage : std::uint8_t {
        Unwritten = 0,
        ProjectionRejected = 1,
        PreviousDepthUnavailable = 2,
        Occluded = 3,
        DepthMismatch = 4,
        DepthConsistent = 5,
    };

    /** @brief Returns the per-eye coverage recipe sharing the exact current and predecessor clip transforms. */
    [[nodiscard]] Render::ShaderManifest EditorVulkanMultiviewCoverageShaderManifest();
    /** @brief Names the transform block and both unfiltered predecessor eye depth images. */
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewCoverageReflectionNames();
    /** @brief Checks the exact compiled coverage stages, sampled depth binding and scalar float color output.
     * @param payload Same-operation SPIR-V package. @param metadata Exact reflected sidecar bytes.
     * @return Owned coverage cook or a typed interface/packing error. Performs no device work.
     */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewCoverageCook(std::span<const std::uint8_t> payload,
                                                                                          std::span<const std::uint8_t> metadata);
}  // namespace Horo::Editor
