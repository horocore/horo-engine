#pragma once
#include "PhysicsSceneStateTarget.h"

#include <algorithm>
#include <array>
#include <limits>

namespace Horo::Physics::Detail {
    inline bool HasSceneStateContent(const Sha256Digest &digest, const std::shared_ptr<const void> &lease) noexcept {
        return lease && std::ranges::any_of(digest.bytes, [](auto byte) {
            return byte != 0;
        });
    }

    template <std::size_t N> Result<std::size_t> SceneStateRemainingBytes(std::size_t maximum, const std::array<std::size_t, N> &charges) {
        for (const auto bytes : charges) {
            if (bytes > maximum)
                return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            maximum -= bytes;
        }
        return Result<std::size_t>::Success(maximum);
    }

    inline std::size_t SceneStatePayloadSum(std::span<const std::size_t> charges) noexcept {
        std::size_t result{};
        for (const auto bytes : charges) {
            if (bytes > std::numeric_limits<std::size_t>::max() - result)
                return std::numeric_limits<std::size_t>::max();
            result += bytes;
        }
        return result;
    }
}  // namespace Horo::Physics::Detail
