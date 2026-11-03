#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace Horo::Physics::StateDetail {
    /** @brief Checked little-endian scalar writer/reader; no native struct
     * representation is serialized. */
    class Wire final {
    public:
        /** @brief Borrows output for a complete already-validated size. */
        explicit Wire(std::span<std::byte> output) noexcept : output_(output) {}

        /** @brief Borrows immutable encoded input. */
        explicit Wire(std::span<const std::byte> input) noexcept : input_(input), reading_(true) {}

        /** @brief Verifies canonical re-encoding in place, without allocating another
         * wire buffer or changing input. */
        [[nodiscard]] static Wire Comparison(std::span<const std::byte> input) noexcept {
            Wire wire{input};
            wire.reading_ = false;
            wire.comparing_ = true;
            return wire;
        }

        /** @brief Processes a canonical unsigned integer in explicit byte order. */
        template <std::unsigned_integral T> void Integer(T &value) noexcept {
            if (!Reserve(sizeof(T)))
                return;
            if (reading_)
                value = 0;
            for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
                if (reading_)
                    value |= static_cast<T>(std::to_integer<std::uint8_t>(input_[offset_ + byte])) << (byte * 8);
                else if (comparing_)
                    failed_ |= input_[offset_ + byte] != static_cast<std::byte>((value >> (byte * 8)) & T{255});
                else
                    output_[offset_ + byte] = static_cast<std::byte>((value >> (byte * 8)) & T{255});
            }
            offset_ += sizeof(T);
        }

        /** @brief Exact finite float bits are preserved, including negative zero and
         * subnormals. */
        void Float(float &value) noexcept {
            auto bits = std::bit_cast<std::uint32_t>(value);
            Integer(bits);
            if (reading_)
                value = std::bit_cast<float>(bits);
        }

        /** @brief Booleans have exactly one byte and reject all unknown
         * representations. */
        void Boolean(bool &value) noexcept {
            std::uint8_t encoded = value ? 1 : 0;
            Integer(encoded);
            if (encoded > 1)
                failed_ = true;
            if (reading_)
                value = encoded == 1;
        }

        /** @brief Fixed scalar arrays have no implicit length, padding or optional
         * extension fields. */
        template <std::size_t N> void Floats(std::array<float, N> &values) noexcept {
            for (auto &value : values)
                Float(value);
        }

        /** @brief Reports the exact processed prefix. */
        std::size_t Size() const noexcept {
            return offset_;
        }

        /** @brief Reports truncation or malformed scalar representation. */
        bool Failed() const noexcept {
            return failed_;
        }

        /** @brief Requires the entire input, preventing ignored trailing required
         * fields. */
        bool Complete() const noexcept {
            return !failed_ && (!(reading_ || comparing_) || offset_ == input_.size());
        }

    private:
        /** @brief Uses subtraction before any indexing, independent of
         * attacker-provided lengths. */
        bool Reserve(std::size_t count) noexcept {
            const auto size = reading_ || comparing_ ? input_.size() : output_.size();
            if (failed_ || count > size - offset_) {
                failed_ = true;
                return false;
            }
            return true;
        }

        std::span<std::byte> output_;
        std::span<const std::byte> input_;
        std::size_t offset_{};
        bool reading_{}, comparing_{}, failed_{};
    };
}  // namespace Horo::Physics::StateDetail
