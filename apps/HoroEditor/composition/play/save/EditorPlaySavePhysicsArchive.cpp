#include "EditorPlaySavePhysicsArchive.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "PhysicsStateSaveContract.h"

#include <algorithm>

namespace Horo::Application::Internal {
    /** @copydoc ValidatePlaySavePhysicsArchive */
    Result<PlaySavePhysicsReadPolicy> ValidatePlaySavePhysicsArchive(const Runtime::ValidatedSaveArchive &archive,
                                                                     const PlaySaveStorageSchema &schema) {
        using Outcome = Result<PlaySavePhysicsReadPolicy>;
        const auto expected = Physics::Detail::PhysicsStateSaveDescriptor();
        const auto &rows = archive.Manifest().participants;
        const auto found = std::ranges::find(rows, expected.participant, &Runtime::SaveManifestParticipant::participant);
        if (!schema.physics) {
            if (found != rows.end())
                return Outcome::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
            return Outcome::Success(PlaySavePhysicsReadPolicy::Unavailable);
        }
        if (*schema.physics != expected)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureRegistryStale));
        if (found == rows.end())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureIncomplete));
        if (found->schemaVersion != expected.schemaVersion || !found->required || found->chunks != expected.ownedRecords)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureRegistryStale));
        const auto bytes = archive.SelectChunk(Physics::Detail::PhysicsStateSaveRecord());
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        if (!bytes.Value())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureIncomplete));
        return Outcome::Success(PlaySavePhysicsReadPolicy::CanonicalRecordRequired);
    }
}  // namespace Horo::Application::Internal
