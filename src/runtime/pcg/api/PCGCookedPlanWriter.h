#pragma once
/** @file PCGCookedPlanWriter.h @brief Target-private canonical bounded plan encoding. */
#include "Horo/PCG/PCGGraphAsset.h"

#include <bit>
#include <type_traits>

namespace Horo::PCG::detail {
    class BoundedPlanWriter final {
    public:
        explicit BoundedPlanWriter(const std::size_t maximum) : maximum_(maximum) {}

        [[nodiscard]] bool Integer(const std::uint64_t value, const std::size_t width) {
            if (width > maximum_ - bytes_.size())
                return false;
            for (std::size_t offset = width; offset > 0; --offset)
                bytes_.push_back(static_cast<std::uint8_t>(value >> ((offset - 1) * 8U)));
            return true;
        }

        [[nodiscard]] bool Bytes(const std::span<const std::uint8_t> value) {
            if (value.size() > maximum_ - bytes_.size())
                return false;
            bytes_.insert(bytes_.end(), value.begin(), value.end());
            return true;
        }

        [[nodiscard]] bool Value(const PCGGraphValue &value) {
            if (value.index() == 0 || !Integer(value.index(), 1))
                return false;
            return std::visit([this]<typename T>(const T &typed) {
                return WriteValuePayload(typed);
            }, value);
        }

        [[nodiscard]] std::vector<std::uint8_t> Take() && {
            return std::move(bytes_);
        }

    private:
        template <typename T> [[nodiscard]] bool WriteValuePayload(const T &typed) {
            if constexpr (std::is_same_v<T, bool>)
                return Integer(typed ? 1 : 0, 1);
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return Integer(static_cast<std::uint64_t>(typed), 8);
            else if constexpr (std::is_same_v<T, std::uint64_t>)
                return Integer(typed, 8);
            else if constexpr (std::is_same_v<T, double>)
                return Integer(std::bit_cast<std::uint64_t>(typed), 8);
            else if constexpr (std::is_same_v<T, Math::Vec2>)
                return Integer(std::bit_cast<std::uint32_t>(typed.x), 4) && Integer(std::bit_cast<std::uint32_t>(typed.y), 4);
            else if constexpr (std::is_same_v<T, Math::Vec3>)
                return Integer(std::bit_cast<std::uint32_t>(typed.x), 4) && Integer(std::bit_cast<std::uint32_t>(typed.y), 4) &&
                       Integer(std::bit_cast<std::uint32_t>(typed.z), 4);
            else if constexpr (std::is_same_v<T, Math::Vec4>)
                return Integer(std::bit_cast<std::uint32_t>(typed.x), 4) && Integer(std::bit_cast<std::uint32_t>(typed.y), 4) &&
                       Integer(std::bit_cast<std::uint32_t>(typed.z), 4) && Integer(std::bit_cast<std::uint32_t>(typed.w), 4);
            else
                return false;
        }

        std::size_t maximum_{};
        std::vector<std::uint8_t> bytes_;
    };

}  // namespace Horo::PCG::detail
