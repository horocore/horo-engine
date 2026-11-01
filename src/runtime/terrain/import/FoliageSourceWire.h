#pragma once
/** @file FoliageSourceWire.h @brief Private padding-free bounded foliage source scalar transport. */
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace Horo::Terrain::FoliageWire {
    /** @brief Writes admitted fixed-size source fields in little-endian order. */
    class Writer final {
    public:
        std::vector<std::byte> bytes;

        template <typename T> void Integer(const T value) {
            using Unsigned = std::make_unsigned_t<T>;
            const auto bits = static_cast<Unsigned>(value);
            for (std::size_t i = 0; i < sizeof(T); ++i)
                bytes.push_back(static_cast<std::byte>(bits >> (i * 8)));
        }

        void Float(const float value) {
            Integer(std::bit_cast<std::uint32_t>(value));
        }

        template <std::size_t Size> void Bytes(const std::array<std::uint8_t, Size> &value) {
            for (const auto byte : value)
                bytes.push_back(static_cast<std::byte>(byte));
        }
    };

    /** @brief Reads only within an already byte-budget-admitted immutable input. */
    class Reader final {
    public:
        explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

        template <typename T> bool Integer(T &value) noexcept {
            if (sizeof(T) > bytes_.size() - offset_)
                return false;
            using Unsigned = std::make_unsigned_t<T>;
            Unsigned bits{};
            for (std::size_t i = 0; i < sizeof(T); ++i)
                bits |= static_cast<Unsigned>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << (i * 8);
            value = std::bit_cast<T>(bits);
            return true;
        }

        bool Float(float &value) noexcept {
            std::uint32_t bits{};
            if (!Integer(bits))
                return false;
            value = std::bit_cast<float>(bits);
            return true;
        }

        template <std::size_t Size> bool Bytes(std::array<std::uint8_t, Size> &value) noexcept {
            if (Size > bytes_.size() - offset_)
                return false;
            for (auto &byte : value)
                byte = std::to_integer<std::uint8_t>(bytes_[offset_++]);
            return true;
        }

        [[nodiscard]] bool Complete() const noexcept {
            return offset_ == bytes_.size();
        }

    private:
        std::span<const std::byte> bytes_;
        std::size_t offset_{};
    };
}  // namespace Horo::Terrain::FoliageWire
