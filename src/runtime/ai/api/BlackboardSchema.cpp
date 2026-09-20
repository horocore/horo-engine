#include "Horo/AI/BlackboardSchema.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::AI {
    namespace {
        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnown(const BlackboardValueKind value) noexcept {
            using enum BlackboardValueKind;
            switch (value) {
                case Boolean:
                case SignedInteger:
                case Scalar:
                case EntityReference:
                case AssetReference:
                case WorldCoordinate:
                    return true;
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const BlackboardValueCardinality value) noexcept {
            using enum BlackboardValueCardinality;
            switch (value) {
                case Scalar:
                case Collection:
                    return true;
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const BlackboardKeyPresence value) noexcept {
            using enum BlackboardKeyPresence;
            switch (value) {
                case Required:
                case Optional:
                    return true;
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const BlackboardKeyAccess value) noexcept {
            using enum BlackboardKeyAccess;
            switch (value) {
                case ReadOnly:
                case ReadWrite:
                    return true;
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const BlackboardUnknownValuePolicy value) noexcept {
            using enum BlackboardUnknownValuePolicy;
            switch (value) {
                case Reject:
                case PreserveOpaque:
                    return true;
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] BlackboardValueKind KindOf(const BlackboardScalarValue &value) noexcept {
            using enum BlackboardValueKind;
            return std::visit([]<typename Value>(const Value &) {
                using T = std::remove_cvref_t<Value>;
                if constexpr (std::is_same_v<T, bool>)
                    return Boolean;
                if constexpr (std::is_same_v<T, std::int64_t>)
                    return SignedInteger;
                if constexpr (std::is_same_v<T, double>)
                    return Scalar;
                if constexpr (std::is_same_v<T, BlackboardStoredEntityReference>)
                    return EntityReference;
                if constexpr (std::is_same_v<T, BlackboardStoredAssetReference>)
                    return AssetReference;
                return WorldCoordinate;
            }, value);
        }

        [[nodiscard]] Result<void> ValidateScalar(const BlackboardScalarValue &value, const BlackboardValueKind expectedKind) {
            if (KindOf(value) != expectedKind)
                return Failure(AIErrors::BlackboardValueTypeMismatch);
            if (const auto *number = std::get_if<double>(&value); number != nullptr && !std::isfinite(*number))
                return Failure(AIErrors::BlackboardValueInvalid);
            if (const auto *entity = std::get_if<BlackboardStoredEntityReference>(&value); entity != nullptr && !entity->IsValid())
                return Failure(AIErrors::BlackboardValueInvalid);
            if (const auto *asset = std::get_if<BlackboardStoredAssetReference>(&value); asset != nullptr && !asset->IsValid())
                return Failure(AIErrors::BlackboardValueInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateKnownValue(const BlackboardValue &value, const BlackboardKeyDescriptor &key) {
            if (key.cardinality == BlackboardValueCardinality::Scalar) {
                const auto *scalar = std::get_if<BlackboardScalarValue>(&value);
                if (scalar == nullptr)
                    return Failure(AIErrors::BlackboardValueTypeMismatch);
                return ValidateScalar(*scalar, key.kind);
            }
            const auto *collection = std::get_if<BlackboardCollectionValue>(&value);
            if (collection == nullptr || collection->elementKind != key.kind)
                return Failure(AIErrors::BlackboardValueTypeMismatch);
            if (collection->count > key.maximumCollectionElements || collection->count > MaximumBlackboardCollectionElements)
                return Failure(AIErrors::BlackboardLimitExceeded);
            for (std::size_t index = 0; index < collection->count; ++index) {
                if (const auto element = ValidateScalar(collection->elements[index], key.kind); element.HasError())
                    return element;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool HasValidKeyMetadata(const BlackboardKeyDescriptor &key) noexcept {
            return key.key.IsValid() && IsKnown(key.kind) && IsKnown(key.cardinality) && IsKnown(key.presence) && IsKnown(key.access);
        }

        [[nodiscard]] bool HasValidCardinality(const BlackboardKeyDescriptor &key) noexcept {
            if (key.cardinality == BlackboardValueCardinality::Scalar)
                return key.maximumCollectionElements == 1;
            return key.maximumCollectionElements != 0 && key.maximumCollectionElements <= MaximumBlackboardCollectionElements;
        }

        [[nodiscard]] Result<void> ValidateKey(const BlackboardKeyDescriptor &key) {
            if (!HasValidKeyMetadata(key) || !HasValidCardinality(key))
                return Failure(AIErrors::BlackboardSchemaInvalid);
            if (key.defaultValue.has_value())
                return ValidateKnownValue(*key.defaultValue, key);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc BlackboardStoredAssetReference::IsValid */
    bool BlackboardStoredAssetReference::IsValid() const noexcept {
        return std::ranges::any_of(canonicalBytes, [](const std::uint8_t byte) {
            return byte != 0;
        });
    }

    /** @copydoc BlackboardCollectionValue::operator== */
    bool BlackboardCollectionValue::operator==(const BlackboardCollectionValue &other) const noexcept {
        return elementKind == other.elementKind && count == other.count && count <= MaximumBlackboardCollectionElements &&
               std::equal(elements.begin(), elements.begin() + static_cast<std::ptrdiff_t>(count), other.elements.begin());
    }

    /** @copydoc BlackboardOpaqueValue::operator== */
    bool BlackboardOpaqueValue::operator==(const BlackboardOpaqueValue &other) const noexcept {
        return schemaVersion == other.schemaVersion && serializedType == other.serializedType && size == other.size &&
               size <= MaximumBlackboardOpaqueBytes &&
               std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size), other.bytes.begin());
    }

    /** @copydoc SerializeBlackboardEntityReference */
    Result<SerializedBlackboardEntityReference> SerializeBlackboardEntityReference(const BlackboardStoredEntityReference &reference) {
        if (!reference.IsValid())
            return Result<SerializedBlackboardEntityReference>::Failure(MakeError(AIErrors::BlackboardValueInvalid));
        SerializedBlackboardEntityReference bytes{};
        const auto encode = [&bytes](const std::uint64_t value, const std::size_t offset, const std::size_t width) {
            for (std::size_t index = 0; index < width; ++index) {
                const std::size_t shift = (width - index - 1) * 8;
                bytes[offset + index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
            }
        };
        encode(reference.sceneIncarnation, 0, 8);
        encode(reference.slot, 8, 4);
        encode(reference.generation, 12, 4);
        return Result<SerializedBlackboardEntityReference>::Success(bytes);
    }

    /** @copydoc DeserializeBlackboardEntityReference */
    Result<BlackboardStoredEntityReference> DeserializeBlackboardEntityReference(const SerializedBlackboardEntityReference &bytes) {
        const auto decode = [&bytes](const std::size_t offset, const std::size_t width) {
            std::uint64_t value{};
            for (std::size_t index = 0; index < width; ++index)
                value = value * 256U + bytes[offset + index];
            return value;
        };
        const BlackboardStoredEntityReference reference{
            decode(0, 8),
            static_cast<std::uint32_t>(decode(8, 4)),
            static_cast<std::uint32_t>(decode(12, 4)),
        };
        if (!reference.IsValid())
            return Result<BlackboardStoredEntityReference>::Failure(MakeError(AIErrors::BlackboardValueInvalid));
        return Result<BlackboardStoredEntityReference>::Success(reference);
    }

    /** @copydoc ValidateBlackboardValue */
    Result<void> ValidateBlackboardValue(const BlackboardValue &value, const BlackboardKeyDescriptor *key,
                                         const BlackboardUnknownValuePolicy unknownPolicy) {
        if (key != nullptr)
            return ValidateKnownValue(value, *key);
        if (!IsKnown(unknownPolicy))
            return Failure(AIErrors::BlackboardSchemaInvalid);
        if (unknownPolicy == BlackboardUnknownValuePolicy::Reject)
            return Failure(AIErrors::BlackboardUnknownValueRejected);
        const auto *opaque = std::get_if<BlackboardOpaqueValue>(&value);
        if (opaque == nullptr)
            return Failure(AIErrors::BlackboardValueTypeMismatch);
        if (opaque->schemaVersion == 0 || opaque->serializedType == 0 || opaque->size > MaximumBlackboardOpaqueBytes)
            return Failure(AIErrors::BlackboardValueInvalid);
        return Result<void>::Success();
    }

    /** @copydoc BlackboardSchema::Capture */
    Result<BlackboardSchema> BlackboardSchema::Capture(const BlackboardSchemaDescriptor &descriptor) {
        if (!descriptor.identity.IsValid() || descriptor.version == 0 || !IsKnown(descriptor.unknownValuePolicy) || descriptor.keys.empty())
            return Result<BlackboardSchema>::Failure(MakeError(AIErrors::BlackboardSchemaInvalid));
        if (descriptor.keys.size() > MaximumBlackboardKeys)
            return Result<BlackboardSchema>::Failure(MakeError(AIErrors::BlackboardLimitExceeded));

        auto keys = std::unique_ptr<std::array<BlackboardKeyDescriptor, MaximumBlackboardKeys>>{
            new (std::nothrow) std::array<BlackboardKeyDescriptor, MaximumBlackboardKeys>{}};
        if (keys == nullptr)
            return Result<BlackboardSchema>::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        std::ranges::copy(descriptor.keys, keys->begin());
        for (std::size_t index = 0; index < descriptor.keys.size(); ++index) {
            if (const auto key = ValidateKey((*keys)[index]); key.HasError())
                return Result<BlackboardSchema>::Failure(key.ErrorValue());
        }
        std::sort(keys->begin(), keys->begin() + static_cast<std::ptrdiff_t>(descriptor.keys.size()),
                  [](const BlackboardKeyDescriptor &left, const BlackboardKeyDescriptor &right) {
            return left.key.Value() < right.key.Value();
        });
        if (const auto end = keys->begin() + static_cast<std::ptrdiff_t>(descriptor.keys.size());
            std::adjacent_find(keys->begin(), end, [](const BlackboardKeyDescriptor &left, const BlackboardKeyDescriptor &right) {
            return left.key == right.key;
        }) != end)
            return Result<BlackboardSchema>::Failure(MakeError(AIErrors::DescriptorConflict));
        return Result<BlackboardSchema>::Success(BlackboardSchema{descriptor.identity, descriptor.version, descriptor.unknownValuePolicy,
                                                                  std::move(keys), descriptor.keys.size()});
    }

    /** @copydoc BlackboardSchema::Keys */
    std::span<const BlackboardKeyDescriptor> BlackboardSchema::Keys() const noexcept {
        if (keys_ == nullptr)
            return {};
        return {keys_->data(), keyCount_};
    }

    /** @brief Initializes an already validated immutable schema value. */
    BlackboardSchema::BlackboardSchema(const BlackboardSchemaId identity, const std::uint32_t version,
                                       const BlackboardUnknownValuePolicy policy,
                                       std::unique_ptr<std::array<BlackboardKeyDescriptor, MaximumBlackboardKeys>> keys,
                                       const std::size_t keyCount) noexcept
        : identity_(identity), version_(version), unknownValuePolicy_(policy), keys_(std::move(keys)), keyCount_(keyCount) {}
}  // namespace Horo::AI
