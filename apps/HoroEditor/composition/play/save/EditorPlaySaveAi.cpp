#include "EditorPlaySaveAi.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;
        using AiRecordRead = Result<std::optional<std::vector<std::byte>>>;

        /** @brief Stable record identity for the Play AI canonical population. */
        SaveRecordId AiRecord() {
            return SaveRecordId::Parse("5037e745-aa41-494b-87bc-8cfbad192aeb").Value();
        }

        /** @brief Identifies the capture owner independently of product instances. */
        SaveParticipantId AiParticipant() {
            return SaveParticipantId::Parse("horo.editor.play.ai.v1").Value();
        }
    }  // namespace

    /** @copydoc PlaySaveAiServices::PlaySaveAiServices */
    PlaySaveAiServices::PlaySaveAiServices(RuntimeSceneService &scenes, std::shared_ptr<AI::AiSceneRuntime> runtime) noexcept
        : scenes_(scenes), runtime_(std::move(runtime)) {}

    /** @copydoc PlaySaveAiServices::Descriptor */
    CanonicalStateParticipantDescriptor PlaySaveAiServices::Descriptor() const {
        return AI::AiSaveCaptureDescriptor(AiParticipant(), AiRecord(), DefaultRuntimeSaveCopiedRecordBytes, true);
    }

    /** @copydoc PlaySaveAiServices::BindCapture */
    Result<AI::AiSaveCaptureParticipant> PlaySaveAiServices::BindCapture(AI::AiSaveIdentitySource identities) const {
        return AI::BindAiSaveCapture(AiParticipant(), AiRecord(), DefaultRuntimeSaveCopiedRecordBytes, runtime_, [scenes = &scenes_] {
            return scenes->ActiveScene();
        }, std::move(identities));
    }

    /** @copydoc PlaySaveAiServices::Stage */
    Result<void> PlaySaveAiServices::Stage(RuntimeSceneView scene, std::span<const std::byte> record,
                                           const PersistentEntityIdentityMap &identities, CancellationToken cancellation) {
        if (pending_.state || !scene.IsCurrent())
            return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
        auto state = AI::DecodeAiSavePopulation({reinterpret_cast<const std::uint8_t *>(record.data()), record.size()},
                                                DefaultRuntimeSaveCopiedRecordBytes, scene, identities);
        if (state.HasError())
            return Result<void>::Failure(std::move(state).ErrorValue());
        try {
            pending_ = {std::make_shared<const AI::AiCanonicalState>(std::move(state).Value()), std::move(cancellation)};
            scene_ = scene;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::RestoreAllocationFailed));
        }
    }

    /** @copydoc PlaySaveAiServices::Take */
    Result<AI::AiSceneRestoredPopulation> PlaySaveAiServices::Take(const RuntimeSceneView &scene) {
        using Outcome = Result<AI::AiSceneRestoredPopulation>;
        if (!pending_.state)
            return Outcome::Success({});
        if (!scene_.IsCurrent() || !scene_.SameIncarnation(scene))
            return Outcome::Failure(MakeError(SaveErrors::RestoreActivationStale));
        return Outcome::Success(std::exchange(pending_, {}));
    }

    /** @copydoc PlaySaveAiServices::ValidateConsumption */
    Result<void> PlaySaveAiServices::ValidateConsumption(const RuntimeSceneView &scene) const {
        if (pending_.state || !scene_.IsCurrent() || !scene_.SameIncarnation(scene))
            return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
        return Result<void>::Success();
    }

    /** @copydoc PlaySaveAiServices::Cancel */
    void PlaySaveAiServices::Cancel(const RuntimeSceneView &scene) noexcept {
        if (scene_.SameIncarnation(scene)) {
            pending_ = {};
            scene_ = {};
        }
    }

    /** @copydoc ReadPlaySaveAi */
    AiRecordRead ReadPlaySaveAi(const ValidatedSaveArchive &archive, const std::optional<CanonicalStateParticipantDescriptor> &descriptor) {
        const auto &participants = archive.Manifest().participants;
        const auto row = std::ranges::find(participants, AiParticipant(), &SaveManifestParticipant::participant);
        auto bytes = archive.SelectChunk(AiRecord());
        if (bytes.HasError())
            return bytes;
        if (!descriptor)
            return row == participants.end() && !bytes.Value() ? AiRecordRead::Success({})
                                                               : AiRecordRead::Failure(MakeError(SaveErrors::CompositionUnsupported));
        if (row == participants.end() || !bytes.Value())
            return AiRecordRead::Failure(MakeError(SaveErrors::CaptureIncomplete));
        if (row->schemaVersion > descriptor->schemaVersion || !row->required || row->chunks != descriptor->ownedRecords)
            return AiRecordRead::Failure(MakeError(SaveErrors::CaptureRegistryStale));
        const auto &record = *bytes.Value();
        const auto validated = AI::ValidateAiCanonicalStateRecord({reinterpret_cast<const std::uint8_t *>(record.data()), record.size()},
                                                                  DefaultRuntimeSaveCopiedRecordBytes, row->schemaVersion.Value());
        return validated.HasError() ? AiRecordRead::Failure(validated.ErrorValue()) : std::move(bytes);
    }
}  // namespace Horo::Application::Internal
