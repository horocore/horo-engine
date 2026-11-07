#include "EditorPlaySavePrefabCapture.h"

#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Application::Internal {
    Runtime::SaveRecordId PlaySavePrefabRecord() {
        return Runtime::SaveRecordId::Parse("ac3cd58d-641a-46df-b9e4-ddf369343441").Value();
    }

    Runtime::CanonicalStateParticipantDescriptor PlaySavePrefabDescriptor() {
        return {.participant = Runtime::SaveParticipantId::Parse("horo.editor.play.prefabs.v1").Value(),
                .schemaVersion = Runtime::ParticipantSchemaVersion::Create(1).Value(),
                .scope = Runtime::SaveParticipantScope::RuntimeScene,
                .roles = Runtime::SaveParticipantRole::Capture,
                .required = true,
                .limits = {8U << 20U, 1, 8},
                .dependencies = {},
                .ownedRecords = {PlaySavePrefabRecord()}};
    }

    Result<std::optional<Prefab::PrefabPersistentState>> DecodePlaySavePrefabs(const Runtime::ValidatedSaveArchive &archive,
                                                                               const std::optional<Prefab::PrefabLimitProfile> &profile,
                                                                               std::size_t authoredCount) {
        using Outcome = Result<std::optional<Prefab::PrefabPersistentState>>;
        const auto bytes = archive.SelectChunk(PlaySavePrefabRecord());
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        if (!profile)
            return bytes.Value() ? Outcome::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported)) : Outcome::Success({});
        if (!bytes.Value() || authoredCount > 16'384)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureIncomplete));
        auto decoded = Prefab::DecodePrefabPersistentState(*bytes.Value(), {}, *profile);
        if (decoded.HasError())
            return Outcome::Failure(decoded.ErrorValue());
        if (decoded.Value().world != archive.Header().world)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid));
        std::size_t remaining = 16'384 - authoredCount;
        for (const auto &instance : decoded.Value().instances) {
            if (instance.members.size() > remaining)
                return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureBudgetExceeded));
            remaining -= instance.members.size();
        }
        return Outcome::Success(std::optional{std::move(decoded).Value()});
    }

    Result<Runtime::CanonicalCaptureDisposition> PlaySavePrefabCaptureAdapter::Capture(const Runtime::CanonicalCaptureContext &context,
                                                                                       Runtime::ICanonicalCaptureSink &sink) const {
        using Outcome = Result<Runtime::CanonicalCaptureDisposition>;
        const auto inventory = services_->Inventory();
        if (!inventory)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
        auto captured = inventory->Capture();
        if (captured.HasError())
            return Outcome::Failure(captured.ErrorValue());
        // The canonical context and actual inventory must name the same currently published Scene.
        for (const auto &record : captured.Value().identities.Records()) {
            if (record.disposition == Runtime::PersistentEntityDisposition::Tombstone)
                continue;
            const auto entity = captured.Value().identities.Resolve(record.identity, record.generation);
            if (entity.HasError() || entity.Value().runtime.value != context.provenance.sceneIncarnation)
                return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
        }
        auto encoded = Prefab::EncodePrefabPersistentState(captured.Value().prefabs, {}, services_->Provider().LimitProfile());
        if (encoded.HasError())
            return Outcome::Failure(encoded.ErrorValue());
        if (auto written = sink.WriteCopied(PlaySavePrefabRecord(), encoded.Value().Bytes()); written.HasError())
            return Outcome::Failure(written.ErrorValue());
        return Outcome::Success(Runtime::CanonicalCaptureDisposition::Captured);
    }
}  // namespace Horo::Application::Internal
