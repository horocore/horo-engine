#pragma once

/** @file PostProcessGraph.h
 * @brief Owned post-process graph plans preserving view, color, exposure, and history generations.
 */

#include "Horo/Runtime/Render/PostProcessSettings.h"
#include "Horo/Runtime/Render/RenderCapabilities.h"
#include "Horo/Runtime/Render/RenderGraph.h"
#include "Horo/Runtime/Render/TemporalHistory.h"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Render {
    /** @brief Scene-referred effects owned by this model; output transforms and accessibility have separate owners. */
    enum class PostProcessEffect : std::uint8_t {
        AmbientOcclusion,
        Reflections,
        DepthOfField,
        MotionBlur,
        Bloom,
        Vignette,
        ChromaticAberration,
        FilmGrain,
        Count
    };

    /** @brief Explicit color representation; data resources use Data and do not infer color from format. */
    enum class PostProcessRepresentation : std::uint8_t {
        Data,
        UnexposedAcesCg,
        PreExposedAcesCg,
        DisplayLinear,
        TransferEncoded
    };

    /** @brief Typed semantic inputs supplied by the active raster and history owners. */
    enum class PostProcessSemantic : std::uint8_t {
        SceneColor,
        LinearDepth,
        Normal,
        Roughness,
        Velocity,
        HistoryColor,
        Count
    };

    /** @brief An exact borrowed resident input with an immutable descriptor and complete frame compatibility metadata. */
    struct PostProcessTextureInput {
        PostProcessSemantic semantic{PostProcessSemantic::SceneColor};
        RenderTextureHandle texture;
        RenderTextureDescriptor descriptor;
        PostProcessRepresentation representation{PostProcessRepresentation::Data};
        TemporalHistoryCompatibility compatibility;
    };

    /** @brief Borrowed exposure state published by the view exposure owner, never calculated by this model. */
    struct PostProcessExposureInput {
        RenderBufferHandle buffer;
        RenderViewId view;
        std::uint64_t generation{0};
    };

    /** @brief One authored recipe preference with an exact cooked variant and finite declared work/reservation bounds. */
    struct PostProcessRecipe {
        PostProcessEffect effect{PostProcessEffect::Bloom};
        std::uint64_t cookedVariant{0};
        RenderCapabilitySet requiredCapabilities;
        RenderPassKind passKind{RenderPassKind::Graphics};
        std::uint64_t maximumWorkItems{0};
        std::uint64_t reservedBytes{0}; /**< Must cover the output image; includes recipe intermediates retained by its later executor. */
    };

    /** @brief Immutable authoring input; recipes are ordered explicit preferences per effect, not inferred profile tiers. */
    struct PostProcessGraphRequest {
        std::uint64_t settingsGeneration{0};
        PostProcessSettings settings;
        TemporalHistoryCompatibility compatibility;
        RenderCapabilitySnapshot capabilities;
        std::span<const PostProcessTextureInput> inputs;
        std::optional<PostProcessExposureInput> exposure;
        std::span<const PostProcessRecipe> recipes; /**< At most 32, with unique variant identities per effect. */
        std::array<PostProcessEffect, 8> order{PostProcessEffect::AmbientOcclusion,
                                               PostProcessEffect::Reflections,
                                               PostProcessEffect::DepthOfField,
                                               PostProcessEffect::MotionBlur,
                                               PostProcessEffect::Bloom,
                                               PostProcessEffect::Vignette,
                                               PostProcessEffect::ChromaticAberration,
                                               PostProcessEffect::FilmGrain};
        std::uint64_t maximumWorkItems{0};
        std::uint64_t maximumReservedBytes{0};
    };

    /** @brief Exact mapping of an admitted effect recipe to graph records and owned resolved settings. */
    struct PostProcessPass {
        PostProcessEffect effect;
        PostProcessRecipe recipe;
        RenderGraphPassRef pass;
        RenderGraphResourceId output;
        RenderTextureDescriptor descriptor;
        PostProcessRepresentation representation;
    };

    /** @brief Immutable graph candidate; resident handles are borrowed, graph records and settings are owned. */
    class PostProcessGraphPlan final {
    public:
        /** @brief Returns the owned logical graph. @return Empty for a no-effect pass-through candidate. */
        [[nodiscard]] const std::optional<RenderGraph> &Graph() const noexcept;
        /** @brief Returns recipe/pass mappings. @return Immutable bounded mappings in requested effect order. */
        [[nodiscard]] std::span<const PostProcessPass> Passes() const noexcept;
        /** @brief Resolves a typed input semantic. @param semantic Admitted semantic role.
         * @return Imported resource ID, or invalid when absent or this is a no-effect pass-through.
         */
        [[nodiscard]] RenderGraphResourceId Input(PostProcessSemantic semantic) const noexcept;
        /** @brief Returns resolved settings. @return Immutable owned settings used to prepare every pass. */
        [[nodiscard]] const PostProcessSettings &Settings() const noexcept;
        /** @brief Returns metadata preserved on every color edge. @return Exact supplied view/history compatibility. */
        [[nodiscard]] const TemporalHistoryCompatibility &Compatibility() const noexcept;
        /** @brief Returns the candidate settings revision. @return Non-zero authored generation. */
        [[nodiscard]] std::uint64_t SettingsGeneration() const noexcept;
        /** @brief Returns final scene color. @return Exported graph resource, or an invalid graph ID for a no-effect pass-through. */
        [[nodiscard]] RenderGraphResourceId SceneColor() const noexcept;
        /** @brief Returns original scene color. @return Borrowed source texture used directly when Graph is empty. */
        [[nodiscard]] RenderTextureHandle SourceSceneColor() const noexcept;
        /** @brief Returns the independent ambient visibility input for host lighting composition. @return Empty when AO is disabled. */
        [[nodiscard]] std::optional<RenderGraphResourceId> AmbientVisibility() const noexcept;

    private:
        friend Result<PostProcessGraphPlan> PreparePostProcessGraph(const PostProcessGraphRequest &);
        /** @brief Adopts a fully validated candidate and its exact owned settings/resource mappings. */
        PostProcessGraphPlan(std::optional<RenderGraph> graph, std::vector<PostProcessPass> passes, const PostProcessGraphRequest &request,
                             RenderGraphResourceId color, std::optional<RenderGraphResourceId> visibility, RenderTextureHandle source,
                             std::array<RenderGraphResourceId, 6> inputs) noexcept;
        std::optional<RenderGraph> graph_;
        std::vector<PostProcessPass> passes_;
        PostProcessSettings settings_;
        TemporalHistoryCompatibility compatibility_;
        std::uint64_t generation_;
        RenderGraphResourceId color_;
        std::optional<RenderGraphResourceId> visibility_;
        RenderTextureHandle source_;
        std::array<RenderGraphResourceId, 6> inputs_{};
    };

    /** @brief Prepares a fresh bounded graph, selecting only explicitly authored recipe preferences.
     * Synchronous authoring work may run on any thread. It owns no jobs or native objects and never
     * submits GPU work. The host publishes a successful candidate at its render safe point and retains
     * previous generations for in-flight frames. Failed/cancelled candidates are discarded without
     * mutating active state. Borrowed resident resources must outlive graph submission and retire
     * through their existing renderer owner. AO is exported separately for lighting integration;
     * the remaining passes preserve the input ACEScg representation and exposure generation.
     * @param request Complete resolved settings, semantic resources, compatibility, recipes and budgets.
     * @return Owned graph plan or a typed validation, input, unsupported-recipe, or allocation failure.
     */
    [[nodiscard]] Result<PostProcessGraphPlan> PreparePostProcessGraph(const PostProcessGraphRequest &request);
}  // namespace Horo::Render
