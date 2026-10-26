#pragma once

/**
 * @file EditorAssetMeshCache.h
 * @brief Bounded owner-thread cache for imported editor mesh payloads used by viewport extraction.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderScene.h"

#include <filesystem>
#include <memory>
#include <unordered_map>

namespace Horo::Editor {
    struct EditorAssetMeshView {
        Render::RenderMeshSourceHandle handle;
        const Render::MeshData *mesh{};
    };

    /** @brief Immutable ownership pin for an existing canonical imported mesh generation. */
    struct EditorAssetMeshLease {
        Render::RenderMeshSourceHandle handle;
        std::shared_ptr<const Render::MeshData> mesh;
    };

    class EditorAssetMeshCache final {
    public:
        /** @brief Loads, validates, and retains one imported core.mesh editor payload. */
        [[nodiscard]] Result<EditorAssetMeshView> Load(Assets::AssetId asset, const std::filesystem::path &absolutePath);
        /** @brief Returns a previously loaded immutable mesh view. */
        [[nodiscard]] std::optional<EditorAssetMeshView> Find(Assets::AssetId asset) const noexcept;
        /** @brief Pins a previously loaded mesh without filesystem access or a second decode/cache.
         * @param asset Exact canonical asset. @return Same immutable generation or a missing/retired source error.
         */
        [[nodiscard]] Result<EditorAssetMeshLease> Acquire(Assets::AssetId asset) const;

        /** @brief Returns this cache-clear incarnation, never a payload/asset publication authority. */
        [[nodiscard]] std::uint32_t Generation() const noexcept {
            return generation_;
        }

        /** @brief Invalidates borrowed views and advances source generation; existing immutable leases remain valid. */
        void Clear() noexcept;

    private:
        std::unordered_map<Assets::AssetId, std::shared_ptr<const Render::MeshData>, Assets::AssetIdHash> meshes_;
        std::uint32_t generation_{1}; /**< Cache-clear incarnation; exhaustion permanently denies new handles instead of wrapping. */
    };
}  // namespace Horo::Editor
