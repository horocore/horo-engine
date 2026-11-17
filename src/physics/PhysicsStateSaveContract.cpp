#include "PhysicsStateSaveContract.h"

#include "Horo/Physics/PhysicsStateSnapshot.h"

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsStateSaveParticipant */
    Runtime::SaveParticipantId PhysicsStateSaveParticipant() {
        return Runtime::SaveParticipantId::Parse("horo.physics.world.state.v2").Value();
    }

    /** @copydoc PhysicsStateSaveRecord */
    Runtime::SaveRecordId PhysicsStateSaveRecord() {
        return Runtime::SaveRecordId::Parse("faad6801-1aca-4f85-bd89-7e51583ca901").Value();
    }

    /** @copydoc PhysicsStateSaveDescriptor */
    Runtime::CanonicalStateParticipantDescriptor PhysicsStateSaveDescriptor() {
        return {.participant = PhysicsStateSaveParticipant(),
                .schemaVersion = Runtime::ParticipantSchemaVersion::Create(PhysicsStateSnapshotVersion).Value(),
                .scope = Runtime::SaveParticipantScope::RuntimeScene,
                .roles = Runtime::SaveParticipantRole::Capture | Runtime::SaveParticipantRole::Restore,
                .required = true,
                .limits = {MaximumPhysicsStateSaveRecordBytes, 1, 1},
                .dependencies = {},
                .ownedRecords = {PhysicsStateSaveRecord()}};
    }
}  // namespace Horo::Physics::Detail
