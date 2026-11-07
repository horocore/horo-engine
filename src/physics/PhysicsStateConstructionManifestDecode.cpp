#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionManifestCodec.h"

namespace Horo::Physics::StateDetail {
    namespace {
        bool Envelope(const ConstructionManifestHeader &h, std::size_t bytes, std::size_t &rowsSize) noexcept {
            if (h.tag != 0x31435048U || h.shapeBytes < 16 || h.shapeBytes > bytes - ConstructionManifestHeader::Bytes)
                return false;
            const auto tail = bytes - ConstructionManifestHeader::Bytes - static_cast<std::size_t>(h.shapeBytes);
            return ConstructionManifestRowSize(h, tail, rowsSize) && rowsSize == tail;
        }

        PhysicsStateConstructionManifest View(PhysicsStateShapeRecipes shapes, ConstructionManifestStorage s) noexcept {
            return {shapes, s.bodies, s.colliders, s.materials, s.joints, s.fixtures, s.queryChildren, s.owners};
        }

        bool CanonicalRows(std::span<const std::byte> bytes, const PhysicsStateConstructionManifest &m) noexcept {
            auto comparison = Wire::Comparison(bytes);
            return ConstructionManifestRows(comparison, m) && comparison.Complete();
        }

        Result<PhysicsStateConstructionManifest> ReadTail(std::span<const std::byte> tail, PhysicsStateShapeRecipes shapes,
                                                          ConstructionManifestStorage storage,
                                                          const PhysicsStateConstructionManifestLimits &limits,
                                                          PhysicsStateFpControl current, std::span<std::uint32_t> scratch) {
            using Outcome = Result<PhysicsStateConstructionManifest>;
            Wire rows{tail};
            if (!ReadConstructionManifestRows(rows, storage))
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto view = View(shapes, storage);
            if (auto valid = ValidatePhysicsStateConstructionManifest(view, limits, current, scratch); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            // Absent optionals and inactive union payloads have their canonical zero representation;
            // encoded garbage cannot disappear behind a false presence bit or fixed/no-mass selector.
            if (!CanonicalRows(tail, view))
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            return Outcome::Success(view);
        }
    }  // namespace

    /** @copydoc DecodeConstructionManifest */
    Result<PhysicsStateConstructionManifest> DecodeConstructionManifest(std::span<const std::byte> input,
                                                                        const PhysicsStateConstructionManifestLimits &limits,
                                                                        PhysicsStateFpControl current, std::span<std::uint32_t> scratch,
                                                                        ConstructionManifestStorage storage) {
        using Outcome = Result<PhysicsStateConstructionManifest>;
        if (input.size() > limits.maximumEncodedBytes)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (input.size() < ConstructionManifestHeader::Bytes)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        Wire prefix{input.first(ConstructionManifestHeader::Bytes)};
        ConstructionManifestHeader header;
        ConstructionManifestHeaderFields(prefix, header);
        std::size_t rowsSize{};
        if (!Envelope(header, input.size(), rowsSize))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (!ConstructionManifestStorageFits(header, storage, limits))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        storage = ConstructionManifestSlice(header, storage);
        if (!ConstructionManifestStorageDisjoint(storage, scratch, input))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        const auto shapeSize = static_cast<std::size_t>(header.shapeBytes);
        auto shapes = DecodeConstructionShapes(input.subspan(ConstructionManifestHeader::Bytes, shapeSize), limits.shapes, current, scratch,
                                               storage.shapes);
        if (shapes.HasError())
            return Outcome::Failure(shapes.ErrorValue());
        return ReadTail(input.subspan(ConstructionManifestHeader::Bytes + shapeSize, rowsSize), shapes.Value(), storage, limits, current,
                        scratch);
    }
}  // namespace Horo::Physics::StateDetail
