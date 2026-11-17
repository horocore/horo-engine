#pragma once
#include "Horo/AI/AIErrors.h"
#include "Horo/Foundation/Result.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Horo::AI::Detail {
    /** @brief One bounded canonical little-endian transport; failure remains sticky and exposes no partial candidate. */
    class DecisionWriter final {
    public:
        explicit DecisionWriter(std::size_t maximum) : maximum_(maximum) {}

        void Unsigned(std::uint64_t value, std::size_t width = 8) {
            if (!valid_)
                return;
            if (width > maximum_ - std::min(maximum_, bytes_.size())) {
                valid_ = false;
                return;
            }
            for (std::size_t index = 0; index < width; ++index) {
                bytes_.push_back(static_cast<std::uint8_t>(value & 255U));
                value >>= 8U;
            }
        }

        template <typename Enum> void Enumeration(Enum value) {
            Unsigned(static_cast<std::underlying_type_t<Enum>>(value), 1);
        }

        template <typename Identity> void IdentityValue(Identity value, bool optional = false) {
            if (!optional && !value.IsValid()) {
                valid_ = false;
                return;
            }
            Unsigned(value.Value());
        }

        bool Count(std::size_t value, std::size_t maximum) {
            if (!valid_)
                return false;
            if (value > maximum) {
                valid_ = false;
                return false;
            }
            Unsigned(value, 4);
            return true;
        }

        void String(std::string_view value, std::size_t maximum = 4096) {
            if (!valid_)
                return;
            if (value.size() > maximum || value.find('\0') != std::string_view::npos) {
                valid_ = false;
                return;
            }
            Unsigned(value.size(), 4);
            for (const auto character : value)
                Unsigned(static_cast<std::uint8_t>(character), 1);
        }

        void Bytes(std::span<const std::uint8_t> value) {
            if (!valid_)
                return;
            if (value.size() > maximum_ - std::min(maximum_, bytes_.size())) {
                valid_ = false;
                return;
            }
            for (const auto byte : value)
                Unsigned(byte, 1);
        }

        void Reject() noexcept {
            valid_ = false;
        }

        [[nodiscard]] bool Valid() const noexcept {
            return valid_;
        }

        [[nodiscard]] std::vector<std::uint8_t> Take() && {
            return std::move(bytes_);
        }

        [[nodiscard]] std::span<const std::uint8_t> Data() const noexcept {
            return bytes_;
        }

    private:
        std::size_t maximum_;
        std::vector<std::uint8_t> bytes_;
        bool valid_{true};
    };

    class DecisionReader final {
    public:
        explicit DecisionReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

        [[nodiscard]] std::uint64_t Unsigned(std::size_t width = 8) noexcept {
            if (width > bytes_.size() - offset_) {
                valid_ = false;
                return 0;
            }
            std::uint64_t value{};
            for (std::size_t index = 0; index < width; ++index)
                value |= static_cast<std::uint64_t>(bytes_[offset_++]) << (8U * index);
            return value;
        }

        template <typename Enum> [[nodiscard]] Enum Enumeration(Enum count) noexcept {
            const auto value = Unsigned(1);
            if (value >= static_cast<std::underlying_type_t<Enum>>(count))
                valid_ = false;
            return static_cast<Enum>(value);
        }

        template <typename Identity> [[nodiscard]] Identity IdentityValue(bool optional = false) {
            const auto value = Unsigned();
            if (optional && value == 0)
                return {};
            auto identity = Identity::Create(value);
            if (identity.HasError()) {
                valid_ = false;
                return {};
            }
            return identity.Value();
        }

        [[nodiscard]] std::size_t Count(std::size_t maximum) noexcept {
            const auto count = Unsigned(4);
            if (count > maximum) {
                valid_ = false;
                return 0;
            }
            return static_cast<std::size_t>(count);
        }

        [[nodiscard]] bool Boolean() noexcept {
            const auto value = Unsigned(1);
            if (value > 1)
                valid_ = false;
            return value != 0;
        }

        [[nodiscard]] std::string String(std::size_t maximum = 4096) {
            const auto count = Count(maximum);
            if (count > bytes_.size() - offset_) {
                valid_ = false;
                return {};
            }
            if (count == 0)
                return {};
            std::string value(reinterpret_cast<const char *>(bytes_.data() + offset_), count);
            offset_ += count;
            if (value.find('\0') != std::string::npos)
                valid_ = false;
            return value;
        }

        [[nodiscard]] std::span<const std::uint8_t> Bytes(std::size_t count) noexcept {
            if (count > bytes_.size() - offset_) {
                valid_ = false;
                return {};
            }
            auto result = bytes_.subspan(offset_, count);
            offset_ += count;
            return result;
        }

        void Reject() noexcept {
            valid_ = false;
        }

        [[nodiscard]] bool Valid() const noexcept {
            return valid_;
        }

        [[nodiscard]] bool Complete() const noexcept {
            return valid_ && offset_ == bytes_.size();
        }

    private:
        std::span<const std::uint8_t> bytes_;
        std::size_t offset_{};
        bool valid_{true};
    };
}  // namespace Horo::AI::Detail
