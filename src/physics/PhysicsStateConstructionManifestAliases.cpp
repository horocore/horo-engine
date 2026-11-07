#include "PhysicsStateConstructionManifestCodec.h"

namespace Horo::Physics::StateDetail {
    namespace {
        bool Aliases(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
            if (a.empty() || b.empty())
                return false;
            const auto first = reinterpret_cast<std::uintptr_t>(a.data());
            const auto second = reinterpret_cast<std::uintptr_t>(b.data());
            return first <= second ? second - first < a.size() : first - second < b.size();
        }
    }  // namespace

    bool ConstructionManifestStorageDisjoint(ConstructionManifestStorage s, std::span<std::uint32_t> scratch,
                                             std::span<const std::byte> input) noexcept {
        const std::array<std::span<const std::byte>, 12> spans{std::as_bytes(s.shapes.nodes),  std::as_bytes(s.shapes.edges),
                                                               std::as_bytes(s.shapes.roots),  std::as_bytes(s.bodies),
                                                               std::as_bytes(s.colliders),     std::as_bytes(s.materials),
                                                               std::as_bytes(s.joints),        std::as_bytes(s.fixtures),
                                                               std::as_bytes(s.queryChildren), std::as_bytes(s.owners),
                                                               std::as_bytes(scratch),         input};
        for (std::size_t index = 0; index < spans.size(); ++index)
            for (std::size_t prior = 0; prior < index; ++prior)
                if (Aliases(spans[index], spans[prior]))
                    return false;
        return true;
    }

    bool ConstructionManifestOutputDisjoint(const PhysicsStateConstructionManifest &m, std::span<std::uint32_t> scratch,
                                            std::span<std::byte> output) noexcept {
        const std::array<std::span<const std::byte>, 11> inputs{std::as_bytes(m.shapes.nodes),  std::as_bytes(m.shapes.edges),
                                                                std::as_bytes(m.shapes.roots),  std::as_bytes(m.bodies),
                                                                std::as_bytes(m.colliders),     std::as_bytes(m.materials),
                                                                std::as_bytes(m.joints),        std::as_bytes(m.fixtures),
                                                                std::as_bytes(m.queryChildren), std::as_bytes(m.owners),
                                                                std::as_bytes(scratch)};
        for (const auto input : inputs)
            if (Aliases(input, output))
                return false;
        return true;
    }
}  // namespace Horo::Physics::StateDetail
