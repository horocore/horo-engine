#include "Horo/AI/AISaveCapture.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>
#include <thread>

namespace Horo::AI {
    namespace {
        using namespace Runtime;
        using CaptureResult = Result<CanonicalCaptureDisposition>;
        constexpr std::uint32_t SaveSchemaVersion = 1;

        /** @brief Copies the base-agent record into the current aggregate capture. */
        class CaptureAdapter final : public ICanonicalStateAdapter {
        public:
            CaptureAdapter(SaveParticipantId participant, SaveRecordId record, std::uint64_t maximumBytes,
                           std::weak_ptr<const AiSceneRuntime> runtime, AiSaveSceneSource scene)
                : participant_(std::move(participant)), record_(std::move(record)), maximumBytes_(maximumBytes),
                  runtime_(std::move(runtime)), scene_(std::move(scene)), owner_(std::this_thread::get_id()) {}

            CaptureResult Capture(const CanonicalCaptureContext &context, ICanonicalCaptureSink &sink) const override {
                if (owner_ != std::this_thread::get_id() || context.participant != participant_ ||
                    context.schemaVersion.Value() != SaveSchemaVersion || context.scope != SaveParticipantScope::RuntimeScene)
                    return CaptureResult::Failure(MakeError(SaveErrors::CaptureRegistryStale));
                const auto &remaining = context.admission;
                const auto budget = std::min({maximumBytes_, remaining.operationPayloadBytes, remaining.participantPayloadBytes,
                                              remaining.maximumCopiedRecordBytes});
                if (budget == 0 || !remaining.operationRecords || !remaining.participantRecords || !remaining.operationSegments)
                    return CaptureResult::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
                const auto owner = runtime_.lock();
                if (!owner)
                    return CaptureResult::Failure(MakeError(AIErrors::RuntimeUnavailable));
                const auto scene = scene_();
                if (!scene || !scene->IsCurrent() || scene->RuntimeId().value != context.provenance.sceneIncarnation ||
                    scene->StructuralRevision() != context.provenance.sceneRevision)
                    return CaptureResult::Failure(
                        MakeError(SaveErrors::CaptureContextInvalid, "AI capture requires the exact current Scene."));
                const auto state = owner->CaptureCanonicalState(*scene, static_cast<std::size_t>(budget));
                if (state.HasError())
                    return CaptureResult::Failure(state.ErrorValue());
                const auto encoded = EncodeAiCanonicalStateRecord(state.Value(), static_cast<std::size_t>(budget));
                if (encoded.HasError())
                    return CaptureResult::Failure(encoded.ErrorValue());
                if (const auto written = sink.WriteCopied(record_, std::as_bytes(std::span{encoded.Value()})); written.HasError())
                    return CaptureResult::Failure(written.ErrorValue());
                return CaptureResult::Success(CanonicalCaptureDisposition::Captured);
            }

        private:
            SaveParticipantId participant_;
            SaveRecordId record_;
            std::uint64_t maximumBytes_;
            std::weak_ptr<const AiSceneRuntime> runtime_;
            AiSaveSceneSource scene_;
            std::thread::id owner_;
        };
    }  // namespace

    /** @copydoc BindAiSaveCapture */
    Result<AiSaveCaptureParticipant> BindAiSaveCapture(Runtime::SaveParticipantId participant, Runtime::SaveRecordId record,
                                                       std::size_t maximumBytes, std::weak_ptr<const AiSceneRuntime> runtime,
                                                       AiSaveSceneSource scene) {
        using Bound = Result<AiSaveCaptureParticipant>;
        if (!participant.IsValid() || !record.IsValid() || maximumBytes < 8 || maximumBytes > DefaultRuntimeSaveCopiedRecordBytes ||
            !scene || runtime.expired())
            return Bound::Failure(MakeError(SaveErrors::ParticipantAdapterMissing));
        try {
            return Bound::Success(
                {{participant,
                  ParticipantSchemaVersion::Create(SaveSchemaVersion).Value(),
                  SaveParticipantScope::RuntimeScene,
                  SaveParticipantRole::Capture,
                  true,
                  {maximumBytes, 1, 1},
                  {},
                  {record}},
                 std::make_shared<CaptureAdapter>(participant, record, maximumBytes, std::move(runtime), std::move(scene))});
        } catch (const std::bad_alloc &) {
            return Bound::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }
}  // namespace Horo::AI
