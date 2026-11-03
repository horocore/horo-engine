#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionManifestCodec.h"

#include <array>

namespace Horo::Physics::StateDetail {
    namespace {
        bool RowsFit(const ConstructionManifestHeader &h, const PhysicsStateConstructionManifestLimits &l) noexcept {
            const std::array<std::uint32_t, 7> maxima{l.maximumBodies,   l.maximumColliders,     l.maximumMaterials, l.maximumJoints,
                                                      l.maximumFixtures, l.maximumQueryChildren, l.maximumOwners};
            for (std::size_t i = 0; i < maxima.size(); ++i)
                if (h.counts[i] > maxima[i])
                    return false;
            return true;
        }

        bool ShapeCountsFit(const ConstructionShapeHeader &h, const PhysicsStateConstructionLimits &l) noexcept {
            return h.tag == 0x31504853U && h.nodes <= l.maximumNodes && h.edges <= l.maximumEdges && h.roots <= l.maximumRoots;
        }

        bool ShapeMinimumFits(const ConstructionShapeHeader &h, std::size_t bytes) noexcept {
            // Minimum node: FP9 + kind1 + primitiveTag1 + sphere4. All fixed edges/roots use their exact field extents.
            if (bytes < 16 || h.nodes > (bytes - 16) / 15)
                return false;
            auto remaining = bytes - 16 - std::size_t{h.nodes} * 15;
            if (h.edges > remaining / 40)
                return false;
            remaining -= std::size_t{h.edges} * 40;
            return h.roots <= remaining / 12;
        }

        Result<ConstructionManifestHeader> ReadManifestHeader(std::span<const std::byte> input) {
            using Outcome = Result<ConstructionManifestHeader>;
            if (input.size() < ConstructionManifestHeader::Bytes + 16)
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            Wire prefix{input.first(ConstructionManifestHeader::Bytes)};
            ConstructionManifestHeader header;
            ConstructionManifestHeaderFields(prefix, header);
            if (!prefix.Complete() || header.tag != 0x31435048U || header.shapeBytes < 16 ||
                header.shapeBytes > input.size() - ConstructionManifestHeader::Bytes)
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            return Outcome::Success(header);
        }

        Result<ConstructionShapeHeader> ReadShapeHeader(std::span<const std::byte> input, std::size_t bytes,
                                                        const PhysicsStateConstructionLimits &ceilings) {
            using Outcome = Result<ConstructionShapeHeader>;
            Wire prefix{input.subspan(ConstructionManifestHeader::Bytes, 16)};
            ConstructionShapeHeader header;
            ConstructionShapeHeaderFields(prefix, header);
            if (!prefix.Complete() || header.tag != 0x31504853U || !ShapeMinimumFits(header, bytes))
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (!ShapeCountsFit(header, ceilings))
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(header);
        }
    }  // namespace

    /** @copydoc SizeConstructionManifest */
    Result<PhysicsStateConstructionManifestLimits> SizeConstructionManifest(std::span<const std::byte> input,
                                                                            const PhysicsStateConstructionManifestLimits &ceilings) {
        using Outcome = Result<PhysicsStateConstructionManifestLimits>;
        if (input.size() > ceilings.maximumEncodedBytes)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        auto parsed = ReadManifestHeader(input);
        if (parsed.HasError())
            return Outcome::Failure(parsed.ErrorValue());
        const auto &header = parsed.Value();
        if (!RowsFit(header, ceilings))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        std::size_t rowBytes{};
        const auto shapeBytes = static_cast<std::size_t>(header.shapeBytes);
        const auto tailBytes = input.size() - ConstructionManifestHeader::Bytes - shapeBytes;
        if (!ConstructionManifestRowSize(header, tailBytes, rowBytes) || rowBytes != tailBytes)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto parsedShapes = ReadShapeHeader(input, shapeBytes, ceilings.shapes);
        if (parsedShapes.HasError())
            return Outcome::Failure(parsedShapes.ErrorValue());
        const auto &shapes = parsedShapes.Value();
        auto sized = ceilings;
        sized.shapes.maximumNodes = shapes.nodes;
        sized.shapes.maximumEdges = shapes.edges;
        sized.shapes.maximumRoots = shapes.roots;
        sized.maximumBodies = header.counts[0];
        sized.maximumColliders = header.counts[1];
        sized.maximumMaterials = header.counts[2];
        sized.maximumJoints = header.counts[3];
        sized.maximumFixtures = header.counts[4];
        sized.maximumQueryChildren = header.counts[5];
        sized.maximumOwners = header.counts[6];
        sized.maximumEncodedBytes = input.size();
        return Outcome::Success(sized);
    }
}  // namespace Horo::Physics::StateDetail
