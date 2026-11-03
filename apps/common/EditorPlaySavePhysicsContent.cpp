#include "EditorPlaySavePhysicsContent.h"

#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateHashInternal.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace Horo::Application::Internal {
    /** @copydoc PlaySavePhysicsContent::AddMaterial */
    Result<void> PlaySavePhysicsContent::AddMaterial(Runtime::RuntimeSceneView scene, Assets::AssetId id, std::size_t maximum) {
        if (std::ranges::find(materials_, id, &Material::id) != materials_.end())
            return Result<void>::Success();
        if (materials_.size() >= maximum)
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        const auto asset = scene.FindAsset(id);
        if (!asset || !asset->type || asset->type->Value() != Physics::PhysicsMaterialAssetTypeName)
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::MaterialDescriptorInvalid));
        auto decoded = Physics::DecodePhysicsMaterialAsset(std::as_bytes(asset->bytes));
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        if (decoded.Value().Id().Asset() != id)
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::MaterialDescriptorInvalid));
        Material owned{.id = id, .revision = decoded.Value().SourceRevision()};
        std::ranges::copy(std::as_bytes(asset->bytes), owned.bytes.begin());
        materials_.push_back(owned);
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsContent::CollectColliders */
    Result<void> PlaySavePhysicsContent::CollectColliders(Runtime::RuntimeSceneView scene, const Runtime::RuntimeComponentSet &components,
                                                          std::size_t maximumMaterials) {
        for (const auto &collider : components.colliders) {
            if (!collider.enabled)
                continue;
            for (const auto &material : collider.materials)
                if (auto added = AddMaterial(scene, material.material, maximumMaterials); added.HasError())
                    return added;
        }
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsContent::Collect */
    Result<void> PlaySavePhysicsContent::Collect(Runtime::RuntimeSceneView scene, PlaySavePhysicsContentLimits limits) {
        if (!scene.IsCurrent() || scene.SlotCount() > limits.maximumEntitySlots)
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        definition_ = scene.DefinitionId();
        revision_ = scene.DefinitionRevision();
        for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
            const auto entity = scene.EntityAt(slot);
            if (!entity || !entity->components)
                continue;
            if (auto collected = CollectColliders(scene, *entity->components, limits.maximumMaterials); collected.HasError())
                return collected;
        }
        std::ranges::sort(materials_, {}, &Material::id);
        return RetainedPayloadBytes() <= limits.maximumRetainedBytes
                   ? Result<void>::Success()
                   : Result<void>::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
    }

    /** @copydoc PlaySavePhysicsContent::Capture */
    Result<std::shared_ptr<const PlaySavePhysicsContent>> PlaySavePhysicsContent::Capture(Runtime::RuntimeSceneView scene,
                                                                                          PlaySavePhysicsContentLimits limits) {
        using Outcome = Result<std::shared_ptr<const PlaySavePhysicsContent>>;
        if (limits.maximumRetainedBytes < sizeof(PlaySavePhysicsContent))
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        if (limits.maximumMaterials > (limits.maximumRetainedBytes - sizeof(PlaySavePhysicsContent)) / sizeof(Material))
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        try {
            auto result = std::shared_ptr<PlaySavePhysicsContent>{new PlaySavePhysicsContent};
            result->materials_.reserve(limits.maximumMaterials);
            if (auto collected = result->Collect(scene, limits); collected.HasError())
                return Outcome::Failure(collected.ErrorValue());
            return Outcome::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        } catch (const std::length_error &) {
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PlaySavePhysicsContent::Fingerprint */
    Sha256Digest PlaySavePhysicsContent::Fingerprint() const noexcept {
        Physics::StateDetail::StateHash hash{"Horo.Physics.State.SceneContent.v1"};
        hash.Word(definition_.value);
        hash.Word(revision_.value);
        hash.Word(materials_.size());
        for (const auto &material : materials_) {
            hash.Id(material.id);
            hash.Word(material.revision);
            hash.Digest(ComputeSha256(material.bytes));
        }
        return hash.Finish();
    }

    /** @copydoc PlaySavePhysicsContent::ValidateMaterials */
    Result<void> PlaySavePhysicsContent::ValidateMaterials(const Physics::PhysicsStateConstructionManifest &manifest) const {
        for (const auto &saved : manifest.materials) {
            const auto actual = std::ranges::lower_bound(materials_, saved.asset, {}, &Material::id);
            if (actual == materials_.end() || actual->id != saved.asset || actual->revision != saved.assetGeneration)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::ProfileUnsupported));
        }
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsContent::RetainedPayloadBytes */
    std::size_t PlaySavePhysicsContent::RetainedPayloadBytes() const noexcept {
        if (materials_.capacity() > (std::numeric_limits<std::size_t>::max() - sizeof(*this)) / sizeof(Material))
            return std::numeric_limits<std::size_t>::max();
        return sizeof(*this) + materials_.capacity() * sizeof(Material);
    }
}  // namespace Horo::Application::Internal
