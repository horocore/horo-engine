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
            batches.push_back({stage, alphaMode, firstMaterial, materialCount, outputs});
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
                canonical.push_back({material.id, material.sourceRevision, material.generation, material.pipeline, material.alphaMode});
            }
            std::ranges::sort(canonical, [](const StandardPbrPassMaterial &left, const StandardPbrPassMaterial &right) {
                if (left.alphaMode != right.alphaMode)
                    return left.alphaMode == MaterialAlphaMode::Opaque;
                return left.id < right.id;
            });
            if (std::ranges::adjacent_find(canonical, {}, &StandardPbrPassMaterial::id) != canonical.end())
                return Result<std::vector<StandardPbrPassMaterial>>::Failure(MakeError(StandardPbrPassPlanErrors::DuplicateMaterial));
            return Result<std::vector<StandardPbrPassMaterial>>::Success(std::move(canonical));
        }

        /** @brief Emits ordered logical batches over two contiguous alpha-class ranges. */
        void BuildBatches(StandardPbrPassPlan &plan, const StandardPbrPassPlanRequest &request) {
            const auto masked = std::ranges::find(plan.materials, MaterialAlphaMode::Masked, &StandardPbrPassMaterial::alphaMode);
            const std::size_t opaqueCount = static_cast<std::size_t>(masked - plan.materials.begin());
            const std::size_t maskedCount = plan.materials.size() - opaqueCount;
            const StandardPbrPassOutputs depthOutputs{.depth = true, .motionVectors = request.motionVectors};
            const StandardPbrPassOutputs colorOutputs{.sceneColor = true,
                                                      .depth = !request.depthPrepass,
                                                      .motionVectors = request.motionVectors && !request.depthPrepass};
            plan.batches.reserve(request.family == StandardPbrRasterFamily::Deferred ? 5U : 4U);
            if (request.depthPrepass) {
                AppendBatch(plan.batches, StandardPbrPassStage::Depth, MaterialAlphaMode::Opaque, 0, opaqueCount, depthOutputs);
                AppendBatch(plan.batches, StandardPbrPassStage::Depth, MaterialAlphaMode::Masked, opaqueCount, maskedCount, depthOutputs);
            }
            if (request.family == StandardPbrRasterFamily::Deferred) {
                const StandardPbrPassOutputs gbufferOutputs{.depth = !request.depthPrepass,
                                                            .motionVectors = request.motionVectors && !request.depthPrepass};
                AppendBatch(plan.batches, StandardPbrPassStage::DeferredGBuffer, MaterialAlphaMode::Opaque, 0, opaqueCount, gbufferOutputs);
                AppendBatch(plan.batches, StandardPbrPassStage::DeferredGBuffer, MaterialAlphaMode::Masked, opaqueCount, maskedCount,
                            gbufferOutputs);
                plan.batches.push_back({StandardPbrPassStage::DeferredLighting, MaterialAlphaMode::Opaque, 0, 0, {.sceneColor = true}});
                return;
            }
            AppendBatch(plan.batches, StandardPbrPassStage::ForwardColor, MaterialAlphaMode::Opaque, 0, opaqueCount, colorOutputs);
            AppendBatch(plan.batches, StandardPbrPassStage::ForwardColor, MaterialAlphaMode::Masked, opaqueCount, maskedCount,
                        colorOutputs);
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
