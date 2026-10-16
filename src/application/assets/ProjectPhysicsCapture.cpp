#include "Horo/Application/ProjectPhysicsCapture.h"

#include "Horo/Physics/PhysicsCollisionSchema.h"

namespace Horo::Application {
    /** @copydoc CaptureProjectTriggerVolumeBinding */
    Result<SceneSource::TriggerVolumeConversionContext> CaptureProjectTriggerVolumeBinding(
        const Physics::PhysicsWorldSimulationBinding &simulation, const Physics::CollisionProfileId profile,
        const Physics::PhysicsMaterialAsset &material, const Assets::AssetCookArtifact &artifact) {
        if (!simulation.schema || simulation.generation == 0)
            return Result<SceneSource::TriggerVolumeConversionContext>::Failure(
                MakeError(Physics::PhysicsErrors::DescriptorInvalid,
                          "Capture the explicit installed Project collision schema before converting triggers."));
        const auto selected = simulation.schema->ResolveStaticSensorProfile(profile);
        if (selected.HasError())
            return Result<SceneSource::TriggerVolumeConversionContext>::Failure(selected.ErrorValue());
        if (!material.IsValid() || artifact.id != material.Id().Asset() || artifact.type.Value() != Physics::PhysicsMaterialAssetTypeName ||
            artifact.payload.empty() || artifact.sourceDigest == Sha256Digest{} ||
            ComputeSha256(std::as_bytes(std::span{artifact.payload})) != artifact.payloadDigest)
            return Result<SceneSource::TriggerVolumeConversionContext>::Failure(
                MakeError(Physics::PhysicsErrors::MaterialDescriptorInvalid,
                          "Capture the actual cooked project material with matching identity and verified payload digest."));
        const auto decoded = Physics::DecodePhysicsMaterialAsset(std::as_bytes(std::span{artifact.payload}));
        if (decoded.HasError())
            return Result<SceneSource::TriggerVolumeConversionContext>::Failure(decoded.ErrorValue());
        if (decoded.Value().Descriptor() != material.Descriptor())
            return Result<SceneSource::TriggerVolumeConversionContext>::Failure(
                MakeError(Physics::PhysicsErrors::MaterialDescriptorInvalid,
                          "The actual material artifact does not encode the supplied immutable project material snapshot."));
        return Result<SceneSource::TriggerVolumeConversionContext>::Success({{simulation.schema->SemanticFingerprint(),
                                                                              profile,
                                                                              {artifact.id, artifact.type},
                                                                              material.SourceRevision(),
                                                                              artifact.payloadDigest},
                                                                             simulation.generation,
                                                                             artifact.sourceDigest});
    }

}  // namespace Horo::Application
