#pragma once

#include "Horo/Physics/PhysicsTriangleMeshCook.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Physics::Detail {
    inline constexpr std::array<std::uint8_t, 4> TriangleMeshPayloadMagic{'P', 'H', 'T', 'M'};
    inline constexpr std::uint32_t TriangleMeshPayloadVersion = 1;
    inline constexpr std::size_t TriangleMeshPayloadHeaderBytes = 180;
    inline constexpr double TriangleMeshMinimumAreaSquared = 1.0e-12;

    template <typename T> [[nodiscard]] Result<T> TriangleMeshFailure(const ErrorCodeDescriptor &descriptor, std::string message) {
        return Result<T>::Failure(MakeError(descriptor, std::move(message)));
    }

    struct TriangleMeshEdgeUse final {
        std::uint32_t first{};
        std::uint32_t second{};
        bool forward{};
    };

    inline void AppendTriangleMeshEdges(std::vector<TriangleMeshEdgeUse> &edges, const std::array<std::uint32_t, 3> &indices) {
        const auto append = [&edges](const std::uint32_t from, const std::uint32_t to) {
            edges.push_back({std::min(from, to), std::max(from, to), from < to});
        };
        append(indices[0], indices[1]);
        append(indices[1], indices[2]);
        append(indices[2], indices[0]);
    }

    template <typename InvalidResult>
    [[nodiscard]] Result<void> ValidateTriangleMeshEdges(std::vector<TriangleMeshEdgeUse> &edges, InvalidResult &&invalidResult) {
        std::ranges::sort(edges, [](const TriangleMeshEdgeUse &left, const TriangleMeshEdgeUse &right) {
            if (left.first != right.first)
                return left.first < right.first;
            if (left.second != right.second)
                return left.second < right.second;
            return !left.forward && right.forward;
        });
        for (auto first = edges.begin(); first != edges.end();) {
            const auto last = std::find_if(first, edges.end(), [first](const TriangleMeshEdgeUse &edge) {
                return edge.first != first->first || edge.second != first->second;
            });
            if (const auto count = last - first; count > 2 || (count == 2 && first->forward == (first + 1)->forward))
                return std::forward<InvalidResult>(invalidResult)();
            first = last;
        }
        return Result<void>::Success();
    }

    [[nodiscard]] inline bool TriangleMeshLimitsAreBounded(const PhysicsTriangleMeshCookLimits &limits) noexcept {
        return limits.maxSourceVertices > 0 && limits.maxSourceVertices <= PhysicsTriangleMeshCookLimits::MaximumSourceVertices &&
               limits.maxTriangles > 0 && limits.maxTriangles <= PhysicsTriangleMeshCookLimits::MaximumTriangles &&
               limits.maxMaterialSlots > 0 && limits.maxMaterialSlots <= PhysicsTriangleMeshCookLimits::MaximumMaterialSlots &&
               limits.maxPayloadBytes > 0 && limits.maxPayloadBytes <= PhysicsTriangleMeshCookLimits::MaximumPayloadBytes;
    }

    class TriangleMeshPayloadWriter final {
    public:
        void Reserve(const std::size_t bytes) {
            bytes_.reserve(bytes);
        }

        void U8(const std::uint8_t value) {
            bytes_.push_back(value);
        }

        void U32(const std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                U8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }

        void U64(const std::uint64_t value) {
            for (unsigned shift = 0; shift < 64; shift += 8)
                U8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }

        void Float(float value) {
            if (value == 0.0F)
                value = 0.0F;
            U32(std::bit_cast<std::uint32_t>(value));
        }

        void Bytes(const std::span<const std::uint8_t> values) {
            bytes_.insert(bytes_.end(), values.begin(), values.end());
        }

        [[nodiscard]] const std::vector<std::uint8_t> &View() const noexcept {
            return bytes_;
        }

        [[nodiscard]] std::vector<std::uint8_t> Take() && {
            return std::move(bytes_);
        }

    private:
        std::vector<std::uint8_t> bytes_;
    };

    [[nodiscard]] inline double TriangleAreaSquared(const Math::Vec3 a, const Math::Vec3 b, const Math::Vec3 c) noexcept {
        const double abx = static_cast<double>(b.x) - a.x;
        const double aby = static_cast<double>(b.y) - a.y;
        const double abz = static_cast<double>(b.z) - a.z;
        const double acx = static_cast<double>(c.x) - a.x;
        const double acy = static_cast<double>(c.y) - a.y;
        const double acz = static_cast<double>(c.z) - a.z;
        const double x = aby * acz - abz * acy;
        const double y = abz * acx - abx * acz;
        const double z = abx * acy - aby * acx;
        return x * x + y * y + z * z;
    }
}  // namespace Horo::Physics::Detail
