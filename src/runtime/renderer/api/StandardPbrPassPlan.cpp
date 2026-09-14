#include "Horo/Runtime/Render/StandardPbrPassPlan.h"

#include "Horo/Runtime/Render/StandardPbrPassPlanErrors.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Reports whether the enum belongs to the public raster-family contract. */
        [[nodiscard]] constexpr bool IsKnown(const StandardPbrRasterFamily family) noexcept {
            return static_cast<std::uint8_t>(family) <= static_cast<std::uint8_t>(StandardPbrRasterFamily::Deferred);
        }

        /** @brief Reports whether a material belongs to the opaque/depth contract. */
        [[nodiscard]] constexpr bool IsOpaqueClass(const MaterialAlphaMode alphaMode) noexcept {
            return alphaMode == MaterialAlphaMode::Opaque || alphaMode == MaterialAlphaMode::Masked;
        }

        /** @brief Appends one non-empty logical batch over the canonical material table. */
        void AppendBatch(std::vector<StandardPbrPassBatch> &batches, const StandardPbrPassStage stage, const MaterialAlphaMode alphaMode,
                         const std::size_t firstMaterial, const std::size_t materialCount, const StandardPbrPassOutputs outputs) {
            if (materialCount == 0)
                return;
            batches.emplace_back(stage, alphaMode, firstMaterial, materialCount, outputs);
        }

        /** @brief Validates frontend policy before any owned plan storage is allocated. */
        [[nodiscard]] Result<void> ValidateRequest(const StandardPbrPassPlanRequest &request, const std::size_t materialCount) {
            if (!request.IsValid() || !IsKnown(request.family) ||
                (request.family == StandardPbrRasterFamily::ClusteredForward && !request.depthPrepass))
                return Result<void>::Failure(MakeError(StandardPbrPassPlanErrors::InvalidRequest));
            if (materialCount > request.maximumMaterials)
                return Result<void>::Failure(MakeError(StandardPbrPassPlanErrors::CapacityExceeded));
            return Result<void>::Success();
        }

        /** @brief Validates and canonicalizes exact material generations without retaining caller storage. */
        [[nodiscard]] Result<std::vector<StandardPbrPassMaterial>> BuildCanonicalMaterials(
            const std::span<const ResidentStandardPbrMaterial> materials) {
            std::vector<StandardPbrPassMaterial> canonical;
            canonical.reserve(materials.size());
            for (const ResidentStandardPbrMaterial &material : materials) {
                if (!IsOpaqueClass(material.alphaMode))
                    return Result<std::vector<StandardPbrPassMaterial>>::Failure(
                        MakeError(StandardPbrPassPlanErrors::UnsupportedMaterialClass));
                if (!material.id.IsValid() || material.sourceRevision == 0 || material.generation == 0 || !material.pipeline.IsValid())
                    return Result<std::vector<StandardPbrPassMaterial>>::Failure(MakeError(StandardPbrPassPlanErrors::InvalidMaterial));
                canonical.emplace_back(material.id, material.sourceRevision, material.generation, material.pipeline, material.alphaMode);
            }
            std::ranges::sort(canonical, {}, &StandardPbrPassMaterial::id);
            if (std::ranges::adjacent_find(canonical, {}, &StandardPbrPassMaterial::id) != canonical.end())
                return Result<std::vector<StandardPbrPassMaterial>>::Failure(MakeError(StandardPbrPassPlanErrors::DuplicateMaterial));
            std::ranges::stable_sort(canonical, [](const StandardPbrPassMaterial &left, const StandardPbrPassMaterial &right) {
                return left.alphaMode == MaterialAlphaMode::Opaque && right.alphaMode == MaterialAlphaMode::Masked;
            });
            return Result<std::vector<StandardPbrPassMaterial>>::Success(std::move(canonical));
        }

        /** @brief Emits ordered logical batches over two contiguous alpha-class ranges. */
        void BuildBatches(StandardPbrPassPlan &plan, const StandardPbrPassPlanRequest &request) {
            using enum MaterialAlphaMode;
            using enum StandardPbrPassStage;

            const auto masked = std::ranges::find(plan.materials, Masked, &StandardPbrPassMaterial::alphaMode);
            const auto opaqueCount = static_cast<std::size_t>(masked - plan.materials.begin());
            const std::size_t maskedCount = plan.materials.size() - opaqueCount;
            const auto depthOutputs = StandardPbrPassOutputs{.depth = true, .motionVectors = request.motionVectors};
            const auto colorOutputs = StandardPbrPassOutputs{.sceneColor = true,
                                                             .depth = !request.depthPrepass,
                                                             .motionVectors = request.motionVectors && !request.depthPrepass};
            plan.batches.reserve(request.family == StandardPbrRasterFamily::Deferred ? 5U : 4U);
            if (request.depthPrepass) {
                AppendBatch(plan.batches, Depth, Opaque, 0, opaqueCount, depthOutputs);
                AppendBatch(plan.batches, Depth, Masked, opaqueCount, maskedCount, depthOutputs);
            }
            if (request.family == StandardPbrRasterFamily::Deferred) {
                const auto gbufferOutputs =
                    StandardPbrPassOutputs{.depth = !request.depthPrepass, .motionVectors = request.motionVectors && !request.depthPrepass};
                AppendBatch(plan.batches, DeferredGBuffer, Opaque, 0, opaqueCount, gbufferOutputs);
                AppendBatch(plan.batches, DeferredGBuffer, Masked, opaqueCount, maskedCount, gbufferOutputs);
                plan.batches.emplace_back(DeferredLighting, Opaque, 0, 0, StandardPbrPassOutputs{.sceneColor = true});
                return;
            }
            AppendBatch(plan.batches, ForwardColor, Opaque, 0, opaqueCount, colorOutputs);
            AppendBatch(plan.batches, ForwardColor, Masked, opaqueCount, maskedCount, colorOutputs);
        }
    }  // namespace

    /** @copydoc PrepareStandardPbrPassPlan */
    Result<StandardPbrPassPlan> PrepareStandardPbrPassPlan(const StandardPbrPassPlanRequest &request,
                                                           const std::span<const ResidentStandardPbrMaterial> materials) {
        if (auto valid = ValidateRequest(request, materials.size()); valid.HasError())
            return Result<StandardPbrPassPlan>::Failure(valid.ErrorValue());
        try {
            auto canonical = BuildCanonicalMaterials(materials);
            if (canonical.HasError())
                return Result<StandardPbrPassPlan>::Failure(canonical.ErrorValue());
            StandardPbrPassPlan plan{.family = request.family,
                                     .recipeGeneration = request.recipeGeneration,
                                     .requiredOutputs = {.sceneColor = true, .depth = true, .motionVectors = request.motionVectors},
                                     .materials = std::move(canonical).Value()};
            BuildBatches(plan, request);
            return Result<StandardPbrPassPlan>::Success(std::move(plan));
        } catch (const std::bad_alloc &) {
            return Result<StandardPbrPassPlan>::Failure(MakeError(StandardPbrPassPlanErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
