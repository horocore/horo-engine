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
        using RecordBytes = Result<std::vector<std::uint8_t>>;

        /** @brief Resolves the active runtime entity to its durable record and generation. */
        Result<AiSavedEntityReference> ProjectEntity(const PersistentEntityIdentityMap &identities,
                                                     const BlackboardStoredEntityReference &reference) {
            using Projected = Result<AiSavedEntityReference>;
            const auto id = identities.Find({{reference.sceneIncarnation}, {reference.slot, reference.generation}});
            if (id.HasError())
                return Projected::Failure(id.ErrorValue());
            const auto records = identities.Records();
            const auto record = std::ranges::lower_bound(records, id.Value(), {}, &PersistentEntityRecord::identity);
            if (record == records.end() || record->identity != id.Value())
                return Projected::Failure(MakeError(AIErrors::CanonicalStateInvalid));
            return Projected::Success({id.Value().Bytes(), record->generation.value});
        }

        /** @brief Finds the current entity and rejects stale generations and tombstones before AI restore. */
        Result<BlackboardStoredEntityReference> ResolveEntity(const RuntimeSceneView &scene, const PersistentEntityIdentityMap &identities,
                                                              const AiSavedEntityReference &reference) {
            using Resolved = Result<BlackboardStoredEntityReference>;
            const auto id = PersistentEntityId::FromBytes(reference.identity);
            if (id.HasError())
                return Resolved::Failure(id.ErrorValue());
            const auto entity = identities.Resolve(id.Value(), {reference.generation});
            if (entity.HasError())
                return Resolved::Failure(entity.ErrorValue());
            const auto view = scene.Get(entity.Value());
            if (view.HasError())
                return Resolved::Failure(view.ErrorValue());
            return Resolved::Success({entity.Value().runtime.value, entity.Value().entity.index, entity.Value().entity.generation});
        }

        /** @brief Counts live Scene components so an incomplete runtime inventory cannot enter a required record. */
        std::size_t SceneAgentCount(const RuntimeSceneView &scene) noexcept {
            std::size_t count{};
            for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
                const auto entity = scene.EntityAt(slot);
                if (entity && (entity->components->aiAgent || entity->components->aiController))
                    ++count;
            }
            return count;
        }

        /** @brief Copies the base-agent record into the current aggregate capture. */
        class CaptureAdapter final : public ICanonicalStateAdapter {
        public:
            CaptureAdapter(SaveParticipantId participant, SaveRecordId record, std::uint64_t maximumBytes,
                           std::weak_ptr<const AiSceneRuntime> runtime, AiSaveSceneSource scene, AiSaveIdentitySource identities)
                : participant_(std::move(participant)), record_(std::move(record)), maximumBytes_(maximumBytes),
                  runtime_(std::move(runtime)), scene_(std::move(scene)), identities_(std::move(identities)),
                  owner_(std::this_thread::get_id()) {}

            CaptureResult Capture(const CanonicalCaptureContext &context, ICanonicalCaptureSink &sink) const override {
                const auto allowance = Admit(context);
                if (allowance.HasError())
                    return CaptureResult::Failure(allowance.ErrorValue());
                const auto encoded = CaptureRecord(context.provenance, allowance.Value());
                if (encoded.HasError())
                    return CaptureResult::Failure(encoded.ErrorValue());
                if (const auto written = sink.WriteCopied(record_, std::as_bytes(std::span{encoded.Value()})); written.HasError())
                    return CaptureResult::Failure(written.ErrorValue());
                return CaptureResult::Success(CanonicalCaptureDisposition::Captured);
            }

        private:
            /** @brief Checks the exact owner lane and the operation's remaining record allowance. */
            [[nodiscard]] Result<std::size_t> Admit(const CanonicalCaptureContext &context) const {
                using Allowance = Result<std::size_t>;
                if (owner_ != std::this_thread::get_id() || context.participant != participant_ ||
                    context.schemaVersion.Value() != (identities_ ? AiCanonicalReferenceRecordVersion : AiCanonicalRecordVersion) ||
                    context.scope != SaveParticipantScope::RuntimeScene)
                    return Allowance::Failure(MakeError(SaveErrors::CaptureRegistryStale));
                const auto &remaining = context.admission;
                const auto budget = std::min({maximumBytes_, remaining.operationPayloadBytes, remaining.participantPayloadBytes,
                                              remaining.maximumCopiedRecordBytes});
                if (budget == 0 || !remaining.operationRecords || !remaining.participantRecords || !remaining.operationSegments)
                    return Allowance::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
                return Allowance::Success(static_cast<std::size_t>(budget));
            }

            /** @brief Captures and encodes only the AI population for the current Scene revision. */
            [[nodiscard]] RecordBytes CaptureRecord(const RuntimeSaveCaptureProvenance &provenance, std::size_t budget) const {
                const auto owner = runtime_.lock();
                if (!owner)
                    return RecordBytes::Failure(MakeError(AIErrors::RuntimeUnavailable));
                const auto scene = scene_();
                if (!scene || !scene->IsCurrent() || scene->RuntimeId().value != provenance.sceneIncarnation ||
                    scene->StructuralRevision() != provenance.sceneRevision)
                    return RecordBytes::Failure(
                        MakeError(SaveErrors::CaptureContextInvalid, "AI capture requires the exact current Scene."));
                const auto state = owner->CaptureCanonicalState(*scene, budget);
                if (state.HasError())
                    return RecordBytes::Failure(state.ErrorValue());
                if (state.Value().agents.size() != SceneAgentCount(*scene))
                    return RecordBytes::Failure(
                        MakeError(AIErrors::CanonicalStateInvalid, "AI capture requires the complete Scene population."));
                return Encode(state.Value(), *scene, budget);
            }

            /** @brief Acquires the authoritative mapping once for the whole save operation. */
            [[nodiscard]] RecordBytes Encode(const AiCanonicalState &state, const RuntimeSceneView &scene, std::size_t budget) const {
                if (!identities_)
                    return EncodeAiCanonicalStateRecord(state, budget);
                auto identities = identities_(scene);
                if (identities.HasError())
                    return RecordBytes::Failure(identities.ErrorValue());
                return EncodeAiCanonicalStateRecord(state, budget, [&](const BlackboardStoredEntityReference &value) {
                    return ProjectEntity(identities.Value(), value);
                });
            }

            SaveParticipantId participant_;
            SaveRecordId record_;
            std::uint64_t maximumBytes_;
            std::weak_ptr<const AiSceneRuntime> runtime_;
            AiSaveSceneSource scene_;
            AiSaveIdentitySource identities_;
            std::thread::id owner_;
        };
    }  // namespace

    /** @copydoc AiSaveCaptureDescriptor */
    Runtime::CanonicalStateParticipantDescriptor AiSaveCaptureDescriptor(Runtime::SaveParticipantId participant,
                                                                         Runtime::SaveRecordId record, std::size_t maximumBytes,
                                                                         bool persistentReferences) {
        return {participant,
                ParticipantSchemaVersion::Create(persistentReferences ? AiCanonicalReferenceRecordVersion : AiCanonicalRecordVersion)
                    .Value(),
                SaveParticipantScope::RuntimeScene,
                SaveParticipantRole::Capture,
                true,
                {maximumBytes, 1, 1},
                {},
                {record}};
    }

    /** @copydoc BindAiSaveCapture */
    Result<AiSaveCaptureParticipant> BindAiSaveCapture(Runtime::SaveParticipantId participant, Runtime::SaveRecordId record,
                                                       std::size_t maximumBytes, std::weak_ptr<const AiSceneRuntime> runtime,
                                                       AiSaveSceneSource scene, AiSaveIdentitySource identities) {
        using Bound = Result<AiSaveCaptureParticipant>;
        if (!participant.IsValid() || !record.IsValid() || maximumBytes < 8 || maximumBytes > DefaultRuntimeSaveCopiedRecordBytes ||
            !scene || runtime.expired())
            return Bound::Failure(MakeError(SaveErrors::ParticipantAdapterMissing));
        try {
            return Bound::Success({AiSaveCaptureDescriptor(participant, record, maximumBytes, static_cast<bool>(identities)),
                                   std::make_shared<CaptureAdapter>(participant, record, maximumBytes, std::move(runtime), std::move(scene),
                                                                    std::move(identities))});
        } catch (const std::bad_alloc &) {
            return Bound::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc DecodeAiSavePopulation */
    Result<AiCanonicalState> DecodeAiSavePopulation(std::span<const std::uint8_t> bytes, std::size_t maximumBytes,
                                                    const Runtime::RuntimeSceneView &scene,
                                                    const Runtime::PersistentEntityIdentityMap &identities) {
        if (!scene.IsCurrent())
            return Result<AiCanonicalState>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        return DecodeAiCanonicalStateRecord(bytes, maximumBytes, [&](const AiSavedEntityReference &value) {
            return ResolveEntity(scene, identities, value);
        });
    }
}  // namespace Horo::AI
