#include "Horo/Runtime/Render/PostProcessGraph.h"

#include "Horo/Runtime/Render/PostProcessErrors.h"
#include "PostProcessGraphInternal.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Keeps fresh candidate records private until all authoring and compilation succeeds. */
        struct Candidate {
            std::array<RenderGraphResourceId, 6> inputs{};
            RenderGraphResourceId exposure;
            RenderGraphResourceId color;
            std::optional<RenderGraphResourceId> visibility;
            std::optional<RenderGraphPassRef> previousColor;
            std::vector<PostProcessPass> passes;
        };

        /** @brief Imports exact Horo resident generations without taking their lifetime ownership. */
        [[nodiscard]] Result<void> ImportInputs(RenderGraphBuilder &builder, const PostProcessGraphRequest &request,
                                                const Detail::PostProcessAdmission &admission, Candidate &candidate) {
            for (std::size_t i = 0; i < admission.inputs.size(); ++i) {
                if (!admission.inputs[i])
                    continue;
                const auto resourceClass = i == static_cast<std::size_t>(PostProcessSemantic::HistoryColor)
                                               ? RenderGraphResourceClass::History
                                               : RenderGraphResourceClass::External;
                auto imported = builder.ImportTexture(admission.inputs[i]->texture, resourceClass);
                if (imported.HasError())
                    return Result<void>::Failure(imported.ErrorValue());
                candidate.inputs[i] = imported.Value();
            }
            candidate.color = candidate.inputs[static_cast<std::size_t>(PostProcessSemantic::SceneColor)];
            if (request.settings.bloom.has_value()) {
                auto imported = builder.ImportBuffer(request.exposure->buffer, RenderGraphResourceClass::External);
                if (imported.HasError())
                    return Result<void>::Failure(imported.ErrorValue());
                candidate.exposure = imported.Value();
            }
            return Result<void>::Success();
        }

        /** @brief Declares semantic reads using the most recent color output and original auxiliary inputs. */
        [[nodiscard]] Result<void> ReadInputs(RenderGraphBuilder &builder, const PostProcessEffect effect, const RenderGraphPassRef pass,
                                              const Candidate &candidate) {
            for (const PostProcessSemantic semantic : Detail::PostProcessRequiredInputs(effect)) {
                const auto resource =
                    semantic == PostProcessSemantic::SceneColor ? candidate.color : candidate.inputs[static_cast<std::size_t>(semantic)];
                if (auto used = builder.AddUsage({pass, resource, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled}); used.HasError())
                    return used;
            }
            if (effect == PostProcessEffect::Bloom)
                return builder.AddUsage({pass, candidate.exposure, RenderGraphAccess::Read, RenderGraphUsageKind::Storage});
            return Result<void>::Success();
        }

        /** @brief Appends one admitted logical effect without imposing color dependencies on ambient visibility. */
        [[nodiscard]] Result<void> AppendEffect(RenderGraphBuilder &builder, const PostProcessGraphRequest &request,
                                                const PostProcessRecipe &recipe, const PostProcessRepresentation colorRepresentation,
                                                Candidate &candidate) {
            const bool ao = recipe.effect == PostProcessEffect::AmbientOcclusion;
            const bool compute = recipe.passKind == RenderPassKind::Compute;
            auto pass = builder.AddPass(recipe.passKind, compute ? RenderQueueRole::Compute : RenderQueueRole::Graphics);
            if (pass.HasError())
                return Result<void>::Failure(pass.ErrorValue());
            auto output = builder.AddTransientResource(RenderGraphResourceKind::Texture);
            if (output.HasError())
                return Result<void>::Failure(output.ErrorValue());
            if (auto read = ReadInputs(builder, recipe.effect, pass.Value(), candidate); read.HasError())
                return read;
            if (auto written = builder.AddUsage({pass.Value(), output.Value(), RenderGraphAccess::Write,
                                                 compute ? RenderGraphUsageKind::Storage : RenderGraphUsageKind::ColorAttachment});
                written.HasError())
                return written;
            if (!ao && candidate.previousColor.has_value()) {
                if (auto dependency =
                        builder.AddDependency({*candidate.previousColor, pass.Value(), RenderGraphDependencyKind::ResourceHazard});
                    dependency.HasError())
                    return dependency;
            }
            if (ao) {
                candidate.visibility = output.Value();
            } else {
                candidate.color = output.Value();
                candidate.previousColor = pass.Value();
            }
            candidate.passes.emplace_back(recipe.effect, recipe, pass.Value(), output.Value(),
                                          Detail::PostProcessOutputDescriptor(request, recipe),
                                          ao ? PostProcessRepresentation::Data : colorRepresentation);
            return Result<void>::Success();
        }

        /** @brief Exports only observable outputs before finalization and validates their dependency schedule. */
        [[nodiscard]] Result<RenderGraph> Finalize(RenderGraphBuilder &builder, const Candidate &candidate) {
            if (auto exported = builder.ExportResource(candidate.color); exported.HasError())
                return Result<RenderGraph>::Failure(exported.ErrorValue());
            if (candidate.visibility.has_value()) {
                if (auto exported = builder.ExportResource(*candidate.visibility); exported.HasError())
                    return Result<RenderGraph>::Failure(exported.ErrorValue());
            }
            auto graph = builder.Finalize();
            if (graph.HasError())
                return graph;
            if (auto compiled = CompileRenderGraph(graph.Value()); compiled.HasError())
                return Result<RenderGraph>::Failure(compiled.ErrorValue());
            return graph;
        }
    }  // namespace

    /** @copydoc PostProcessGraphPlan::PostProcessGraphPlan */
    PostProcessGraphPlan::PostProcessGraphPlan(std::optional<RenderGraph> graph, std::vector<PostProcessPass> passes,
                                               const PostProcessGraphRequest &request, const RenderGraphResourceId color,
                                               const std::optional<RenderGraphResourceId> visibility, const RenderTextureHandle source,
                                               const std::array<RenderGraphResourceId, 6> &inputs) noexcept
        : graph_(std::move(graph)), passes_(std::move(passes)), settings_(request.settings), compatibility_(request.compatibility),
          generation_(request.settingsGeneration), color_(color), visibility_(visibility), source_(source), inputs_(inputs) {}

    /** @copydoc PostProcessGraphPlan::Graph */
    const std::optional<RenderGraph> &PostProcessGraphPlan::Graph() const noexcept {
        return graph_;
    }

    /** @copydoc PostProcessGraphPlan::Passes */
    std::span<const PostProcessPass> PostProcessGraphPlan::Passes() const noexcept {
        return passes_;
    }

    /** @copydoc PostProcessGraphPlan::Input */
    RenderGraphResourceId PostProcessGraphPlan::Input(const PostProcessSemantic semantic) const noexcept {
        const auto index = static_cast<std::size_t>(semantic);
        return index < inputs_.size() ? inputs_[index] : RenderGraphResourceId{};
    }

    /** @copydoc PostProcessGraphPlan::Settings */
    const PostProcessSettings &PostProcessGraphPlan::Settings() const noexcept {
        return settings_;
    }

    /** @copydoc PostProcessGraphPlan::Compatibility */
    const TemporalHistoryCompatibility &PostProcessGraphPlan::Compatibility() const noexcept {
        return compatibility_;
    }

    /** @copydoc PostProcessGraphPlan::SettingsGeneration */
    std::uint64_t PostProcessGraphPlan::SettingsGeneration() const noexcept {
        return generation_;
    }

    /** @copydoc PostProcessGraphPlan::SceneColor */
    RenderGraphResourceId PostProcessGraphPlan::SceneColor() const noexcept {
        return color_;
    }

    /** @copydoc PostProcessGraphPlan::SourceSceneColor */
    RenderTextureHandle PostProcessGraphPlan::SourceSceneColor() const noexcept {
        return source_;
    }

    /** @copydoc PostProcessGraphPlan::AmbientVisibility */
    std::optional<RenderGraphResourceId> PostProcessGraphPlan::AmbientVisibility() const noexcept {
        return visibility_;
    }

    /** @copydoc PreparePostProcessGraph */
    Result<PostProcessGraphPlan> PreparePostProcessGraph(const PostProcessGraphRequest &request) {
        auto admitted = Detail::AdmitPostProcessGraph(request);
        if (admitted.HasError())
            return Result<PostProcessGraphPlan>::Failure(admitted.ErrorValue());
        const auto &admission = admitted.Value();
        const auto &scene = *admission.inputs[static_cast<std::size_t>(PostProcessSemantic::SceneColor)];
        if (std::ranges::none_of(admission.recipes, [](const auto &recipe) {
            return recipe.has_value();
        }))
            return Result<PostProcessGraphPlan>::Success(PostProcessGraphPlan{{}, {}, request, {}, {}, scene.texture, {}});
        try {
            auto created = RenderGraphBuilder::Create({8, 16, 64, 8});
            if (created.HasError())
                return Result<PostProcessGraphPlan>::Failure(created.ErrorValue());
            auto builder = std::move(created).Value();
            Candidate candidate;
            candidate.passes.reserve(8);
            if (auto imported = ImportInputs(builder, request, admission, candidate); imported.HasError())
                return Result<PostProcessGraphPlan>::Failure(imported.ErrorValue());
            for (const PostProcessEffect effect : request.order) {
                const auto &recipe = admission.recipes[static_cast<std::size_t>(effect)];
                if (!recipe.has_value())
                    continue;
                if (auto appended = AppendEffect(builder, request, *recipe, scene.representation, candidate); appended.HasError())
                    return Result<PostProcessGraphPlan>::Failure(appended.ErrorValue());
            }
            auto graph = Finalize(builder, candidate);
            if (graph.HasError())
                return Result<PostProcessGraphPlan>::Failure(graph.ErrorValue());
            return Result<PostProcessGraphPlan>::Success(PostProcessGraphPlan{std::move(graph).Value(), std::move(candidate.passes),
                                                                              request, candidate.color, candidate.visibility, scene.texture,
                                                                              candidate.inputs});
        } catch (const std::bad_alloc &) {
            return Result<PostProcessGraphPlan>::Failure(MakeError(PostProcessErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
