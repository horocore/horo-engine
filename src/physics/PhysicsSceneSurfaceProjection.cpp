#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "PhysicsSceneActivationInternal.h"

#include <algorithm>

namespace Horo::Physics::Detail {
    /** @copydoc DecodeScenePhysicalMaterial */
    Result<PhysicsQueryMaterial> DecodeScenePhysicalMaterial(const Runtime::PhysicsColliderMaterialBinding &binding,
                                                             const Assets::AssetTypeId &type, std::span<const std::uint8_t> payload) {
        using Return = Result<PhysicsQueryMaterial>;
        if (type.Value() != PhysicsMaterialAssetTypeName)
            return Return::Failure(MakeError(PhysicsErrors::MaterialDescriptorInvalid));
        auto decoded = DecodePhysicsMaterialAsset(std::as_bytes(payload));
        if (decoded.HasError())
            return Return::Failure(std::move(decoded).ErrorValue());
        if (decoded.Value().Id().Asset() != binding.material)
            return Return::Failure(MakeError(PhysicsErrors::MaterialDescriptorInvalid));
        return Return::Success({binding.material, decoded.Value().SourceRevision(), binding.slot});
    }

    /** @copydoc BindColliderSurface */
    Result<std::optional<PhysicsSurfaceEvidence>> BindColliderSurface(const Runtime::ColliderComponent &collider,
                                                                      const PhysicsSurfaceWorldBinding &surfaces) {
        using Return = Result<std::optional<PhysicsSurfaceEvidence>>;
        if (collider.surfaceBindings.size() > Runtime::MaximumPhysicsColliderSurfaceBindings)
            return Return::Failure(MakeError(SurfaceErrors::Limit));
        if (collider.surfaceBindings.empty())
            return Return::Success(std::nullopt);
        if (auto valid = ValidatePhysicsSurfaceWorldBinding(surfaces); valid.HasError())
            return Return::Failure(std::move(valid).ErrorValue());
        if (!surfaces.catalog)
            return Return::Failure(MakeError(SurfaceErrors::Invalid));
        // Analytic native shapes have one used physical slot. Cooked mesh realization remains explicitly unsupported.
        if (collider.materials.size() != 1 || collider.surfaceBindings.size() != 1)
            return Return::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        const auto &binding = collider.surfaceBindings.front();
        if (binding.slot != collider.materials.front().slot || !surfaces.catalog->Contains(binding.surface))
            return Return::Failure(MakeError(SurfaceErrors::Invalid));
        auto evidence = MakePhysicsSurfaceEvidence(surfaces, binding.surface);
        evidence.collider = collider.collider.value;
        return Return::Success(evidence);
    }
}  // namespace Horo::Physics::Detail
