#pragma once

#include "SceneCellContentFields.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>

namespace Horo::Runtime::CellCookDetail {
    /** @brief Bounded inverse of the Scene semantic writer, retaining typed identity factory failures. */
    class PayloadReader final {
    public:
        PayloadReader(const std::span<const std::byte> bytes, const std::size_t allocationBytes, const CancellationToken &cancellation)
            : bytes_(bytes), allocationRemaining_(allocationBytes),
              workRemaining_(bytes.size() == std::numeric_limits<std::size_t>::max() ? bytes.size() : bytes.size() + 1),
              cancellation_(cancellation) {}

        template <class... T> void Fields(T &...values) {
            (Add(values), ...);
        }

        template <class T> void Add(T &value) {
            if (!Work())
                return;
            if constexpr (std::is_integral_v<T>) {
                Integer(value);
            } else if constexpr (std::is_enum_v<T>) {
                std::underlying_type_t<T> raw{};
                Add(raw);
                if (!error_)
                    value = static_cast<T>(raw);
            } else if constexpr (std::is_same_v<T, float>) {
                std::uint32_t raw{};
                Add(raw);
                if (!error_)
                    value = std::bit_cast<float>(raw);
            } else if constexpr (std::is_same_v<T, double>) {
                std::uint64_t raw{};
                Add(raw);
                if (!error_)
                    value = std::bit_cast<double>(raw);
            } else if constexpr (requires { value.Bytes(); }) {
                ByteIdentity(value);
            } else if constexpr (requires { value.Asset(); }) {
                Assets::AssetId raw;
                Add(raw);
                if (!error_) {
                    if (raw.IsValid())
                        Assign(value, T::Create(raw));
                    else
                        value = T{};
                }
            } else if constexpr (requires { value.Value(); }) {
                ValueIdentity(value);
            } else if constexpr (requires { ContentFields(value); }) {
                std::apply([this](auto &...fields) {
                    Fields(fields...);
                }, ContentFields(value));
            } else {
                Add(value.value);
            }
        }

        void Add(std::string &value) {
            if (!Work())
                return;
            const auto size = Count();
            if (error_ || size > bytes_.size() - position_ || !Allocate(size)) {
                if (!error_)
                    Fail(SceneCellPayloadErrors::Invalid);
                return;
            }
            value.resize(size);
            Raw(std::as_writable_bytes(std::span{value.data(), value.size()}));
        }

        template <class T> void Add(std::optional<T> &value) {
            if (!Work())
                return;
            bool present{};
            Add(present);
            if (error_)
                return;
            if (!present) {
                value.reset();
                return;
            }
            value.emplace();
            Add(*value);
        }

        template <class... T> void Add(std::variant<T...> &value) {
            if (!Work())
                return;
            const auto index = Count();
            if (!error_)
                Variant<0>(value, index);
        }

        template <class T> void Add(std::vector<T> &values) {
            Sequence(values, std::numeric_limits<std::size_t>::max());
        }

        /** @brief Applies root semantic count ceilings before allocating its decoded entity or dependency table. */
        template <class T> void Sequence(std::vector<T> &values, const std::size_t maximumCount) {
            if (!Work())
                return;
            const auto count = Count();
            if (error_ || count > maximumCount || count > allocationRemaining_ / sizeof(T)) {
                if (!error_)
                    Fail(SceneCellPayloadErrors::CapacityExceeded);
                return;
            }
            if (!Allocate(count * sizeof(T)))
                return;
            if constexpr (std::is_same_v<T, std::byte> || std::is_same_v<T, std::uint8_t>) {
                if (count > bytes_.size() - position_) {
                    Fail(SceneCellPayloadErrors::Invalid);
                    return;
                }
                values.resize(count);
                Raw(std::as_writable_bytes(std::span{values}));
            } else {
                if (count > workRemaining_) {
                    Fail(SceneCellPayloadErrors::CapacityExceeded);
                    return;
                }
                values.resize(count);
                for (auto &value : values) {
                    if (error_ || cancellation_.IsCancellationRequested())
                        break;
                    Add(value);
                }
            }
        }

        template <class T, std::size_t N> void Add(std::array<T, N> &values) {
            if (!Work())
                return;
            if (Count() != N) {
                Fail(SceneCellPayloadErrors::Invalid);
                return;
            }
            if (error_)
                return;
            if constexpr (std::is_same_v<T, std::byte> || std::is_same_v<T, std::uint8_t>) {
                Raw(std::as_writable_bytes(std::span{values}));
            } else {
                for (auto &value : values) {
                    if (error_ || cancellation_.IsCancellationRequested())
                        break;
                    Add(value);
                }
            }
        }

        /** @brief Requires complete consumption; malformed, truncated and trailing input cannot expose a partial model. */
        [[nodiscard]] Result<void> Finish() const {
            if (cancellation_.IsCancellationRequested())
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
            if (error_)
                return Result<void>::Failure(*error_);
            if (position_ != bytes_.size())
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid, "CoreEcs payload contains trailing bytes."));
            return Result<void>::Success();
        }

    private:
        /** @brief Charges logical container/string bytes before any corresponding allocation is attempted. */
        bool Allocate(const std::size_t bytes) {
            if (bytes > allocationRemaining_) {
                Fail(SceneCellPayloadErrors::CapacityExceeded);
                return false;
            }
            allocationRemaining_ -= bytes;
            return true;
        }

        /** @brief Bounds recursive units as well as bytes so zero-width marker types cannot create unbounded work. */
        bool Work() {
            if (error_)
                return false;
            if (cancellation_.IsCancellationRequested()) {
                Fail(SceneCellPayloadErrors::Cancelled);
                return false;
            }
            if (!workRemaining_) {
                Fail(SceneCellPayloadErrors::CapacityExceeded);
                return false;
            }
            --workRemaining_;
            return true;
        }

        /** @brief Retains the first typed failure instead of overwriting a more useful identity/parser cause. */
        void Fail(const ErrorCodeDescriptor &descriptor) {
            if (!error_)
                error_ = MakeError(descriptor);
        }

        /** @brief Reads the canonical fixed eight-byte integer representation without signed conversion overflow. */
        template <class T> void Integer(T &value) {
            if (bytes_.size() - position_ < 8) {
                Fail(SceneCellPayloadErrors::Invalid);
                return;
            }
            std::uint64_t raw{};
            for (unsigned i = 0; i < 8; ++i)
                raw |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[position_ + i])) << (i * 8U);
            position_ += 8;
            if constexpr (std::is_signed_v<T>) {
                const auto signedValue = std::bit_cast<std::int64_t>(raw);
                if (signedValue < std::numeric_limits<T>::min() || signedValue > std::numeric_limits<T>::max()) {
                    Fail(SceneCellPayloadErrors::Invalid);
                    return;
                }
                value = static_cast<T>(signedValue);
            } else {
                if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
                    Fail(SceneCellPayloadErrors::Invalid);
                    return;
                }
                value = static_cast<T>(raw);
            }
        }

        /** @brief Converts a wire count only after it fits the actual host index type. */
        std::size_t Count() {
            std::uint64_t count{};
            Integer(count);
            if (count > std::numeric_limits<std::size_t>::max()) {
                Fail(SceneCellPayloadErrors::CapacityExceeded);
                return 0;
            }
            return static_cast<std::size_t>(count);
        }

        /** @brief Copies explicit byte sequences in cancellable units after complete range validation. */
        void Raw(std::span<std::byte> destination) {
            if (destination.size() > bytes_.size() - position_) {
                Fail(SceneCellPayloadErrors::Invalid);
                return;
            }
            while (!destination.empty()) {
                if (cancellation_.IsCancellationRequested()) {
                    Fail(SceneCellPayloadErrors::Cancelled);
                    return;
                }
                const auto count = std::min<std::size_t>(65536, destination.size());
                std::ranges::copy(bytes_.subspan(position_, count), destination.begin());
                position_ += count;
                destination = destination.subspan(count);
            }
        }

        /** @brief Commits a successfully admitted strong value, preserving the owner's exact typed rejection otherwise. */
        template <class T> void Assign(T &value, Result<T> admitted) {
            if (admitted.HasError()) {
                if (!error_)
                    error_ = admitted.ErrorValue();
                return;
            }
            value = std::move(admitted).Value();
        }

        /** @brief Reconstructs persistent GUIDs through their owning byte factory, preserving invalid optional sentinels. */
        template <class T> void ByteIdentity(T &value) {
            std::remove_cvref_t<decltype(value.Bytes())> bytes{};
            Add(bytes);
            if (error_)
                return;
            if constexpr (requires { T::FromBytes(bytes); }) {
                value = T::FromBytes(bytes);
            } else {
                if (bytes != T{}.Bytes())
                    Assign(value, T::Create(bytes));
                else
                    value = T{};
            }
        }

        /** @brief Uses existing typed string/numeric factories while preserving exact default sentinels for disabled fields. */
        template <class T> void ValueIdentity(T &value) {
            using V = std::remove_cvref_t<decltype(value.Value())>;
            V raw{};
            Add(raw);
            if (error_)
                return;
            if (raw == T{}.Value()) {
                value = T{};
                return;
            }
            if constexpr (requires { T::Parse(raw); })
                Assign(value, T::Parse(raw));
            else if constexpr (requires { T::FromValue(raw); })
                value = T::FromValue(raw);
            else
                Assign(value, T::Create(raw));
        }

        /** @brief Admits the explicit variant tag before constructing one known semantic alternative. */
        template <std::size_t I, class... T> void Variant(std::variant<T...> &value, const std::size_t index) {
            if constexpr (I == sizeof...(T)) {
                Fail(SceneCellPayloadErrors::Invalid);
            } else if (index == I) {
                value.template emplace<I>();
                Add(std::get<I>(value));
            } else
                Variant<I + 1>(value, index);
        }

        std::span<const std::byte> bytes_;
        std::size_t position_{};
        std::size_t allocationRemaining_{};
        std::size_t workRemaining_{};
        const CancellationToken &cancellation_;
        std::optional<Error> error_;
    };
}  // namespace Horo::Runtime::CellCookDetail
