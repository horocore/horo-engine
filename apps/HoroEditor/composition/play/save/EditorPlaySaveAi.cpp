#include "EditorPlaySaveAi.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;
        using Population = std::shared_ptr<const AI::AiCanonicalState>;
        using Decoded = Result<Population>;

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

    /** @copydoc PlaySaveAiServices::BindCapture */
    Result<AI::AiSaveCaptureParticipant> PlaySaveAiServices::BindCapture() const {
        return AI::BindAiSaveCapture(AiParticipant(), AiRecord(), DefaultRuntimeSaveCopiedRecordBytes, runtime_, [scenes = &scenes_] {
            return scenes->ActiveScene();
        });
    }

    /** @copydoc PlaySaveAiServices::Stage */
    Result<void> PlaySaveAiServices::Stage(RuntimeSceneView scene, Population state, CancellationToken cancellation) {
        if (pending_.state || !state || !scene.IsCurrent())
            return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
        scene_ = scene;
        pending_ = {std::move(state), std::move(cancellation)};
        return Result<void>::Success();
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

    /** @copydoc DecodePlaySaveAi */
    Decoded DecodePlaySaveAi(const ValidatedSaveArchive &archive, const std::optional<CanonicalStateParticipantDescriptor> &descriptor) {
        const auto &participants = archive.Manifest().participants;
        const auto row = std::ranges::find(participants, AiParticipant(), &SaveManifestParticipant::participant);
        const auto bytes = archive.SelectChunk(AiRecord());
        if (bytes.HasError())
            return Decoded::Failure(bytes.ErrorValue());
        if (!descriptor)
            return row == participants.end() && !bytes.Value() ? Decoded::Success({})
                                                               : Decoded::Failure(MakeError(SaveErrors::CompositionUnsupported));
        if (row == participants.end() || !bytes.Value())
            return Decoded::Failure(MakeError(SaveErrors::CaptureIncomplete));
        if (row->schemaVersion != descriptor->schemaVersion || !row->required || row->chunks != descriptor->ownedRecords)
            return Decoded::Failure(MakeError(SaveErrors::CaptureRegistryStale));
        const auto record = *bytes.Value();
        auto decoded = AI::DecodeAiCanonicalStateRecord({reinterpret_cast<const std::uint8_t *>(record.data()), record.size()},
                                                        DefaultRuntimeSaveCopiedRecordBytes);
        if (decoded.HasError())
            return Decoded::Failure(decoded.ErrorValue());
        try {
            return Decoded::Success(std::make_shared<const AI::AiCanonicalState>(std::move(decoded).Value()));
        } catch (const std::bad_alloc &) {
            return Decoded::Failure(MakeError(SaveErrors::RestoreAllocationFailed));
        }
    }
}  // namespace Horo::Application::Internal
