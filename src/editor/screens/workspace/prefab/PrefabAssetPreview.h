#ifndef HORO_EDITOR_PREFAB_ASSET_PREVIEW_H
#define HORO_EDITOR_PREFAB_ASSET_PREVIEW_H
/** @file PrefabAssetPreview.h
 * @brief Immutable canonical prefab geometry rendered by the existing bounded preview host.
 */
#include "Horo/Assets/AssetPreview.h"
#include "Horo/Prefab/PrefabValueSnapshot.h"
#include "editor/screens/workspace/prefab/PrefabHierarchySources.h"

namespace Horo::Editor {
    /** @brief Immutable value authorities published explicitly by an owner; asset cards ordinarily have no scene-instance delta. */
    struct PrefabAssetPreviewValues final {
        std::optional<Prefab::PrefabValueSchemaSnapshot> schemas;
        std::optional<Prefab::PrefabValueOverrideSet> delta;
        std::uint64_t publication{}; /**< Owner publication fence, separate from successful image/stage counters. */
    };

    /** @brief Worker-safe capture of owned immutable values; must not read a mutable document or borrow its lifetime.
     * @details Implementations may return a previously captured value or load a synchronized immutable owner publication.
     * Owner completion fencing remains required after the worker's final check; this is not an OS-atomic display snapshot.
     */
    using PrefabAssetPreviewValueCapture = std::function<Result<PrefabAssetPreviewValues>(const CancellationToken &)>;
    /** @brief Rasterizes canonical expanded primitive geometry with a fixed isometric camera and light.
     * @param candidate Fully resolved immutable source hierarchy.
     * @param width Output width, between 16 and 256 pixels.
     * @param height Output height, between 16 and 256 pixels.
     * @param cancellation Cooperative owner cancellation, checked between objects, triangles and scanlines.
     * @return Complete tightly packed RGBA image or typed admission, codec, geometry, capacity or cancellation error.
     * @details Load/background work; at most 20000 triangles and 2000000 covered pixel samples are admitted.
     * No document, renderer, native handle or runtime Scene is borrowed or activated.
     */
    [[nodiscard]] Result<Assets::AssetPreviewImage> RenderPrefabAssetPreview(const Prefab::EffectivePrefabCandidate &candidate,
                                                                             std::uint32_t width, std::uint32_t height,
                                                                             const CancellationToken &cancellation = {});

    /** @brief Creates a source-owning strategy for the existing cancellable thumbnail scheduler.
     * @param registry Pinned asset publication used to classify and resolve every source.
     * @param asset Exact registered core.prefab source identity.
     * @param capture Explicit host loader, whose borrowed project authority must outlive preview service shutdown.
     * @param projectLimits Explicit captured project policy. @param values Optional worker-safe immutable schema/delta publication capture.
     * @return Shared provider retaining only owned identity/publication data and the explicit host capability.
     * @details The controller cancels and joins the preview service before releasing document services. The host
     * capture owns its project lease during source capture and final closure revalidation; CPU rendering releases it.
     */
    [[nodiscard]] std::shared_ptr<const Assets::IAssetPreviewProvider> CreatePrefabAssetPreviewProvider(
        Assets::AssetRegistrySnapshot registry, Assets::AssetId asset, PrefabHierarchySourceCapture capture,
        const Prefab::PrefabLimitProfile *projectLimits = nullptr, PrefabAssetPreviewValueCapture values = {});
}  // namespace Horo::Editor

#endif
