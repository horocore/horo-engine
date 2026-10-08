#pragma once

#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationTileArtifact.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace Horo::Navigation::TileArtifactInternal {
    /** @brief Fixed-width little-endian portable encoder with checked growth. */
    class Writer final {
    public:
        explicit Writer(const std::size_t maximum) : maximum_(maximum) {}

        void Integer(const std::uint64_t value, const std::size_t width = 4) {
            Require(width);
            for (std::size_t i = 0; i < width; ++i)
                bytes_.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
        }

        void Float(const float value) {
            Integer(std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
        }

        void Vector(const Math::Vec3 value) {
            Float(value.x);
            Float(value.y);
            Float(value.z);
        }

        void Digest(const Sha256Digest &value) {
            Raw(value.bytes);
        }

        void Raw(const std::span<const std::uint8_t> value) {
            Require(value.size());
            bytes_.insert(bytes_.end(), value.begin(), value.end());
        }

        [[nodiscard]] const std::vector<std::uint8_t> &Bytes() const noexcept {
            return bytes_;
        }

        [[nodiscard]] std::vector<std::uint8_t> TakeBytes() && noexcept {
            return std::move(bytes_);
        }

    private:
        void Require(const std::size_t count) const {
            if (count > maximum_ - bytes_.size())
                throw std::length_error("Navigation tile artifact byte ceiling");
        }

        std::vector<std::uint8_t> bytes_;
        std::size_t maximum_;
    };

    /** @brief Bounded decoder rejects impossible counts before allocating any table. */
    class Reader final {
    public:
        explicit Reader(const std::span<const std::uint8_t> input) : remaining_(input), allocationRemaining_(input.size() * 2) {}

        [[nodiscard]] std::uint64_t Integer(const std::size_t width = 4) {
            const auto bytes = Raw(width);
            std::uint64_t value{};
            for (std::size_t i = 0; i < width; ++i)
                value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
            return value;
        }

        [[nodiscard]] float Float() {
            return std::bit_cast<float>(static_cast<std::uint32_t>(Integer()));
        }

        [[nodiscard]] Math::Vec3 Vector() {
            return {Float(), Float(), Float()};
        }

        [[nodiscard]] Sha256Digest Digest() {
            Sha256Digest digest;
            std::ranges::copy(Raw(digest.bytes.size()), digest.bytes.begin());
            return digest;
        }

        [[nodiscard]] std::span<const std::uint8_t> Raw(const std::size_t count) {
            if (count > remaining_.size())
                throw std::invalid_argument("Truncated navigation tile artifact");
            const auto bytes = remaining_.first(count);
            remaining_ = remaining_.subspan(count);
            return bytes;
        }

        [[nodiscard]] std::size_t Count(const std::size_t maximum, const std::size_t minimumRowBytes) {
            const auto count = Integer();
            if (count > maximum || count > remaining_.size() / minimumRowBytes)
                throw std::invalid_argument("Invalid navigation tile artifact count");
            // Every decoded row is bounded by twice its portable minimum (including alignment).
            if (count > allocationRemaining_ / (minimumRowBytes * 2))
                throw std::invalid_argument("Navigation decoded storage ceiling");
            allocationRemaining_ -= static_cast<std::size_t>(count) * minimumRowBytes * 2;
            return static_cast<std::size_t>(count);
        }

        [[nodiscard]] bool Empty() const noexcept {
            return remaining_.empty();
        }

    private:
        std::span<const std::uint8_t> remaining_;
        std::size_t allocationRemaining_;
    };

    /** @brief Encodes the exact validated owned project profile captured by the producer.
     * @param writer Bounded canonical payload writer. @param profile Validated project authority. */
    void WriteContentProfile(Writer &writer, const NavigationProjectProfile &profile);
    /** @brief Reconstructs and validates captured profile facts and their deterministic fingerprint.
     * @param reader Bounded payload reader. @return Owned validated profile.
     * @throws std::invalid_argument Malformed or noncanonical captured profile. */
    [[nodiscard]] NavigationProjectProfile ReadContentProfile(Reader &reader);

    /** @brief Constructs a domain error without discarding its registered identity. */
    template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &error) {
        return Result<T>::Failure(MakeError(error));
    }

    /** @brief Checks required digest evidence. */
    [[nodiscard]] inline bool Present(const Sha256Digest &digest) noexcept {
        return std::ranges::any_of(digest.bytes, [](const auto byte) {
            return byte != 0;
        });
    }
}  // namespace Horo::Navigation::TileArtifactInternal
