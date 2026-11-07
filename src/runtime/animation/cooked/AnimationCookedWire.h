#pragma once
/** @file AnimationCookedWire.h
 * @brief Private bounded explicit little-endian wire cursor; no asset, scheduler or publication authority.
 */
#include "AnimationCookedTableLimits.h"
#include "Horo/Animation/AnimationCookedAssets.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <concepts>
#include <limits>
#include <tuple>
#include <type_traits>
#include <variant>

namespace Horo::Animation::CookedInternal {
    /** @brief Declares explicit field order for each supported portable semantic value. */
    template <typename T> struct Fields;

    /** @brief Shared cold-operation byte/allocation/work admission, checked before every bounded operation. */
    class Policy final {
    public:
        explicit Policy(const AnimationCookedContext &context) : context(context) {}

        bool Value() {
            if (!Good())
                return false;
            if (context.cancellation.IsCancellationRequested())
                return Reject(AnimationAssetErrors::Cancelled);
            if (!context.accepting)
                return Reject(AnimationAssetErrors::Unavailable);
            if (work == context.limits.values)
                return Reject(AnimationAssetErrors::Capacity);
            ++work;
            return true;
        }

        bool Allocate(std::uint64_t bytes) {
            if (bytes > context.limits.ownedBytes - owned)
                return Reject(AnimationAssetErrors::Capacity);
            owned += bytes;
            return true;
        }

        template <typename T> bool Elements(std::uint64_t count) {
            using Limit = TableLimit<T>;
            if (count > Limit::single || count > Limit::total - tables[Limit::index])
                return Reject(AnimationAssetErrors::Capacity);
            tables[Limit::index] += count;
            return true;
        }

        bool Reject(const ErrorCodeDescriptor &error) {
            if (!failure)
                failure = &error;
            return false;
        }

        bool Good() const noexcept {
            return failure == nullptr;
        }

        const AnimationCookedContext &context;
        const ErrorCodeDescriptor *failure{};
        std::uint64_t work{};
        std::uint64_t owned{};
        std::array<std::uint64_t, TableBudgetCount> tables{};
    };

    /** @brief Writes each integer explicitly, never a native object's memory representation. */
    class Writer final {
    public:
        explicit Writer(const AnimationCookedContext &context) : policy(context) {}

        template <std::integral T>
            requires(!std::same_as<T, bool>)
        void Scalar(T value) {
            if (!policy.Value() || !Reserve(sizeof(T)))
                return;
            using Bits = std::make_unsigned_t<T>;
            const auto bits = std::bit_cast<Bits>(value);
            for (std::size_t index = 0; index < sizeof(T); ++index)
                bytes.push_back(static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xffU));
        }

        void Raw(std::span<const std::uint8_t> value) {
            if (policy.Value() && Reserve(value.size()))
                bytes.insert(bytes.end(), value.begin(), value.end());
        }

        bool Reserve(std::size_t count) {
            if (count > policy.context.limits.bytes - bytes.size())
                return policy.Reject(AnimationAssetErrors::Capacity);
            const auto required = bytes.size() + count;
            if (required <= bytes.capacity())
                return true;
            const auto grown =
                std::min<std::uint64_t>(policy.context.limits.bytes, std::max<std::uint64_t>(required, bytes.capacity() * 2ULL));
            if (bytes.capacity() > policy.context.limits.ownedBytes - policy.owned)
                return policy.Reject(AnimationAssetErrors::Capacity);
            if (grown > policy.context.limits.ownedBytes - policy.owned - bytes.capacity())
                return policy.Reject(AnimationAssetErrors::Capacity);
            bytes.reserve(static_cast<std::size_t>(grown));
            return true;
        }

        Policy policy;
        std::vector<std::uint8_t> bytes;
    };

    /** @brief Reads only admitted bytes and charges logical owned tables before their allocation. */
    class Reader final {
    public:
        Reader(std::span<const std::uint8_t> bytes, const AnimationCookedContext &context) : policy(context), bytes(bytes) {}

        template <std::integral T>
            requires(!std::same_as<T, bool>)
        T Scalar() {
            if (!policy.Value() || !Available(sizeof(T)))
                return {};
            using Bits = std::make_unsigned_t<T>;
            Bits bits{};
            for (std::size_t index = 0; index < sizeof(T); ++index)
                bits |= static_cast<Bits>(static_cast<Bits>(bytes[position++]) << (index * 8U));
            return std::bit_cast<T>(bits);
        }

        std::span<const std::uint8_t> Raw(std::size_t count) {
            if (!policy.Value() || !Available(count))
                return {};
            const auto result = bytes.subspan(position, count);
            position += count;
            return result;
        }

        bool Available(std::size_t count) {
            if (count > bytes.size() - position)
                return policy.Reject(AnimationAssetErrors::Malformed);
            return true;
        }

        bool Table(std::uint32_t count, std::size_t elementBytes) {
            if (!policy.Good() || count > policy.context.limits.values - policy.work)
                return policy.Reject(AnimationAssetErrors::Capacity);
            if (!Available(count))
                return false;
            if (elementBytes != 0 && count > policy.context.limits.ownedBytes / elementBytes)
                return policy.Reject(AnimationAssetErrors::Capacity);
            return policy.Allocate(static_cast<std::uint64_t>(count) * elementBytes);
        }

        Policy policy;
        std::span<const std::uint8_t> bytes;
        std::size_t position{};
    };

    template <typename T> struct IsVector : std::false_type {};

    template <typename T, typename Allocator> struct IsVector<std::vector<T, Allocator>> : std::true_type {};

    template <typename T> struct IsSpan : std::false_type {};

    template <typename T, std::size_t N> struct IsSpan<std::span<T, N>> : std::true_type {};

    template <typename T> struct IsOptional : std::false_type {};

    template <typename T> struct IsOptional<std::optional<T>> : std::true_type {};

    template <typename T> struct IsVariant : std::false_type {};

    template <typename... T> struct IsVariant<std::variant<T...>> : std::true_type {};

    template <typename T> struct IsArray : std::false_type {};

    template <typename T, std::size_t N> struct IsArray<std::array<T, N>> : std::true_type {};

    /** @brief Primitive numeric wire path, shared only as scalar representation, never semantic validation. */
    template <typename T>
        requires(std::integral<T> && !std::same_as<T, bool>)
    void Transfer(Writer &wire, const T &value) {
        wire.Scalar(value);
    }

    template <typename T>
        requires(std::integral<T> && !std::same_as<T, bool>)
    void Transfer(Reader &wire, T &value) {
        value = wire.Scalar<T>();
    }

    inline void Transfer(Writer &wire, const bool &value) {
        wire.Scalar<std::uint8_t>(value ? 1 : 0);
    }

    inline void Transfer(Reader &wire, bool &value) {
        const auto encoded = wire.Scalar<std::uint8_t>();
        if (encoded > 1)
            wire.policy.Reject(AnimationAssetErrors::Malformed);
        value = encoded != 0;
    }

    template <std::floating_point T> void Transfer(Writer &wire, const T &value) {
        if (!std::isfinite(value)) {
            wire.policy.Reject(AnimationAssetErrors::Malformed);
            return;
        }
        using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
        wire.Scalar(std::bit_cast<Bits>(value == 0 ? T{} : value));
    }

    template <std::floating_point T> void Transfer(Reader &wire, T &value) {
        using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
        value = std::bit_cast<T>(wire.Scalar<Bits>());
        if (!std::isfinite(value) || (value == 0 && std::signbit(value)))
            wire.policy.Reject(AnimationAssetErrors::Malformed);
    }

    /** @brief Closes the exact compiled enum vocabulary before semantic asset validation. */
    template <typename T> constexpr bool KnownEnum(T value) {
        if constexpr (requires { T::Count; })
            return value >= static_cast<T>(0) && value < T::Count;
        else
            return false;
    }

    template <> constexpr bool KnownEnum(GraphValueType value) {
        return value < GraphValueType::Unspecified;
    }

    template <> constexpr bool KnownEnum(GraphStatePredicate value) {
        return value <= GraphStatePredicate::Trigger;
    }

    template <> constexpr bool KnownEnum(GraphStateInterruption value) {
        return value <= GraphStateInterruption::Either;
    }

    template <> constexpr bool KnownEnum(GraphStateBlendCurve value) {
        return value <= GraphStateBlendCurve::EaseOut;
    }

    template <typename T>
        requires std::is_enum_v<T>
    void Transfer(Writer &wire, const T &value) {
        if (!KnownEnum(value))
            wire.policy.Reject(AnimationAssetErrors::Malformed);
        else
            wire.Scalar(static_cast<std::underlying_type_t<T>>(value));
    }

    template <typename T>
        requires std::is_enum_v<T>
    void Transfer(Reader &wire, T &value) {
        value = static_cast<T>(wire.Scalar<std::underlying_type_t<T>>());
        if (!KnownEnum(value))
            wire.policy.Reject(AnimationAssetErrors::Malformed);
    }

    inline void Transfer(Writer &wire, const Assets::AssetId &value) {
        wire.Raw(value.Bytes());
    }

    inline void Transfer(Reader &wire, Assets::AssetId &value) {
        const auto bytes = wire.Raw(16);
        if (!wire.policy.Good())
            return;
        std::array<std::uint8_t, 16> copied{};
        std::ranges::copy(bytes, copied.begin());
        value = Assets::AssetId::FromBytes(copied);
    }

    template <typename T>
    concept PersistentIdentity = requires(const T &value) {
        value.Asset();
        T::Create(value.Asset());
    };

    template <PersistentIdentity T> void Transfer(Writer &wire, const T &value) {
        Transfer(wire, value.Asset());
    }

    template <PersistentIdentity T> void Transfer(Reader &wire, T &value) {
        Assets::AssetId asset;
        Transfer(wire, asset);
        if (!asset.IsValid()) {
            value = {};
            return;
        }
        auto parsed = T::Create(asset);
        if (parsed.HasError())
            wire.policy.Reject(AnimationAssetErrors::Malformed);
        else
            value = parsed.Value();
    }

    template <typename T>
    concept StableIdentity = requires(const T &value) {
        value.Value();
        T::Create(std::uint64_t{});
    };

    template <StableIdentity T> void Transfer(Writer &wire, const T &value) {
        wire.Scalar<std::uint64_t>(value.Value());
    }

    template <StableIdentity T> void Transfer(Reader &wire, T &value) {
        const auto encoded = wire.Scalar<std::uint64_t>();
        if (encoded == 0) {
            value = {};
            return;
        }
        auto parsed = T::Create(encoded);
        if (parsed.HasError())
            wire.policy.Reject(AnimationAssetErrors::Malformed);
        else
            value = parsed.Value();
    }

    /** @brief Bounded NFC UTF-8 scalar handling is implemented by this domain codec's pinned Unicode contribution. */
    void Transfer(Writer &wire, const std::string &value);
    void Transfer(Reader &wire, std::string &value);

    void Transfer(Writer &wire, const Assets::AssetTypeId &value);
    void Transfer(Reader &wire, Assets::AssetTypeId &value);

    template <typename T> void Transfer(Reader &wire, T &value);
    template <typename T> void Transfer(Writer &wire, const T &value);

    /** @brief Dispatches only one declared closed variant arm; unknown tags never alias another category. */
    template <std::size_t I = 0, typename... T> void ReadVariant(Reader &wire, std::variant<T...> &value, std::uint32_t index) {
        if (index == I) {
            value.template emplace<I>();
            Transfer(wire, std::get<I>(value));
        } else if constexpr (I + 1 < sizeof...(T))
            ReadVariant<I + 1>(wire, value, index);
        else
            wire.policy.Reject(AnimationAssetErrors::Malformed);
    }

    template <typename T> void WriteAggregate(Writer &wire, const T &value);
    template <typename T> void ReadAggregate(Reader &wire, T &value);

    /** @brief Explicit vector/optional/variant/field composition; all counts are charged before allocation. */
    template <typename T> void WriteComposite(Writer &wire, const T &value) {
        if constexpr (IsVector<T>::value || IsSpan<T>::value) {
            if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
                wire.policy.Reject(AnimationAssetErrors::Capacity);
                return;
            }
            if (!wire.policy.template Elements<typename T::value_type>(value.size()))
                return;
            wire.Scalar(static_cast<std::uint32_t>(value.size()));
            for (const auto &element : value) {
                Transfer(wire, element);
                if (!wire.policy.Good())
                    return;
            }
        } else if constexpr (IsOptional<T>::value) {
            Transfer(wire, value.has_value());
            if (value)
                Transfer(wire, *value);
        } else
            WriteAggregate(wire, value);
    }

    template <typename T> void WriteAggregate(Writer &wire, const T &value) {
        if constexpr (IsVariant<T>::value) {
            wire.Scalar(static_cast<std::uint32_t>(value.index()));
            std::visit([&wire](const auto &element) {
                Transfer(wire, element);
            }, value);
        } else if constexpr (IsArray<T>::value) {
            for (const auto &element : value) {
                Transfer(wire, element);
                if (!wire.policy.Good())
                    return;
            }
        } else
            std::apply([&wire](const auto &...field) {
                (Transfer(wire, field), ...);
            }, Fields<T>::Get(value));
    }

    template <typename T> void ReadComposite(Reader &wire, T &value) {
        if constexpr (IsVector<T>::value) {
            const auto count = wire.Scalar<std::uint32_t>();
            if (!wire.policy.template Elements<typename T::value_type>(count) || !wire.Table(count, sizeof(typename T::value_type)))
                return;
            value.resize(count);
            for (auto &element : value) {
                Transfer(wire, element);
                if (!wire.policy.Good())
                    return;
            }
        } else if constexpr (IsOptional<T>::value) {
            bool present{};
            Transfer(wire, present);
            if (present) {
                value.emplace();
                Transfer(wire, *value);
            } else
                value.reset();
        } else
            ReadAggregate(wire, value);
    }

    template <typename T> void ReadAggregate(Reader &wire, T &value) {
        if constexpr (IsVariant<T>::value)
            ReadVariant(wire, value, wire.Scalar<std::uint32_t>());
        else if constexpr (IsArray<T>::value) {
            for (auto &element : value)
                Transfer(wire, element);
        } else
            std::apply([&wire](auto &...field) {
                (Transfer(wire, field), ...);
            }, Fields<T>::Get(value));
    }

    template <typename T> void Transfer(Reader &wire, T &value) {
        ReadComposite(wire, value);
    }

    template <typename T> void Transfer(Writer &wire, const T &value) {
        WriteComposite(wire, value);
    }
}  // namespace Horo::Animation::CookedInternal
