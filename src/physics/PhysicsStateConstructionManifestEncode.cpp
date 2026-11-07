#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionManifestCodec.h"

#include <algorithm>

namespace Horo::Physics::StateDetail {
    namespace {
        ConstructionManifestHeader Header(const PhysicsStateConstructionManifest &m) noexcept {
            return {.counts = {static_cast<std::uint32_t>(m.bodies.size()), static_cast<std::uint32_t>(m.colliders.size()),
                               static_cast<std::uint32_t>(m.materials.size()), static_cast<std::uint32_t>(m.joints.size()),
                               static_cast<std::uint32_t>(m.fixtures.size()), static_cast<std::uint32_t>(m.queryChildren.size()),
                               static_cast<std::uint32_t>(m.owners.size())}};
        }

        Result<std::size_t> EncodeRows(const PhysicsStateConstructionManifest &m, ConstructionManifestHeader &header, std::size_t shapeSize,
                                       std::size_t rowsSize, std::span<std::byte> output) {
            using Outcome = Result<std::size_t>;
            const auto offset = ConstructionManifestHeader::Bytes + shapeSize;
            Wire rows{output.subspan(offset, rowsSize)};
            if (!ConstructionManifestRows(rows, m) || rows.Size() != rowsSize)
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            header.shapeBytes = shapeSize;
            Wire prefix{output.first(ConstructionManifestHeader::Bytes)};
            ConstructionManifestHeaderFields(prefix, header);
            return Outcome::Success(offset + rowsSize);
        }
    }  // namespace

    /** @copydoc EncodeConstructionManifest */
    Result<std::size_t> EncodeConstructionManifest(const PhysicsStateConstructionManifest &m,
                                                   const PhysicsStateConstructionManifestLimits &limits, PhysicsStateFpControl current,
                                                   std::span<std::uint32_t> scratch, std::span<std::byte> output) {
        using Outcome = Result<std::size_t>;
        if (!ConstructionManifestOutputDisjoint(m, scratch, output))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (auto valid = ValidatePhysicsStateConstructionManifest(m, limits, current, scratch); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto maximum = std::min(output.size(), limits.maximumEncodedBytes);
        if (maximum < ConstructionManifestHeader::Bytes)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        auto header = Header(m);
        std::size_t rowsSize{};
        const auto remaining = maximum - ConstructionManifestHeader::Bytes;
        if (!ConstructionManifestRowSize(header, remaining, rowsSize))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        auto shapes = EncodeConstructionShapes(m.shapes, limits.shapes, current, scratch,
                                               output.subspan(ConstructionManifestHeader::Bytes, remaining - rowsSize));
        if (shapes.HasError())
            return Outcome::Failure(shapes.ErrorValue());
        return EncodeRows(m, header, shapes.Value(), rowsSize, output);
    }
}  // namespace Horo::Physics::StateDetail
