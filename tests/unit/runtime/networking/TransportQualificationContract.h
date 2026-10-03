#pragma once

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Network::Qualification {
    struct Packet final {
        std::uint32_t channel{};
        std::vector<std::byte> bytes;

        bool operator==(const Packet &) const = default;
    };

    // Test-only projection: production and deterministic transports retain their
    // distinct public contracts and native implementation boundaries.
    class Driver {
    public:
        virtual ~Driver() = default;
        [[nodiscard]] virtual std::size_t MaximumMessageBytes() const = 0;
        [[nodiscard]] virtual std::uint32_t MaximumChannels() const = 0;
        [[nodiscard]] virtual bool Send(std::uint32_t channel, std::span<const std::byte> bytes) = 0;
        [[nodiscard]] virtual std::vector<Packet> Receive(std::size_t expected) = 0;
        virtual void Shutdown() = 0;
    };

    inline void VerifyDeliveryAndBounds(Driver &driver) {
        const std::vector<std::byte> shortPayload{std::byte{0x11}};
        const std::vector<std::byte> fragmentedPayload{std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25},
                                                       std::byte{0x26}, std::byte{0x27}, std::byte{0x28}, std::byte{0x29}};
        auto mutablePayload = fragmentedPayload;
        REQUIRE(driver.Send(0, shortPayload));
        REQUIRE(driver.Send(0, mutablePayload));
        mutablePayload.assign(mutablePayload.size(), std::byte{0xff});

        const auto secondChannel = driver.MaximumChannels() > 1 ? 1U : 0U;
        REQUIRE(driver.Send(secondChannel, shortPayload));
        const std::vector expected{Packet{0, shortPayload}, Packet{0, fragmentedPayload}, Packet{secondChannel, shortPayload}};
        REQUIRE(driver.Receive(expected.size()) == expected);

        // The backend must reject unsupported channel and oversize input before
        // admitting work; no completion may appear for either rejected send.
        REQUIRE_FALSE(driver.Send(driver.MaximumChannels(), shortPayload));
        const std::vector oversized(driver.MaximumMessageBytes() + 1, std::byte{0x41});
        REQUIRE_FALSE(driver.Send(0, oversized));
        REQUIRE(driver.Receive(0).empty());
        driver.Shutdown();
        REQUIRE_FALSE(driver.Send(0, shortPayload));
    }
}  // namespace Horo::Network::Qualification
