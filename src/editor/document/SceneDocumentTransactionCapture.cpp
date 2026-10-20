#include "editor/document/SceneDocumentTransactionCapture.h"

#include "Horo/Scene/SceneSource.h"

namespace Horo::Editor {
    using namespace SceneDocumentDetail;

    namespace {
        /** @brief Preserve the canonical typed transaction rejection. */
        Error Rejected(const SceneTransactionFailure reason) {
            return MakeError(SceneTransactionError(reason));
        }
    }  // namespace

    /** @copydoc FingerprintSceneObjectContent */
    Result<Sha256Digest> FingerprintSceneObjectContent(const SceneObjectSnapshot &object, const std::size_t maximumBytes) {
        if (!maximumBytes || maximumBytes > MaximumSceneObjectFingerprintBytes ||
            EstimateSceneObjectOwnedMemoryBytes(object) > maximumBytes)
            return Result<Sha256Digest>::Failure(Rejected(SceneTransactionFailure::Capacity));
        auto authored = object;
        authored.generated.reset();
        const auto encoded = SceneSource::EncodeSceneSource({std::span{&authored, 1}, {}});
        if (encoded.size() > maximumBytes)
            return Result<Sha256Digest>::Failure(Rejected(SceneTransactionFailure::Capacity));
        if (auto parsed = SceneSource::DecodeSceneSource(encoded); parsed.HasError())
            return Result<Sha256Digest>::Failure(parsed.ErrorValue());
        return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span{encoded.data(), encoded.size()})));
    }

    namespace SceneTransactionCapture {
        /** @brief Compare exact semantic content and its independent generation ownership. */
        static Result<void> Matches(const SceneObjectSnapshot &object, const Sha256Digest &content,
                                    const std::optional<SceneSource::SceneGeneratedOwnership> &ownership) {
            if (object.generated != ownership)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
            const auto fingerprint = FingerprintSceneObjectContent(object, kMaximumHistoryBytes);
            if (fingerprint.HasError())
                return Result<void>::Failure(fingerprint.ErrorValue());
            if (fingerprint.Value() != content)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
            return Result<void>::Success();
        }

        /** @copydoc ChargeObjects */
        bool ChargeObjects(const std::span<const SceneObjectSnapshot> objects, const std::size_t limit, std::size_t &bytes) {
            for (const auto &object : objects) {
                const auto nested = EstimateSceneObjectOwnedMemoryBytes(object);
                if (nested > limit || sizeof(object) > limit - nested || bytes > limit - nested - sizeof(object))
                    return false;
                bytes += nested + sizeof(object);
            }
            return true;
        }

        /** @brief Prove a generated fragment's authored/retirement reservation before copying it. */
        static bool FitsReservation(const SceneObjectReplacement &change, const SceneObjectSnapshot *before) {
            if (!(change.desired && change.desired->generated) && !(before && before->generated))
                return true;
            const std::size_t afterBytes =
                change.desired ? sizeof(SceneObjectSnapshot) + EstimateSceneObjectOwnedMemoryBytes(*change.desired) : 0;
            const std::size_t beforeBytes = before ? sizeof(SceneObjectSnapshot) + EstimateSceneObjectOwnedMemoryBytes(*before) : 0;
            return afterBytes <= change.resources.residentBytes && beforeBytes <= change.resources.retirementBytes &&
                   afterBytes <= change.resources.preparationBytes && beforeBytes <= change.resources.preparationBytes - afterBytes;
        }

        /** @brief Derive committed indices from surviving authored order before applying detached changes. */
        static void ResolveReplacementIndices(SceneObjectsReplacedDelta &delta, const std::size_t objectCount) {
            const auto removed = static_cast<std::size_t>(std::ranges::count_if(delta.objects, [](const auto &change) {
                return change.before && !change.after;
            }));
            std::size_t appended = objectCount - removed;
            for (auto &change : delta.objects) {
                if (!change.after)
                    continue;
                if (!change.before) {
                    change.after->index = appended++;
                    continue;
                }
                const auto removedBefore = std::ranges::count_if(delta.objects, [&change](const auto &other) {
                    return other.before && !other.after && other.before->index < change.before->index;
                });
                change.after->index = change.before->index - static_cast<std::size_t>(removedBefore);
            }
        }

        /** @brief Capture one validated before/after pair; absence denotes an exact semantic no-op. */
        static Result<std::optional<SceneObjectReplacementDelta>> CaptureReplacement(const std::vector<SceneObjectSnapshot> &objects,
                                                                                     const SceneObjectReplacement &change) {
            using Return = Result<std::optional<SceneObjectReplacementDelta>>;
            const auto current = FindObject(objects, change.object);
            if (!FitsReservation(change, current == objects.end() ? nullptr : std::to_address(current)))
                return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
            SceneObjectReplacementDelta captured{change.object, {}, {}};
            if (current == objects.end()) {
                if (change.priorContent != Sha256Digest{} || change.expectedOwnership || !change.desired)
                    return Return::Failure(Rejected(SceneTransactionFailure::Stale));
            } else {
                if (IsEffectivelyLocked(objects, change.object))
                    return Return::Failure(LockedObjectError());
                if (const auto matches = Matches(*current, change.priorContent, change.expectedOwnership); matches.HasError())
                    return Return::Failure(matches.ErrorValue());
                captured.before = IndexedSceneObject{*current, static_cast<std::size_t>(current - objects.begin())};
            }
            if (change.desired) {
                if (change.desired->id != change.object)
                    return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
                // Existing objects keep their index; ResolveReplacementIndices assigns final positions after removals.
                captured.after = IndexedSceneObject{*change.desired, captured.before ? captured.before->index : objects.size()};
            }
            if (captured.before && captured.after && captured.before->object.generated == captured.after->object.generated) {
                const auto desired = FingerprintSceneObjectContent(captured.after->object, kMaximumHistoryBytes);
                if (desired.HasError())
                    return Return::Failure(desired.ErrorValue());
                if (desired.Value() == change.priorContent)
                    return Return::Success(std::nullopt);
            }
            return Return::Success(std::move(captured));
        }

        /** @copydoc CaptureReplacements */
        Result<SceneObjectsReplacedDelta> CaptureReplacements(const std::vector<SceneObjectSnapshot> &objects,
                                                              const std::span<const SceneObjectReplacement> changes) {
            SceneObjectsReplacedDelta delta;
            delta.objects.reserve(changes.size());
            std::vector<SceneObjectId> submitted;
            submitted.reserve(changes.size());
            for (const auto &change : changes) {
                if (!change.object.IsValid() || std::ranges::find(submitted, change.object) != submitted.end())
                    return Result<SceneObjectsReplacedDelta>::Failure(Rejected(SceneTransactionFailure::Invalid));
                submitted.push_back(change.object);
                auto captured = CaptureReplacement(objects, change);
                if (captured.HasError())
                    return Result<SceneObjectsReplacedDelta>::Failure(captured.ErrorValue());
                auto owned = std::move(captured).Value();
                if (owned)
                    delta.objects.push_back(std::move(*owned));
            }
            ResolveReplacementIndices(delta, objects.size());
            return Result<SceneObjectsReplacedDelta>::Success(std::move(delta));
        }

        /** @copydoc ValidateReplay */
        Result<void> ValidateReplay(const std::vector<SceneObjectSnapshot> &objects, const SceneCommandDelta &delta, const bool redo) {
            const auto *batch = std::get_if<SceneObjectsReplacedDelta>(&delta);
            if (!batch)
                return Result<void>::Success();
            for (const auto &change : batch->objects) {
                const auto &expected = redo ? change.before : change.after;
                const auto current = FindObject(objects, change.object);
                if (!expected) {
                    if (current != objects.end())
                        return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
                    continue;
                }
                if (current == objects.end())
                    return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
                const auto fingerprint = FingerprintSceneObjectContent(expected->object, kMaximumHistoryBytes);
                if (fingerprint.HasError())
                    return Result<void>::Failure(fingerprint.ErrorValue());
                if (auto matched = Matches(*current, fingerprint.Value(), expected->object.generated); matched.HasError())
                    return matched;
            }
            return Result<void>::Success();
        }

        /** @copydoc CapturePrefabSubtree */
        Result<SceneObjectsReplacedDelta> CapturePrefabSubtree(const std::vector<SceneObjectSnapshot> &objects,
                                                               const std::vector<ScenePrefabInstance> &instances, const SceneObjectId root,
                                                               const std::size_t maximumChanges) {
            using Return = Result<SceneObjectsReplacedDelta>;
            std::vector<SceneObjectReplacement> changes;
            std::vector<SceneObjectId> subtree{root};
            for (std::size_t cursor = 0; cursor < subtree.size(); ++cursor)
                for (const auto &object : objects)
                    if (object.parent == subtree[cursor]) {
                        if (subtree.size() >= maximumChanges)
                            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
                        subtree.push_back(object.id);
                    }
            for (const auto id : subtree) {
                const auto object = FindObject(objects, id);
                if (object->generated || IsEffectivelyLocked(objects, id))
                    return Return::Failure(Rejected(SceneTransactionFailure::Unauthorized));
                const auto digest = FingerprintSceneObjectContent(*object, MaximumSceneObjectFingerprintBytes);
                if (digest.HasError())
                    return Return::Failure(digest.ErrorValue());
                changes.push_back({id, digest.Value()});
            }
            for (const auto &placement : instances)
                if (placement.parent && std::ranges::find(subtree, *placement.parent) != subtree.end())
                    return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
            return CaptureReplacements(objects, changes);
        }
    }  // namespace SceneTransactionCapture
}  // namespace Horo::Editor
