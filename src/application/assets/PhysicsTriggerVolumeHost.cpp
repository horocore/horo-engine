#include "Horo/Application/PhysicsTriggerVolumeHost.h"

#include "Horo/Physics/PhysicsCollisionSchema.h"

#include <algorithm>

namespace Horo::Application {
    namespace {
        /** @brief Explicit registered typed producer; canonical material source and cooked payload share one format. */
        class PhysicsMaterialCooker final : public Assets::ICookerStrategy {
        public:
            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {.version = "horo.physics-material.v1", .settingsDigest = {}, .settingsSchemaVersion = 1};
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Assets::CookOutputSink>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                const auto material = Physics::DecodePhysicsMaterialAsset(std::as_bytes(source.bytes));
                if (material.HasError())
                    return Result<Assets::CookOutputSink>::Failure(material.ErrorValue());
                if (source.type.Value() != Physics::PhysicsMaterialAssetTypeName || material.Value().Id().Asset() != source.id)
                    return Result<Assets::CookOutputSink>::Failure(MakeError(Physics::PhysicsErrors::MaterialDescriptorInvalid));
                return Result<Assets::CookOutputSink>::Success({{source.bytes.begin(), source.bytes.end()}, {}, {}});
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                               const std::span<const std::uint8_t> payload) const override {
                const auto material = Physics::DecodePhysicsMaterialAsset(std::as_bytes(payload));
                if (material.HasError())
                    return Result<void>::Failure(material.ErrorValue());
                if (source.type.Value() != Physics::PhysicsMaterialAssetTypeName || material.Value().Id().Asset() != source.id ||
                    !std::ranges::equal(source.bytes, payload))
                    return Result<void>::Failure(MakeError(Physics::PhysicsErrors::MaterialDescriptorInvalid));
                return Result<void>::Success();
            }
        };
    }  // namespace

    /** @copydoc MakePhysicsMaterialCooker */
    std::shared_ptr<const Assets::ICookerStrategy> MakePhysicsMaterialCooker() {
        return std::make_shared<const PhysicsMaterialCooker>();
    }

    /** @copydoc CaptureProjectTriggerVolumeBinding */
    Result<SceneSource::TriggerVolumeConversionContext> CaptureProjectTriggerVolumeBinding(
        const Physics::PhysicsWorldSimulationBinding &simulation, const Physics::CollisionProfileId profile,
        const Physics::PhysicsMaterialAsset &material, const Assets::AssetCookArtifact &artifact) {
        if (!simulation.schema || simulation.generation == 0)
            return Result<SceneSource::TriggerVolumeConversionContext>::Failure(
                MakeError(Physics::PhysicsErrors::DescriptorInvalid,
                          "Capture the explicit installed Project collision schema before converting triggers."));
        if (const auto selected = simulation.schema->ResolveStaticSensorProfile(profile); selected.HasError())
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

    /** @copydoc CookScenesWithProjectTriggerVolumes */
    Result<Assets::AssetCookReport> CookScenesWithProjectTriggerVolumes(PrefabSceneCookHost &host, const PrefabSceneCookRequest &request,
                                                                        const Physics::PhysicsWorldSimulationBinding &simulation,
                                                                        const Physics::CollisionProfileId profile,
                                                                        const Physics::PhysicsMaterialAsset &material,
                                                                        const Assets::AssetCookArtifact &artifact,
                                                                        const CancellationToken &cancellation) {
        if (artifact.target != request.assets.target)
            return Result<Assets::AssetCookReport>::Failure(
                MakeError(PrefabSceneCookErrors::Invalid, "The captured Physics material artifact has a different cook target."));
        auto binding = CaptureProjectTriggerVolumeBinding(simulation, profile, material, artifact);
        if (binding.HasError())
            return Result<Assets::AssetCookReport>::Failure(binding.ErrorValue());
        auto bound = request;
        bound.triggerBinding = std::move(binding).Value();
        return host.Cook(bound, cancellation);
    }
}  // namespace Horo::Application
