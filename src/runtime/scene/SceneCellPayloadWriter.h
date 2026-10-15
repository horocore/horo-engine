#pragma once

#include "SceneCellContentFields.h"

#include <algorithm>
#include <bit>
#include <type_traits>

namespace Horo::Runtime::CellCookDetail {
    /** @brief Two-pass bounded semantic writer sharing the existing Scene field mapping, never native object bytes. */
    class PayloadWriter final {
    public:
        PayloadWriter(const CancellationToken &cancellation, const std::size_t maximumBytes, std::vector<std::byte> *output = nullptr)
            : cancellation_(cancellation), maximum_(maximumBytes), output_(output) {}

        template <class... T> void Fields(const T &...values) {
            (Add(values), ...);
        }

        template <class T> void Add(const T &value) {
            if constexpr (std::is_integral_v<T>) {
                std::array<std::byte, 8> bytes{};
                const auto bits = static_cast<std::uint64_t>(value);
                for (unsigned i = 0; i < bytes.size(); ++i)
                    bytes[i] = static_cast<std::byte>(bits >> (8U * i));
                Bytes(bytes);
            } else if constexpr (std::is_enum_v<T>) {
                Add(static_cast<std::underlying_type_t<T>>(value));
            } else if constexpr (std::is_same_v<T, float>) {
                Add(std::bit_cast<std::uint32_t>(value));
            } else if constexpr (std::is_same_v<T, double>) {
                Add(std::bit_cast<std::uint64_t>(value));
            } else if constexpr (requires { value.Bytes(); }) {
                Add(value.Bytes());
            } else if constexpr (requires { value.Asset(); }) {
                Add(value.Asset());
            } else if constexpr (requires { value.Value(); }) {
                Add(value.Value());
            } else if constexpr (requires { ContentFields(value); }) {
                std::apply([this](const auto &...fields) {
                    Fields(fields...);
                }, ContentFields(value));
            } else {
                Add(value.value);
            }
        }

        void Add(const std::string &value) {
            Add(std::string_view{value});
        }

        void Add(const std::string_view value) {
            Add(value.size());
            Bytes(std::as_bytes(std::span{value.data(), value.size()}));
        }

        template <class T> void Add(const std::optional<T> &value) {
            Add(value.has_value());
            if (value)
                Add(*value);
        }

        template <class... T> void Add(const std::variant<T...> &value) {
            Add(value.index());
            std::visit([this](const auto &item) {
                Add(item);
            }, value);
        }

        template <class T> void Add(const std::vector<T> &values) {
            Add(std::span<const T>{values});
        }

        template <class T, std::size_t N> void Add(const std::array<T, N> &values) {
            Add(std::span<const T>{values});
        }

        template <class T> void Add(const std::span<const T> values) {
            Add(values.size());
            if constexpr (std::is_same_v<T, std::byte> || std::is_same_v<T, std::uint8_t>) {
                Bytes(std::as_bytes(values));
            } else {
                for (const auto &value : values) {
                    if (failed_ || cancellation_.IsCancellationRequested())
                        break;
                    Add(value);
                }
            }
        }

        /** @brief Appends already versioned literal header bytes through the same bounded copying path. */
        void Bytes(std::span<const std::byte> bytes) {
            if (failed_ || cancellation_.IsCancellationRequested())
                return;
            if (bytes.size() > maximum_ - position_ || (output_ && bytes.size() > output_->size() - position_)) {
                failed_ = true;
                return;
            }
            while (!bytes.empty()) {
                if (cancellation_.IsCancellationRequested())
                    return;
                const auto count = std::min<std::size_t>(65536, bytes.size());
                if (output_)
                    std::ranges::copy(bytes.first(count), output_->begin() + static_cast<std::ptrdiff_t>(position_));
                position_ += count;
                bytes = bytes.subspan(count);
            }
        }

        /** @brief Reports complete bounded measurement or the exact output size after the second pass. */
        [[nodiscard]] Result<std::size_t> Finish() const {
            if (cancellation_.IsCancellationRequested())
                return Result<std::size_t>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
            if (failed_)
                return Result<std::size_t>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
            return Result<std::size_t>::Success(position_);
        }

    private:
        const CancellationToken &cancellation_;
        std::size_t maximum_{};
        std::vector<std::byte> *output_{};
        std::size_t position_{};
        bool failed_{};
    };
}  // namespace Horo::Runtime::CellCookDetail
