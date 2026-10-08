#pragma once

#include "SceneCellContentFields.h"

#include <algorithm>
#include <bit>
#include <type_traits>

namespace Horo::Runtime::CellCookDetail {
    /** @brief Streams a versioned semantic key without padding, pointers, native endian data or hash-time allocation. */
    class ContentHasher final {
    public:
        ContentHasher(const CancellationToken &cancellation, std::size_t maximumBytes)
            : cancellation_(cancellation), remaining_(maximumBytes) {}

        template <class... T> void Fields(const T &...values) {
            (Add(values), ...);
        }

        template <class T> void Add(const T &value) {
            if constexpr (std::is_integral_v<T>) {
                const auto bits = static_cast<std::uint64_t>(value);
                std::array<std::byte, 8> bytes{};
                for (unsigned i = 0; i < 8; ++i)
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

        void Add(std::string_view value) {
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

        template <class T> void Add(std::span<const T> values) {
            Add(values.size());
            if constexpr (std::is_same_v<T, std::byte> || std::is_same_v<T, std::uint8_t>) {
                Bytes(std::as_bytes(values));
            } else {
                if (values.size() > remaining_ / 8) {
                    failed_ = true;
                    return;
                }
                for (const auto &value : values) {
                    if (failed_ || cancellation_.IsCancellationRequested())
                        break;
                    Add(value);
                }
            }
        }

        [[nodiscard]] Result<Sha256Digest> Finish() {
            if (cancellation_.IsCancellationRequested())
                return Result<Sha256Digest>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
            if (failed_)
                return Result<Sha256Digest>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
            return Result<Sha256Digest>::Success(hash_.Finalize());
        }

    private:
        /** @brief Bounds total key bytes and observes cancellation on every fragment before SHA work. */
        void Bytes(std::span<const std::byte> bytes) {
            if (failed_ || cancellation_.IsCancellationRequested())
                return;
            if (bytes.size() > remaining_) {
                failed_ = true;
                return;
            }
            remaining_ -= bytes.size();
            while (!bytes.empty() && !cancellation_.IsCancellationRequested()) {
                const auto count = std::min(bytes.size(), std::size_t{65536});
                if (!hash_.Update(bytes.first(count))) {
                    failed_ = true;
                    return;
                }
                bytes = bytes.subspan(count);
            }
        }

        const CancellationToken &cancellation_;
        std::size_t remaining_;
        bool failed_{};
        Sha256Builder hash_;
    };
}  // namespace Horo::Runtime::CellCookDetail
