#pragma once

#include "Horo/Navigation/NavigationDefinitionSerialization.h"

#include <bit>
#include <utility>

namespace Horo::Navigation::DefinitionCodec {
    /** @brief Internal transactional parser failure; never crosses the public Result boundary. */
    struct InvalidPayload final {};

    /** @brief Payload-local portable little-endian writer; outer HNAV owns checksums and framing. */
    struct Writer final {
        std::vector<std::byte> bytes;

        void Integer(const std::uint64_t value, const std::size_t width) {
            for (std::size_t index = 0; index < width; ++index)
                bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
        }

        void U32(const std::uint32_t value) {
            Integer(value, 4);
        }

        void U64(const std::uint64_t value) {
            Integer(value, 8);
        }

        void Float(const float value) {
            U32(std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
        }

        void Text(const std::string &value) {
            U32(static_cast<std::uint32_t>(value.size()));
            const auto source = std::as_bytes(std::span{value.data(), value.size()});
            bytes.insert(bytes.end(), source.begin(), source.end());
        }
    };

    /** @brief Bounds every read and count before allocation; no borrowed payload survives decode. */
    struct Reader final {
        std::span<const std::byte> bytes;
        std::size_t offset{};

        std::uint64_t Integer(const std::size_t width) {
            if (width > bytes.size() - offset)
                throw InvalidPayload{};
            std::uint64_t value{};
            for (std::size_t index = 0; index < width; ++index)
                value |= std::to_integer<std::uint64_t>(bytes[offset++]) << (index * 8U);
            return value;
        }

        std::uint32_t U32() {
            return static_cast<std::uint32_t>(Integer(4));
        }

        std::uint64_t U64() {
            return Integer(8);
        }

        float Float() {
            return std::bit_cast<float>(U32());
        }

        std::uint32_t Count(const std::size_t maximum, const std::size_t minimumRowBytes) {
            const auto count = U32();
            if (count > maximum || count > (bytes.size() - offset) / minimumRowBytes)
                throw InvalidPayload{};
            return count;
        }

        std::string Text() {
            const auto count = Count(NavigationDefinition::MaximumProfileNameBytes, 1);
            const auto source = bytes.subspan(offset, count);
            offset += count;
            return {reinterpret_cast<const char *>(source.data()), source.size()};
        }

        template <typename Identity> Identity Id() {
            const auto value = Identity::Create(U64());
            if (value.HasError())
                throw InvalidPayload{};
            return value.Value();
        }
    };

    /** @brief Emits the already canonical registry and authored policy without generated data. */
    std::vector<std::byte> EncodePayload(const NavigationDefinition &definition);
    /** @brief Parses complete bounded fields; public caller performs semantic and canonical validation. */
    NavigationDefinitionInput DecodePayload(std::span<const std::byte> bytes);
}  // namespace Horo::Navigation::DefinitionCodec
