#include "CanonicalPhysicsStateShapeRecipe.h"
#include "CanonicalPhysicsStateTopologyHash.h"

namespace Horo::Physics::Detail {
    namespace {
        template <class T> void Metadata(StateDetail::StateHash &hash, const T &value) noexcept {
            hash.Id(value.layer);
            hash.Id(value.profile);
            hash.Id(value.channel);
            hash.Word(static_cast<std::uint8_t>(value.response));
            hash.Word(value.trigger);
            hash.Word(value.material.has_value());
            if (value.material)
                HashStateMaterial(hash, *value.material);
            hash.Word(value.surface.has_value());
            if (value.surface)
                HashStateSurface(hash, *value.surface);
        }

        void Descriptor(StateDetail::StateHash &hash, const PhysicsQueryFixtureDescriptor &value,
                        const Sha256Digest &actualRecipe) noexcept {
            hash.Digest(actualRecipe);
            Metadata(hash, value);
            hash.Word(value.subshape.has_value());
            if (value.subshape)
                hash.Word(value.subshape->Value());
            if (const auto *compound = std::get_if<PhysicsCompoundShapeDescriptor>(&value.shape)) {
                hash.Word(compound->children.size());
                for (const auto &child : compound->children) {
                    hash.Word(child.subshape.Value());
                    Metadata(hash, child);
                }
            }
        }
    }  // namespace

    /** @copydoc HashStateFixtureBody */
    Result<void> HashStateFixtureBody(StateDetail::StateHash &hash, const CanonicalWorld &world,
                                      const CanonicalPhysicsStateBodyBinding &binding, const CanonicalPhysicsStateBodyRecordIndex &index,
                                      const CanonicalPhysicsStateTopologyMap &map) {
        if (index.index >= world.query.fixtures.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto &record = world.query.fixtures[index.index];
        if (record.fixture.body != binding.body || record.nativeBody.GetIndexAndSequenceNumber() != binding.nativeId)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto shape = FindStateShapeId(map, record.fixture.shape);
        const auto *native = world.native.system->GetBodyLockInterfaceNoLock().TryGetBody(record.nativeBody);
        if (shape == 0 || !native || native->GetShape() != record.shape.GetPtr())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        if (!record.bodyCreationControl)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        hash.Word(static_cast<std::uint8_t>(record.bodyCreationControl->architecture));
        hash.Word(record.bodyCreationControl->value);
        hash.Word(binding.id);
        hash.Word(shape);
        Descriptor(hash, record.descriptor, record.recipe);
        HashStateNativeBody(hash, *native);
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
