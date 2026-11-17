#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateConstructionExport.h"
#include "PhysicsStateSurfaceBinding.h"

namespace Horo::Physics::Detail {
    namespace {
        Result<void> Children(const PhysicsQueryFixtureDescriptor &descriptor, CanonicalConstructionRows &rows,
                              PhysicsStateConstructionFixture &out) {
            out.childBegin = static_cast<std::uint32_t>(rows.children);
            const auto *compound = std::get_if<PhysicsCompoundShapeDescriptor>(&descriptor.shape);
            if (!compound)
                return Result<void>::Success();
            if (compound->children.size() > rows.storage.queryChildren.size() - rows.children)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            out.childCount = static_cast<std::uint32_t>(compound->children.size());
            for (const auto &child : compound->children)
                rows.storage.queryChildren[rows.children++] = {.subshape = child.subshape,
                                                               .material = child.material,
                                                               .layer = child.layer,
                                                               .profile = child.profile,
                                                               .channel = child.channel,
                                                               .response = child.response,
                                                               .trigger = child.trigger,
                                                               .surface = StateDetail::CaptureStateSurface(child.surface)};
            return Result<void>::Success();
        }

        Result<void> Fixture(const CanonicalWorld &world, const CanonicalPhysicsStateBodyBinding &binding,
                             const CanonicalPhysicsStateBodyRecordIndex &index, const CanonicalPhysicsStateTopologyMap &map,
                             CanonicalConstructionRows &rows) {
            if (index.index >= world.query.fixtures.size() || rows.fixtures >= rows.storage.fixtures.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            const auto &record = world.query.fixtures[index.index];
            if (record.fixture.body != binding.body || record.nativeBody.GetIndexAndSequenceNumber() != binding.nativeId)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
            if (!record.bodyCreationControl)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            const auto &d = record.descriptor;
            PhysicsStateConstructionFixture out{.body = binding.id,
                                                .shape = FindStateShapeId(map, record.fixture.shape),
                                                .originalPose = d.pose,
                                                .layer = d.layer,
                                                .profile = d.profile,
                                                .channel = d.channel,
                                                .response = d.response,
                                                .trigger = d.trigger,
                                                .subshape = d.subshape,
                                                .material = d.material,
                                                .surface = StateDetail::CaptureStateSurface(d.surface),
                                                .floatingPoint = *record.bodyCreationControl};
            if (auto copied = Children(d, rows, out); copied.HasError())
                return copied;
            rows.storage.fixtures[rows.fixtures++] = out;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ExportCanonicalConstructionFixtures */
    Result<void> ExportCanonicalConstructionFixtures(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                                                     const CanonicalPhysicsStateTopologyMap &map, CanonicalConstructionRows &rows) {
        if (map.bodies.size() != bindings.stableBodies.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        for (std::size_t i = 0; i < map.bodies.size(); ++i) {
            if (!map.bodies[i].fixture)
                continue;
            if (auto copied = Fixture(world, bindings.stableBodies[i], map.bodies[i], map, rows); copied.HasError())
                return copied;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
