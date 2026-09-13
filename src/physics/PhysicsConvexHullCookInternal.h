#pragma once

#include "Horo/Physics/PhysicsConvexHullCook.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Physics::Detail {
    inline constexpr std::array<std::uint8_t, 4> ConvexPayloadMagic{'P', 'H', 'C', 'V'};
    inline constexpr std::uint32_t ConvexPayloadVersion = 1;
    inline constexpr std::size_t ConvexPayloadHeaderBytes = 172;
    inline constexpr double ConvexHullPlaneToleranceMeters = 1.0e-6;

    [[nodiscard]] inline bool ConvexLimitsAreBounded(const PhysicsConvexHullCookLimits &limits) noexcept {
        return limits.maxSourceVertices > 0 && limits.maxSourceVertices <= PhysicsConvexHullCookLimits::MaximumSourceVertices &&
               limits.maxHullVertices >= 4 && limits.maxHullVertices <= PhysicsConvexHullCookLimits::MaximumHullVertices &&
               limits.maxPayloadBytes > 0 && limits.maxPayloadBytes <= PhysicsConvexHullCookLimits::MaximumPayloadBytes;
    }

    [[nodiscard]] inline std::array<double, 3> ConvexCross(const Math::Vec3 origin, const Math::Vec3 first,
                                                           const Math::Vec3 second) noexcept {
        const double ax = static_cast<double>(first.x) - origin.x;
        const double ay = static_cast<double>(first.y) - origin.y;
        const double az = static_cast<double>(first.z) - origin.z;
        const double bx = static_cast<double>(second.x) - origin.x;
        const double by = static_cast<double>(second.y) - origin.y;
        const double bz = static_cast<double>(second.z) - origin.z;
        return {ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx};
    }

    class ConvexPayloadWriter final {
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
}  // namespace Horo::Physics::Detail
