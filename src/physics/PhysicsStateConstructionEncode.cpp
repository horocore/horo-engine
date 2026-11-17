#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionCodec.h"
#include "PhysicsStateValidationInternal.h"

#include <algorithm>

namespace Horo::Physics::StateDetail {
    namespace {
        std::size_t NodeBytes(const PhysicsStateShapeRecipe &node) noexcept {
            if (const auto *shape = std::get_if<PhysicsShapeDescriptor>(&node.geometry)) {
                constexpr std::array<std::size_t, 4> sizes{12, 4, 8, 16};
                return 11 + sizes[shape->index()];
            }
            return 18;
        }

        bool Add(std::size_t count, std::size_t stride, std::size_t maximum, std::size_t &size) noexcept {
            if (count > (maximum - size) / stride)
                return false;
            size += count * stride;
            return true;
        }

        Result<std::size_t> Size(const PhysicsStateShapeRecipes &r, std::size_t maximum) {
            using Outcome = Result<std::size_t>;
            if (maximum < 16)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            std::size_t size = 16;
            for (const auto &node : r.nodes)
                if (!Add(1, NodeBytes(node), maximum, size))
                    return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            if (!Add(r.edges.size(), 40, maximum, size) || !Add(r.roots.size(), 12, maximum, size))
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(size);
        }

        bool OutputAliases(const PhysicsStateShapeRecipes &r, std::span<std::uint32_t> scratch, std::span<std::byte> output) noexcept {
            auto bytes = std::span{reinterpret_cast<std::uint8_t *>(output.data()), output.size()};
            return Overlaps(bytes, r.nodes) || Overlaps(bytes, r.edges) || Overlaps(bytes, r.roots) ||
                   Overlaps(bytes, std::span<const std::uint32_t>{scratch});
        }

        void Rows(Wire &wire, const PhysicsStateShapeRecipes &r) noexcept {
            for (auto node : r.nodes)
                static_cast<void>(ConstructionShapeNodeFields(wire, node, false));
            for (auto edge : r.edges)
                ConstructionShapeEdgeFields(wire, edge);
            for (auto root : r.roots)
                ConstructionShapeRootFields(wire, root);
        }
    }  // namespace

    /** @copydoc EncodeConstructionShapes */
    Result<std::size_t> EncodeConstructionShapes(const PhysicsStateShapeRecipes &r, const PhysicsStateConstructionLimits &limits,
                                                 PhysicsStateFpControl current, std::span<std::uint32_t> scratch,
                                                 std::span<std::byte> output) {
        if (OutputAliases(r, scratch, output))
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (auto valid = ValidatePhysicsStateShapeRecipes(r, limits, current, scratch); valid.HasError())
            return Result<std::size_t>::Failure(valid.ErrorValue());
        auto size = Size(r, std::min(output.size(), limits.maximumBytes));
        if (size.HasError())
            return size;
        ConstructionShapeHeader header{.nodes = static_cast<std::uint32_t>(r.nodes.size()),
                                       .edges = static_cast<std::uint32_t>(r.edges.size()),
                                       .roots = static_cast<std::uint32_t>(r.roots.size())};
        Wire wire{output.first(size.Value())};
        ConstructionShapeHeaderFields(wire, header);
        Rows(wire, r);
        if (wire.Failed() || wire.Size() != size.Value())
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return Result<std::size_t>::Success(wire.Size());
    }
}  // namespace Horo::Physics::StateDetail
