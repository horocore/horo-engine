#pragma once

#include "Horo/Runtime/Save/SaveCanonicalCodec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Runtime::CanonicalCodecDetail {
    [[nodiscard]] inline bool ValidLimits(const CanonicalCodecLimits &value) noexcept {
        return value.maximumBytes && value.maximumDecodedBytes && value.maximumStringBytes && value.maximumCollectionElements &&
               value.maximumFields && value.maximumNestingDepth && value.maximumStringBytes <= value.maximumBytes &&
               value.maximumReadWorkBytes && value.maximumReadWorkBytes <= 256ULL * 1024 * 1024 &&
               value.maximumBytes <= 64ULL * 1024 * 1024 && value.maximumDecodedBytes <= 128ULL * 1024 * 1024 &&
               value.maximumNestingDepth <= 64 && value.maximumCollectionElements <= 1'048'576 && value.maximumFields <= 65'536 &&
               value.maximumCollectionElements <= std::numeric_limits<std::uint32_t>::max() &&
               value.maximumFields <= std::numeric_limits<std::uint32_t>::max();
    }

    template <typename Unsigned> [[nodiscard]] std::array<std::byte, sizeof(Unsigned)> ToLittleEndian(const Unsigned value) noexcept {
        static_assert(std::is_unsigned_v<Unsigned>);
        std::array<std::byte, sizeof(Unsigned)> output{};
        std::size_t shift{};
        for (std::byte &byte : output) {
            byte = static_cast<std::byte>(value >> shift);
            shift += 8U;
        }
        return output;
    }

    template <typename Unsigned> [[nodiscard]] Unsigned FromLittleEndian(const std::array<std::byte, sizeof(Unsigned)> &bytes) noexcept {
        static_assert(std::is_unsigned_v<Unsigned>);
        Unsigned output{};
        std::size_t shift{};
        for (const std::byte byte : bytes) {
            output |= static_cast<Unsigned>(std::to_integer<std::uint8_t>(byte)) << shift;
            shift += 8U;
        }
        return output;
    }

    [[nodiscard]] inline bool BytesLess(const std::span<const std::byte> left, const std::span<const std::byte> right) noexcept {
        return std::ranges::lexicographical_compare(left, right);
    }

    [[nodiscard]] inline bool BytesEqual(const std::span<const std::byte> left, const std::span<const std::byte> right) noexcept {
        return std::ranges::equal(left, right);
    }

    [[nodiscard]] inline std::size_t MaximumDepth(const std::span<const CanonicalEncodedValue> values) noexcept {
        std::size_t depth{};
        for (const auto &value : values)
            depth = std::max(depth, value.StructuralDepth());
        return depth;
    }

    template <typename Value, typename Less, typename Equal>
    [[nodiscard]] std::vector<const Value *> OrderedUnique(const std::span<const Value> values, Less less, Equal equal, bool &duplicate) {
        std::vector<const Value *> ordered;
        ordered.reserve(values.size());
        for (const auto &value : values)
            ordered.push_back(&value);
        std::ranges::sort(ordered, less);
        duplicate = false;
        for (std::size_t index = 1; index < ordered.size(); ++index) {
            if (equal(*ordered[index - 1], *ordered[index])) {
                duplicate = true;
                break;
            }
        }
        return ordered;
    }
}  // namespace Horo::Runtime::CanonicalCodecDetail

namespace Horo::Runtime {
    struct CanonicalPathNode final {
        CanonicalPathNode(CanonicalFieldId fieldValue, std::shared_ptr<const CanonicalPathNode> parentValue, const std::size_t depthValue)
            : field(fieldValue), parent(std::move(parentValue)), depth(depthValue) {}

        CanonicalFieldId field;
        std::shared_ptr<const CanonicalPathNode> parent;
        std::size_t depth{};
    };
}  // namespace Horo::Runtime
