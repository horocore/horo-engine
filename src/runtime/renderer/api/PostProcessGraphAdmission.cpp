#include "Horo/Runtime/Render/PostProcessErrors.h"
#include "PostProcessGraphInternal.h"

#include <algorithm>
#include <ranges>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Bounded enum indexing used only after admission. */
        template <typename T> [[nodiscard]] constexpr std::size_t Index(const T value) noexcept {
            return static_cast<std::size_t>(value);
        }

        /** @brief Admits a complete ordering permutation and bounded immutable frame context. */
        [[nodiscard]] bool ValidRequest(const PostProcessGraphRequest &request) noexcept {
            if (request.settingsGeneration == 0 || !request.compatibility.IsValid() || !request.capabilities.IsValid() ||
                request.compatibility.deviceGeneration != request.capabilities.deviceIncarnation || request.maximumWorkItems == 0 ||
                request.maximumReservedBytes == 0 || request.compatibility.renderExtent.width > 16'384 ||
                request.compatibility.renderExtent.height > 16'384 || request.recipes.size() > 32 || request.inputs.size() > 6)
                return false;
            std::array<bool, 8> seen{};
            for (const PostProcessEffect effect : request.order) {
                if (Index(effect) >= seen.size() || seen[Index(effect)])
                    return false;
                seen[Index(effect)] = true;
            }
            return true;
        }

        /** @brief Requires canonical data formats rather than inferring semantic roles from native attachments. */
        [[nodiscard]] bool ValidFormat(const PostProcessSemantic semantic, const RenderTextureFormat format) noexcept {
            using enum PostProcessSemantic;
            using enum RenderTextureFormat;
            switch (semantic) {
                case SceneColor:
                case HistoryColor:
                    return format == Rgba16Float;
                case LinearDepth:
                    return format == R16Float || format == R32Float;
                case Normal:
                    return format == Rgba16Float || format == Rgba32Float;
                case Roughness:
                    return format == R8Unorm || format == R16Float;
                case Velocity:
                    return format == Rg16Float || format == Rg32Float;
                default:
                    return false;
            }
        }

        /** @brief Validates every supplied semantic input and exact compatibility before importing resident handles. */
        [[nodiscard]] Result<void> AdmitInputs(const PostProcessGraphRequest &request, PostProcessAdmission &admission) {
            for (const PostProcessTextureInput &input : request.inputs) {
                const std::size_t index = Index(input.semantic);
                if (index >= admission.inputs.size() || admission.inputs[index] || !input.texture.IsValid() ||
                    !input.descriptor.IsValid() || input.descriptor.dimension != RenderTextureDimension::TwoD ||
                    input.descriptor.extent != request.compatibility.renderExtent || input.descriptor.sampleCount != 1 ||
                    input.descriptor.layerCount != 1 || input.descriptor.mipCount != 1 ||
                    !HasTextureUsage(input.descriptor.usage, RenderTextureUsage::Sampled) ||
                    !ValidFormat(input.semantic, input.descriptor.format) || input.compatibility != request.compatibility)
                    return Result<void>::Failure(MakeError(PostProcessErrors::IncompatibleInput));
                if (const bool color =
                        input.semantic == PostProcessSemantic::SceneColor || input.semantic == PostProcessSemantic::HistoryColor;
                    color ? (input.representation != PostProcessRepresentation::UnexposedAcesCg &&
                             input.representation != PostProcessRepresentation::PreExposedAcesCg)
                          : input.representation != PostProcessRepresentation::Data)
                    return Result<void>::Failure(MakeError(PostProcessErrors::IncompatibleInput));
                for (const auto *other : admission.inputs) {
                    if (other && other->texture == input.texture)
                        return Result<void>::Failure(MakeError(PostProcessErrors::IncompatibleInput));
                }
                if (!request.capabilities.Supports(input.descriptor))
                    return Result<void>::Failure(MakeError(PostProcessErrors::UnsupportedRecipe));
                admission.inputs[index] = &input;
            }
            const auto *scene = admission.inputs[Index(PostProcessSemantic::SceneColor)];
            const auto *history = admission.inputs[Index(PostProcessSemantic::HistoryColor)];
            if (!scene)
                return Result<void>::Failure(MakeError(PostProcessErrors::MissingInput));
            if (history && history->representation != scene->representation)
                return Result<void>::Failure(MakeError(PostProcessErrors::IncompatibleInput));
            if (request.exposure.has_value() &&
                (!request.exposure->buffer.IsValid() || request.exposure->view != request.compatibility.view ||
                 request.exposure->generation != request.compatibility.exposureGeneration))
                return Result<void>::Failure(MakeError(PostProcessErrors::IncompatibleInput));
            return Result<void>::Success();
        }

        /** @brief Converts authored settings into a conservative bounded work multiplier. */
        [[nodiscard]] std::uint64_t WorkPerPixel(const PostProcessSettings &s, const PostProcessEffect effect) noexcept {
            using enum PostProcessEffect;
            switch (effect) {
                case AmbientOcclusion:
                    return s.ambientOcclusion->sampleCount;
                case Reflections:
                    return s.reflections->maxSteps;
                case DepthOfField:
                    return s.depthOfField->sampleCount;
                case MotionBlur:
                    return s.motionBlur->sampleCount;
                case Bloom:
                    return s.bloom->downsampleLevels * 2U;
                default:
                    return 1;
            }
        }

        /** @brief Rejects malformed recipe declarations even when a different preference would be supported. */
        [[nodiscard]] bool ValidRecipes(const std::span<const PostProcessRecipe> recipes) noexcept {
            for (std::size_t i = 0; i < recipes.size(); ++i) {
                const auto &recipe = recipes[i];
                if (Index(recipe.effect) >= 8 || recipe.cookedVariant == 0 || !recipe.requiredCapabilities.IsValid() ||
                    (recipe.passKind != RenderPassKind::Graphics && recipe.passKind != RenderPassKind::Compute) ||
                    recipe.maximumWorkItems == 0 || recipe.reservedBytes == 0)
                    return false;
                if (std::ranges::any_of(recipes.first(i), [&](const auto &r) {
                    return r.effect == recipe.effect && r.cookedVariant == recipe.cookedVariant;
                }))
                    return false;
            }
            return true;
        }

        /** @brief Tests one authored recipe without changing backend, effect algorithm, color or exposure identity. */
        [[nodiscard]] bool Supported(const PostProcessGraphRequest &request, const PostProcessRecipe &recipe, const std::uint64_t work,
                                     const std::uint64_t remainingBytes, const std::uint64_t remainingWork) noexcept {
            const bool compute = recipe.passKind == RenderPassKind::Compute;
            const auto &capabilities = request.capabilities;
            const auto descriptor = PostProcessOutputDescriptor(request, recipe);
            const std::uint64_t pixels = static_cast<std::uint64_t>(descriptor.extent.width) * descriptor.extent.height;
            const std::uint64_t minimumBytes = pixels * (recipe.effect == PostProcessEffect::AmbientOcclusion ? 1U : 8U);
            return (capabilities.features.bits & recipe.requiredCapabilities.bits) == recipe.requiredCapabilities.bits &&
                   (!compute || capabilities.features.Supports(RenderCapability::Compute)) &&
                   (compute ? capabilities.queues.compute : capabilities.queues.graphics) && capabilities.Supports(descriptor) &&
                   recipe.reservedBytes >= minimumBytes && recipe.reservedBytes <= remainingBytes && work <= recipe.maximumWorkItems &&
                   work <= remainingWork;
        }

        /** @brief Selects the first admitted authored preference per enabled effect within cumulative budgets. */
        [[nodiscard]] Result<void> AdmitRecipes(const PostProcessGraphRequest &request, PostProcessAdmission &admission) {
            if (!ValidRecipes(request.recipes))
                return Result<void>::Failure(MakeError(PostProcessErrors::InvalidGraph));
            std::uint64_t bytes = request.maximumReservedBytes;
            std::uint64_t work = request.maximumWorkItems;
            const std::uint64_t pixels =
                static_cast<std::uint64_t>(request.compatibility.renderExtent.width) * request.compatibility.renderExtent.height;
            for (const PostProcessEffect effect : request.order) {
                if (!PostProcessEnabled(request.settings, effect))
                    continue;
                for (const PostProcessSemantic semantic : PostProcessRequiredInputs(effect)) {
                    if (!admission.inputs[Index(semantic)])
                        return Result<void>::Failure(MakeError(PostProcessErrors::MissingInput));
                }
                if (effect == PostProcessEffect::Bloom && !request.exposure.has_value())
                    return Result<void>::Failure(MakeError(PostProcessErrors::MissingInput));
                const std::uint64_t requiredWork = pixels * WorkPerPixel(request.settings, effect);
                const auto selected = std::ranges::find_if(request.recipes, [&](const auto &recipe) {
                    return recipe.effect == effect && Supported(request, recipe, requiredWork, bytes, work);
                });
                if (selected == request.recipes.end())
                    return Result<void>::Failure(MakeError(PostProcessErrors::UnsupportedRecipe));
                admission.recipes[Index(effect)] = *selected;
                bytes -= selected->reservedBytes;
                work -= requiredWork;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PostProcessEnabled */
    bool PostProcessEnabled(const PostProcessSettings &s, const PostProcessEffect effect) noexcept {
        using enum PostProcessEffect;
        switch (effect) {
            case AmbientOcclusion:
                return s.ambientOcclusion.has_value();
            case Reflections:
                return s.reflections.has_value();
            case DepthOfField:
                return s.depthOfField.has_value();
            case MotionBlur:
                return s.motionBlur.has_value();
            case Bloom:
                return s.bloom.has_value();
            case Vignette:
                return s.vignette.has_value();
            case ChromaticAberration:
                return s.chromaticAberration.has_value();
            case FilmGrain:
                return s.filmGrain.has_value();
            default:
                return false;
        }
    }

    /** @copydoc PostProcessRequiredInputs */
    std::span<const PostProcessSemantic> PostProcessRequiredInputs(const PostProcessEffect effect) noexcept {
        using enum PostProcessSemantic;
        static constexpr std::array ao{LinearDepth, Normal};
        static constexpr std::array reflections{SceneColor, LinearDepth, Normal, Roughness, HistoryColor};
        static constexpr std::array dof{SceneColor, LinearDepth};
        static constexpr std::array motion{SceneColor, LinearDepth, Velocity, HistoryColor};
        static constexpr std::array color{SceneColor};
        switch (effect) {
            case PostProcessEffect::AmbientOcclusion:
                return ao;
            case PostProcessEffect::Reflections:
                return reflections;
            case PostProcessEffect::DepthOfField:
                return dof;
            case PostProcessEffect::MotionBlur:
                return motion;
            default:
                return color;
        }
    }

    /** @copydoc PostProcessOutputDescriptor */
    RenderTextureDescriptor PostProcessOutputDescriptor(const PostProcessGraphRequest &request, const PostProcessRecipe &recipe) noexcept {
        using enum RenderTextureUsage;
        return {.extent = request.compatibility.renderExtent,
                .format =
                    recipe.effect == PostProcessEffect::AmbientOcclusion ? RenderTextureFormat::R8Unorm : RenderTextureFormat::Rgba16Float,
                .usage = Sampled | (recipe.passKind == RenderPassKind::Compute ? Storage : RenderAttachment)};
    }

    /** @copydoc AdmitPostProcessGraph */
    Result<PostProcessAdmission> AdmitPostProcessGraph(const PostProcessGraphRequest &request) {
        if (!ValidRequest(request))
            return Result<PostProcessAdmission>::Failure(MakeError(PostProcessErrors::InvalidGraph));
        if (auto valid = ValidatePostProcessSettings(request.settings); valid.HasError())
            return Result<PostProcessAdmission>::Failure(valid.ErrorValue());
        PostProcessAdmission admitted;
        if (auto valid = AdmitInputs(request, admitted); valid.HasError())
            return Result<PostProcessAdmission>::Failure(valid.ErrorValue());
        if (auto valid = AdmitRecipes(request, admitted); valid.HasError())
            return Result<PostProcessAdmission>::Failure(valid.ErrorValue());
        return Result<PostProcessAdmission>::Success(admitted);
    }
}  // namespace Horo::Render::Detail
