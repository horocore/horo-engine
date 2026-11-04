#pragma once
#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "Horo/Physics/PhysicsStateConstructionManifest.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <vector>

namespace Horo::Application::Internal {
    struct PlaySavePhysicsContentLimits final {
        std::size_t maximumEntitySlots{}, maximumMaterials{}, maximumRetainedBytes{};
    };

    /** @brief Actual Scene-ready canonical material bytes copied cold into immutable content ownership.
     * No paths, current load epochs or authoring labels authenticate data. Saved material IDs/source revisions must match these bytes.
     * The actual Scene provider/type admission precedes this capture; no source I/O or new provider lookup is performed here.
     */
    class PlaySavePhysicsContent final {
    public:
        [[nodiscard]] static Result<std::shared_ptr<const PlaySavePhysicsContent>> Capture(Runtime::RuntimeSceneView,
                                                                                           PlaySavePhysicsContentLimits);
        [[nodiscard]] Sha256Digest Fingerprint() const noexcept;
        [[nodiscard]] Result<void> ValidateMaterials(const Physics::PhysicsStateConstructionManifest &) const;
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;

    private:
        PlaySavePhysicsContent() = default;

        struct Material final {
            Assets::AssetId id;
            std::uint64_t revision{};
            Physics::PhysicsMaterialPayload bytes;
        };

        [[nodiscard]] Result<void> Collect(Runtime::RuntimeSceneView, PlaySavePhysicsContentLimits);
        [[nodiscard]] Result<void> CollectColliders(Runtime::RuntimeSceneView, const Runtime::RuntimeComponentSet &,
                                                    std::size_t maximumMaterials);
        [[nodiscard]] Result<void> AddMaterial(Runtime::RuntimeSceneView, Assets::AssetId, std::size_t maximum);
        Runtime::SceneDefinitionId definition_;
        Runtime::SceneDefinitionRevision revision_;
        std::vector<Material> materials_;
    };
}  // namespace Horo::Application::Internal
