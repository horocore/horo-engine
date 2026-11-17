#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateEnvelopeInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Physics::StateDetail {
    using namespace EnvelopeDetail;

    /** @copydoc EncodeSnapshotEnvelope */
    Result<std::size_t> EncodeSnapshotEnvelope(const PhysicsStateCompatibility &compatibility, std::span<const std::byte> solver,
                                               const PhysicsStateWorldView &world, std::span<std::byte> output,
                                               std::span<std::uint8_t> eventOrdinalScratch, std::span<const std::byte> construction) {
        using Outcome = Result<std::size_t>;
        if (auto valid = ValidatePhysicsStateCompatibility(compatibility); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (!EncodeInputsDisjoint(output, solver, world, eventOrdinalScratch, construction))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (auto valid = ValidateWorld(world, eventOrdinalScratch); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        auto header = Header(compatibility, solver.size(), world);
        header.constructionBytes = construction.size();
        std::size_t size{};
        if (!EncodeSize(header, output.size(), size))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        Wire prefix{output.first(SnapshotHeader::Bytes)};
        SnapshotHeaderFields(prefix, header);
        std::ranges::copy(solver, output.begin() + SnapshotHeader::Bytes);
        const auto constructionOffset = SnapshotHeader::Bytes + solver.size();
        std::ranges::copy(construction, output.begin() + constructionOffset);
        const auto rowsOffset = constructionOffset + construction.size();
        Wire rows{output.subspan(rowsOffset, size - rowsOffset)};
        Rows(rows, world.previousPairs, world.publishedEvents);
        return Outcome::Success(size);
    }

}  // namespace Horo::Physics::StateDetail
