#include "CollisionProjectSettingsSessionState.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <stdexcept>

namespace Horo::Editor {
    namespace {
        /** @brief Converts canonical project UTF-8 to a native path without the Windows narrow-codepage overload. */
        std::filesystem::path NativePath(const std::string_view text) {
            std::u8string bytes;
            bytes.reserve(text.size());
            for (const char byte : text)
                bytes.push_back(static_cast<char8_t>(byte));
            return std::filesystem::path{bytes};
        }

        /** @brief The declared built-in payload contracts cannot carry a Physics profile/channel reference. */
        bool NoCollisionReferences(const std::string_view type) {
            static constexpr std::array types{"core.mesh",          "core.texture", "core.material",
                                              "core.shader",        "core.font",    "core.physics_material",
                                              "core.physics_shape", "core.audio",   "core.audio_clip"};
            return std::ranges::find(types, type) != types.end();
        }

        /** @brief Keeps the complete impact list bounded; exceeding the bound is failure, never truncation. */
        void Append(std::vector<CollisionReferenceImpact> &impacts, CollisionReferenceImpact impact) {
            if (impacts.size() >= 65'536)
                throw std::length_error("Collision reference inventory capacity exceeded.");
            impacts.push_back(std::move(impact));
        }

        /** @brief Detects deletion or retirement of any persistent definition, independent from display names. */
        bool RetiresIdentity(const Physics::NormalizedCollisionSchema &old, const Physics::NormalizedCollisionSchema &next) {
            for (const auto &v : old.Profiles())
                if (std::ranges::none_of(next.Profiles(), [&](const auto &n) {
                    return n.id == v.id;
                }))
                    return true;
            for (const auto &v : old.Layers())
                if (std::ranges::none_of(next.Layers(), [&](const auto &n) {
                    return n.id == v.id && (!v.enabled || n.enabled);
                }))
                    return true;
            return std::ranges::any_of(old.QueryChannels(), [&](const auto &v) {
                return std::ranges::none_of(next.QueryChannels(), [&](const auto &n) {
                    return n.id == v.id && (!v.enabled || n.enabled);
                });
            });
        }

        /** @brief Inventories the actual durable Character bindings, including disabled configured components. */
        void CharacterImpacts(const SceneSource::SceneObjectSnapshot &object, const std::string &path,
                              const Physics::NormalizedCollisionSchema &candidate, std::vector<CollisionReferenceImpact> &impacts) {
            if (!object.components.characterController)
                return;
            const auto &character = *object.components.characterController;
            const auto &collision = character.collision;
            for (const auto profile : {collision.profile, collision.requiredProfile})
                if (profile)
                    Append(impacts, {path, object.id, profile, false, candidate.ResolveProfile(*profile).HasError(),
                                     CollisionImpactReason::ProfileReference});
            if (collision.requiredLayer) {
                const bool missing = std::ranges::none_of(candidate.Layers(), [&](const auto &layer) {
                    return layer.id == *collision.requiredLayer && (!character.enabled || layer.enabled);
                });
                Append(impacts, {path, object.id, {}, false, missing, CollisionImpactReason::LayerReference, collision.requiredLayer, {}});
            }
            if (collision.queryChannel) {
                const bool missing = std::ranges::none_of(candidate.QueryChannels(), [&](const auto &channel) {
                    return channel.id == *collision.queryChannel && (!character.enabled || channel.enabled);
                });
                Append(impacts,
                       {path, object.id, {}, false, missing, CollisionImpactReason::QueryChannelReference, {}, collision.queryChannel});
            }
        }

        /** @brief Lists exact authored collider references and explicitly unavailable extension/prefab owners. */
        void SceneImpacts(const SceneSource::SceneSourceView &scene, const std::string &path,
                          const Physics::NormalizedCollisionSchema &candidate, const bool retires,
                          const std::optional<SceneSource::TriggerVolumeConversionContext> &trigger, const std::uint64_t savedFingerprint,
                          std::vector<CollisionReferenceImpact> &impacts) {
            using enum CollisionImpactReason;
            for (const auto &object : scene.objects) {
                CharacterImpacts(object, path, candidate, impacts);
                for (const auto &collider : object.components.colliders) {
                    const auto profile = candidate.ResolveProfile(collider.collisionProfile);
                    Append(impacts, {path, object.id, collider.collisionProfile, false, profile.HasError(), ProfileReference});
                }
                if (!object.components.behaviors.empty() || !object.components.gameplayComponents.empty())
                    Append(impacts, {path, object.id, {}, true, retires, OpaqueOwner});
                if (object.components.triggerVolume) {
                    const bool exact = trigger && trigger->binding.collisionSchemaFingerprint == savedFingerprint &&
                                       trigger->collisionFilterGeneration != 0 && trigger->materialSourceDigest != Sha256Digest{};
                    if (!exact)
                        Append(impacts, {path, object.id, {}, true, retires, TriggerCaptureStale});
                    else
                        Append(impacts, {path, object.id, trigger->binding.profile, false,
                                         candidate.ResolveStaticSensorProfile(trigger->binding.profile).HasError(), ProfileReference});
                }
            }
            if (!scene.prefabInstances.empty())
                Append(impacts, {path, {}, {}, true, retires, OpaqueOwner});
        }
    }  // namespace

    /** @copydoc CollisionProjectSettingsSession::State::Review */
    Result<std::shared_ptr<CollisionSettingsReview::State>> CollisionProjectSettingsSession::State::Review(
        std::shared_ptr<const Application::ProjectCollisionSettings> candidate) const {
        using Return = Result<std::shared_ptr<CollisionSettingsReview::State>>;
        try {
            auto review = std::make_shared<CollisionSettingsReview::State>();
            review->opening = opening;
            review->revision = revision;
            review->activation = activation;
            review->inventory = inventory;
            review->candidate = std::move(candidate);
            const auto snapshot = authorities.registry->Snapshot();
            review->registryRevision = snapshot.Revision();
            review->registrySnapshot = snapshot;
            if (snapshot.Records().size() > 4096)
                throw std::length_error("Collision source inventory capacity exceeded.");
            const bool retires = RetiresIdentity(*saved.settings->Schema(), *review->candidate->Schema());
            if (inventory.status != Assets::AssetRegistryBuildStatus::Complete || inventory.revision.value == 0 ||
                inventory.revision != snapshot.Revision())
                Append(review->impacts, {".", {}, {}, true, retires, CollisionImpactReason::InventoryIncomplete});
            if (const bool semanticChange =
                    saved.settings->Schema()->SemanticFingerprint() != review->candidate->Schema()->SemanticFingerprint();
                (activation.state == CollisionActivationState::Unknown && semanticChange) ||
                (activation.state == CollisionActivationState::Active &&
                 activation.fingerprint != review->candidate->Schema()->SemanticFingerprint()) ||
                (activation.playRunning &&
                 saved.settings->Schema()->SemanticFingerprint() != review->candidate->Schema()->SemanticFingerprint()))
                Append(review->impacts, {".horo/collision.json", {}, {}, false, true, CollisionImpactReason::ActiveGeneration});
            if (activeScene) {
                auto scene = activeScene->Snapshot();
                if (scene.HasError())
                    return Return::Failure(scene.ErrorValue());
                if (!sceneOpening || scene.Value().session != *sceneOpening)
                    return Return::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale)));
                review->activeScene = std::move(scene).Value();
            }
            if (const auto captured = CaptureSources(snapshot, retires, *review); captured.HasError())
                return Return::Failure(captured.ErrorValue());
            review->allowed = std::ranges::none_of(review->impacts, &CollisionReferenceImpact::blocksPublication);
            if (const auto fresh = Fresh(*review); fresh.HasError())
                return Return::Failure(fresh.ErrorValue());
            return Return::Success(std::move(review));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
        } catch (const std::length_error &) {
            return Return::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
        }
    }

    /** @copydoc CollisionProjectSettingsSession::State::CaptureRecord */
    Result<void> CollisionProjectSettingsSession::State::CaptureRecord(const Assets::AssetRecord &record, const bool retires,
                                                                       CollisionSettingsReview::State &review) const {
        const auto metadataPath = root / NativePath(record.metadataPath.String());
        auto metadata = Assets::CaptureAssetSource(root, metadataPath, 1024 * 1024);
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        review.sources.emplace_back(metadataPath, metadata.Value().identity);
        if (!metadata.Value().identity.exists)
            Append(review.impacts, {record.metadataPath.String(), {}, {}, true, retires, CollisionImpactReason::InventoryIncomplete});
        if (NoCollisionReferences(record.type.Value()))
            return Result<void>::Success();
        if (record.type.Value() != "core.scene") {
            Append(review.impacts, {record.sourcePath.String(), {}, {}, true, retires, CollisionImpactReason::OpaqueOwner});
            return Result<void>::Success();
        }
        const auto path = root / NativePath(record.sourcePath.String());
        auto source = Assets::CaptureAssetSource(root, path, 16 * 1024 * 1024);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        if (!source.Value().identity.exists)
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale)));
        review.sources.emplace_back(path, source.Value().identity);
        const auto &bytes = source.Value().bytes;
        std::string text(bytes.size(), '\0');
        std::ranges::transform(bytes, text.begin(), [](const std::byte value) {
            return std::to_integer<char>(value);
        });
        auto decoded = SceneSource::DecodeSceneSource(text);
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        for (const auto componentOwner : decoded.Value().unsupportedComponentOwners)
            Append(review.impacts, {record.sourcePath.String(), componentOwner, {}, true, retires, CollisionImpactReason::OpaqueOwner});
        if (review.activeScene && path == activeScenePath) {
            SceneImpacts({review.activeScene->objects, review.activeScene->prefabInstances}, record.sourcePath.String(),
                         *review.candidate->Schema(), retires, triggerBinding, saved.settings->Schema()->SemanticFingerprint(),
                         review.impacts);
            return Result<void>::Success();
        }
        SceneImpacts({decoded.Value().objects, decoded.Value().prefabInstances}, record.sourcePath.String(), *review.candidate->Schema(),
                     retires, triggerBinding, saved.settings->Schema()->SemanticFingerprint(), review.impacts);
        return Result<void>::Success();
    }

    /** @copydoc CollisionProjectSettingsSession::State::CaptureSources */
    Result<void> CollisionProjectSettingsSession::State::CaptureSources(const Assets::AssetRegistrySnapshot &snapshot, const bool retires,
                                                                        CollisionSettingsReview::State &review) const {
        const auto indexPath = root / std::filesystem::path{ProjectLayout::AssetIndexPath};
        auto index = Assets::CaptureAssetSource(root, indexPath, 16 * 1024 * 1024);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        review.sources.emplace_back(indexPath, index.Value().identity);
        if (!index.Value().identity.exists)
            Append(review.impacts,
                   {std::string{ProjectLayout::AssetIndexPath}, {}, {}, true, retires, CollisionImpactReason::InventoryIncomplete});
        bool activeKnown = !review.activeScene;
        for (const auto &record : snapshot.Records()) {
            if (const auto captured = CaptureRecord(record, retires, review); captured.HasError())
                return captured;
            activeKnown |= root / NativePath(record.sourcePath.String()) == activeScenePath && record.type.Value() == "core.scene";
        }
        if (!activeKnown)
            Append(review.impacts, {"active Scene", {}, {}, true, retires, CollisionImpactReason::InventoryIncomplete});
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
