#pragma once

#include <array>
#include <cstdint>

namespace Horo::Runtime::SceneTest {
    template <typename Identity> Identity Id(const std::uint8_t marker) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = marker;
        return Identity::FromBytes(bytes).Value();
    }
}  // namespace Horo::Runtime::SceneTest
