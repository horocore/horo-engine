#pragma once

#include "Horo/Foundation/Sha256.h"
#include "Horo/Physics/PhysicsPose.h"

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string_view>

namespace Horo::Physics::StateDetail {
    /** @brief Explicit little-endian semantic hashing, used only with
     * already-admitted bounded Physics inventories. */
    class StateHash final {
    public:
        explicit StateHash(std::string_view domain) noexcept {
            (void)hash_.Update(std::as_bytes(std::span{domain.data(), domain.size()}));
        }

        void Word(std::uint64_t value) noexcept {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>((value >> (8 * index)) & 255U);
            (void)hash_.Update(bytes);
        }

        void Float(float value) noexcept {
            Word(std::bit_cast<std::uint32_t>(value));
        }

        void Vector(const Math::Vec3 &value) noexcept {
            Float(value.x);
            Float(value.y);
            Float(value.z);
        }

        void Pose(const PhysicsPose &value) noexcept {
            Vector(value.translation);
            Float(value.rotation.x);
            Float(value.rotation.y);
            Float(value.rotation.z);
            Float(value.rotation.w);
        }

        void Digest(const Sha256Digest &value) noexcept {
            (void)hash_.Update(std::as_bytes(std::span{value.bytes}));
        }

        template <class T> void Id(const T &value) noexcept {
            (void)hash_.Update(std::as_bytes(std::span{value.Bytes()}));
        }

        [[nodiscard]] Sha256Digest Finish() noexcept {
            return hash_.Finalize();
        }

    private:
        Sha256Builder hash_;
    };
}  // namespace Horo::Physics::StateDetail
