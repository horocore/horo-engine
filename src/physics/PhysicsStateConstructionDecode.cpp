#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionCodec.h"
#include "PhysicsStateValidationInternal.h"

namespace Horo::Physics::StateDetail {
    namespace {
        template <class T> std::span<std::uint8_t> Bytes(std::span<T> v) noexcept {
            return {reinterpret_cast<std::uint8_t *>(v.data()), v.size_bytes()};
        }

        template <class A, class B> bool Aliases(std::span<A> a, std::span<B> b) noexcept {
            return Overlaps(Bytes(a), std::span<const B>{b});
        }

        bool AliasesStorage(const ConstructionShapeStorage &s, std::span<std::uint32_t> depths, std::span<const std::byte> input) noexcept {
            return Aliases(s.nodes, s.edges) || Aliases(s.nodes, s.roots) || Aliases(s.edges, s.roots) || Aliases(depths, s.nodes) ||
                   Aliases(depths, s.edges) || Aliases(depths, s.roots) || Overlaps(Bytes(s.nodes), input) ||
                   Overlaps(Bytes(s.edges), input) || Overlaps(Bytes(s.roots), input) || Overlaps(Bytes(depths), input);
        }

        bool ConsumeMinimum(std::uint32_t count, std::size_t stride, std::size_t &remaining) noexcept {
            if (count > remaining / stride)
                return false;
            remaining -= std::size_t{count} * stride;
            return true;
        }

        bool MinimumBytes(ConstructionShapeHeader h, std::size_t inputBytes) noexcept {
            if (inputBytes < 16)
                return false;
            auto remaining = inputBytes - 16;
            return ConsumeMinimum(h.nodes, 15, remaining) && ConsumeMinimum(h.edges, 40, remaining) &&
                   ConsumeMinimum(h.roots, 12, remaining);
        }

        bool Counts(ConstructionShapeHeader h, const PhysicsStateConstructionLimits &limits, const ConstructionShapeStorage &s,
                    std::span<std::uint32_t> depths) noexcept {
            return h.nodes <= limits.maximumNodes && h.edges <= limits.maximumEdges && h.roots <= limits.maximumRoots &&
                   h.nodes <= s.nodes.size() && h.edges <= s.edges.size() && h.roots <= s.roots.size() && h.nodes <= depths.size();
        }

        bool ReadRows(Wire &wire, ConstructionShapeStorage s) noexcept {
            for (auto &node : s.nodes)
                if (!ConstructionShapeNodeFields(wire, node, true))
                    return false;
            for (auto &edge : s.edges)
                ConstructionShapeEdgeFields(wire, edge);
            for (auto &root : s.roots)
                ConstructionShapeRootFields(wire, root);
            return wire.Complete();
        }
    }  // namespace

    /** @copydoc DecodeConstructionShapes */
    Result<PhysicsStateShapeRecipes> DecodeConstructionShapes(std::span<const std::byte> input,
                                                              const PhysicsStateConstructionLimits &limits, PhysicsStateFpControl current,
                                                              std::span<std::uint32_t> depths, ConstructionShapeStorage storage) {
        using Outcome = Result<PhysicsStateShapeRecipes>;
        if (input.size() > limits.maximumBytes)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        Wire wire{input};
        ConstructionShapeHeader header;
        ConstructionShapeHeaderFields(wire, header);
        if (wire.Failed() || header.tag != 0x31504853U || !MinimumBytes(header, input.size()))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (!Counts(header, limits, storage, depths))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        storage = {storage.nodes.first(header.nodes), storage.edges.first(header.edges), storage.roots.first(header.roots)};
        depths = depths.first(header.nodes);
        if (AliasesStorage(storage, depths, input))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (!ReadRows(wire, storage))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        PhysicsStateShapeRecipes view{storage.nodes, storage.edges, storage.roots};
        if (auto valid = ValidatePhysicsStateShapeRecipes(view, limits, current, depths); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        return Outcome::Success(view);
    }
}  // namespace Horo::Physics::StateDetail
