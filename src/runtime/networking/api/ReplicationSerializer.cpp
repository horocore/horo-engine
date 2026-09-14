#include "Horo/Network/ReplicationSerializer.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <string_view>
#include <tuple>
#include <utility>

namespace Horo::Network {
    namespace {
        constexpr std::size_t ScalarBytes = sizeof(std::uint64_t);

        template <typename T> [[nodiscard]] Result<T> Fail(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        [[nodiscard]] constexpr bool IsOwnerSeparator(const unsigned char value) noexcept {
            return value == '.' || value == '-' || value == '_';
        }

        [[nodiscard]] constexpr bool IsLowercaseAlphanumeric(const unsigned char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
        }

        [[nodiscard]] bool IsCanonicalOwner(const std::string_view owner) noexcept {
            if (owner.empty())
                return false;
            bool previousSeparator = true;
            for (const unsigned char value : owner) {
                if (IsOwnerSeparator(value)) {
                    if (previousSeparator)
                        return false;
                    previousSeparator = true;
                } else {
                    if (!IsLowercaseAlphanumeric(value))
                        return false;
                    previousSeparator = false;
                }
            }
            return !previousSeparator;
        }

        [[nodiscard]] auto EntryKey(const ReplicationSerializerDescriptor &descriptor) noexcept {
            return std::tuple{descriptor.owner.value, descriptor.valueType.Value(), descriptor.codec.Value()};
        }

        [[nodiscard]] ReplicationValueKind KindOf(const ReplicationRuntimeValue &value) noexcept {
            if (std::holds_alternative<bool>(value))
                return ReplicationValueKind::Boolean;
            if (std::holds_alternative<std::int64_t>(value))
                return ReplicationValueKind::SignedInteger;
            if (std::holds_alternative<std::uint64_t>(value))
                return ReplicationValueKind::UnsignedInteger;
            if (std::holds_alternative<double>(value))
                return ReplicationValueKind::FloatingPoint;
            if (std::holds_alternative<std::string>(value))
                return ReplicationValueKind::Utf8Text;
            return ReplicationValueKind::ByteSequence;
        }

        [[nodiscard]] std::size_t ElementCount(const ReplicationRuntimeValue &value) noexcept {
            if (const auto *text = std::get_if<std::string>(&value); text != nullptr)
                return text->size();
            if (const auto *bytes = std::get_if<std::vector<std::byte>>(&value); bytes != nullptr)
                return bytes->size();
            return 1;
        }

        [[nodiscard]] bool ValidQuantization(const ReplicationSerializerDescriptor &descriptor) noexcept {
            using enum ReplicationQuantizationMode;
            if (descriptor.quantization.mode == Exact)
                return descriptor.quantization.step == 0.0;
            return descriptor.quantization.mode == NearestStep && descriptor.valueKind == ReplicationValueKind::FloatingPoint &&
                   std::isfinite(descriptor.quantization.step) && descriptor.quantization.step > 0.0;
        }

        [[nodiscard]] bool ValidDescriptor(const ReplicationSerializerDescriptor &descriptor,
                                           const ReplicationSerializerRegistryLimits &limits) noexcept {
            return descriptor.valueType.IsValid() && descriptor.codec.IsValid() && IsCanonicalOwner(descriptor.owner.value) &&
                   descriptor.valueKind < ReplicationValueKind::Count && ValidQuantization(descriptor) &&
                   descriptor.maximumEncodedBytes > 0 && descriptor.maximumEncodedBytes <= limits.maximumEncodedBytes &&
                   descriptor.maximumElementCount > 0 && descriptor.maximumElementCount <= limits.maximumElementCount;
        }

        [[nodiscard]] const ReplicationFieldDescriptor *FindField(const ReplicationSchemaDescriptor &schema, const FieldId field) noexcept {
            const auto found = std::ranges::lower_bound(schema.fields, field, {}, &ReplicationFieldDescriptor::id);
            return found != schema.fields.end() && found->id == field ? std::to_address(found) : nullptr;
        }

        [[nodiscard]] auto AdapterKey(const ModuleId &owner, const ReplicationFieldDescriptor &field) {
            return std::tuple{owner.value, field.valueType.Value(), field.codec.Value()};
        }

        [[nodiscard]] bool FieldFitsAdapter(const ReplicationFieldDescriptor &field,
                                            const ReplicationSerializerDescriptor &adapter) noexcept {
            return field.limits.maximumEncodedBytes <= adapter.maximumEncodedBytes &&
                   field.limits.maximumElementCount <= adapter.maximumElementCount;
        }

        void WriteU64(std::vector<std::byte> &bytes, const std::uint64_t value) {
            for (int shift = 56; shift >= 0; shift -= 8)
                bytes.push_back(static_cast<std::byte>(value >> static_cast<unsigned>(shift)));
        }

        [[nodiscard]] std::uint64_t ReadU64(const std::span<const std::byte> bytes) noexcept {
            std::uint64_t value{};
            for (const std::byte byte : bytes)
                value = (value << 8U) | std::to_integer<std::uint8_t>(byte);
            return value;
        }

        [[nodiscard]] Result<std::int64_t> Quantize(const double value, const double step) {
            if (!std::isfinite(value))
                return Fail<std::int64_t>(NetworkErrors::ReplicationSerializerValueInvalid);
            const double scaled = value / step;
            constexpr double Minimum = -9223372036854775808.0;
            constexpr double MaximumExclusive = 9223372036854775808.0;
            if (!std::isfinite(scaled) || scaled < Minimum || scaled >= MaximumExclusive)
                return Fail<std::int64_t>(NetworkErrors::ReplicationSerializerCapacityExceeded);
            return Result<std::int64_t>::Success(static_cast<std::int64_t>(std::round(scaled)));
        }

        [[nodiscard]] Result<void> EncodeFloating(std::vector<std::byte> &bytes, const ReplicationSerializerDescriptor &descriptor,
                                                  const double value) {
            if (!std::isfinite(value))
                return Fail<void>(NetworkErrors::ReplicationSerializerValueInvalid);
            if (descriptor.quantization.mode == ReplicationQuantizationMode::Exact) {
                WriteU64(bytes, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
                return Result<void>::Success();
            }
            const Result<std::int64_t> quantized = Quantize(value, descriptor.quantization.step);
            if (quantized.HasError())
                return Result<void>::Failure(quantized.ErrorValue());
            WriteU64(bytes, std::bit_cast<std::uint64_t>(quantized.Value()));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<std::byte>> EncodeScalar(const ReplicationSerializerDescriptor &descriptor,
                                                                  const ReplicationRuntimeValue &value) {
            std::vector<std::byte> bytes;
            bytes.reserve(ScalarBytes);
            switch (descriptor.valueKind) {
                case ReplicationValueKind::Boolean:
                    bytes.push_back(std::get<bool>(value) ? std::byte{1} : std::byte{0});
                    break;
                case ReplicationValueKind::SignedInteger:
                    WriteU64(bytes, std::bit_cast<std::uint64_t>(std::get<std::int64_t>(value)));
                    break;
                case ReplicationValueKind::UnsignedInteger:
                    WriteU64(bytes, std::get<std::uint64_t>(value));
                    break;
                case ReplicationValueKind::FloatingPoint: {
                    const Result<void> encoded = EncodeFloating(bytes, descriptor, std::get<double>(value));
                    if (encoded.HasError())
                        return Result<std::vector<std::byte>>::Failure(encoded.ErrorValue());
                    break;
                }
                case ReplicationValueKind::Utf8Text:
                case ReplicationValueKind::ByteSequence:
                case ReplicationValueKind::Count:
                    return Fail<std::vector<std::byte>>(NetworkErrors::ReplicationSerializerInvalid);
            }
            return Result<std::vector<std::byte>>::Success(std::move(bytes));
        }

        [[nodiscard]] Result<ReplicationRuntimeValue> DecodeFloating(const ReplicationSerializerDescriptor &descriptor,
                                                                     const std::uint64_t bits) {
            if (descriptor.quantization.mode == ReplicationQuantizationMode::NearestStep) {
                const double value = static_cast<double>(std::bit_cast<std::int64_t>(bits)) * descriptor.quantization.step;
                return std::isfinite(value) ? Result<ReplicationRuntimeValue>::Success(value)
                                            : Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerValueInvalid);
            }
            const double value = std::bit_cast<double>(bits);
            if (!std::isfinite(value) || (value == 0.0 && bits != 0))
                return Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerValueInvalid);
            return Result<ReplicationRuntimeValue>::Success(value);
        }

        [[nodiscard]] Result<ReplicationRuntimeValue> DecodeScalar(const ReplicationSerializerDescriptor &descriptor,
                                                                   const std::span<const std::byte> canonicalBytes) {
            const std::uint64_t bits = ReadU64(canonicalBytes);
            switch (descriptor.valueKind) {
                case ReplicationValueKind::Boolean:
                    if (bits > 1)
                        return Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerValueInvalid);
                    return Result<ReplicationRuntimeValue>::Success(bits != 0);
                case ReplicationValueKind::SignedInteger:
                    return Result<ReplicationRuntimeValue>::Success(std::bit_cast<std::int64_t>(bits));
                case ReplicationValueKind::UnsignedInteger:
                    return Result<ReplicationRuntimeValue>::Success(bits);
                case ReplicationValueKind::FloatingPoint:
                    return DecodeFloating(descriptor, bits);
                case ReplicationValueKind::Utf8Text:
                case ReplicationValueKind::ByteSequence:
                case ReplicationValueKind::Count:
                    return Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerInvalid);
            }
            return Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerInvalid);
        }
    }  // namespace

    /** @brief Resolves one serializer by exact owner, semantic type, and codec identity. */
    const ReplicationSerializerRegistry::Entry *ReplicationSerializerRegistry::FindEntry(const std::span<const Entry> entries,
                                                                                         const ModuleId &owner,
                                                                                         const ReplicationFieldDescriptor &field) noexcept {
        const auto key = AdapterKey(owner, field);
        const auto found = std::ranges::lower_bound(entries, key, {}, [](const Entry &entry) {
            return EntryKey(entry.descriptor);
        });
        return found != entries.end() && EntryKey(found->descriptor) == key ? std::to_address(found) : nullptr;
    }

    /** @brief Proves an optional default is a valid canonical value for its exact adapter. */
    Result<void> ReplicationSerializerRegistry::ValidateDefault(const ReplicationFieldDescriptor &field, const Entry &entry) {
        if (!field.canonicalDefault.has_value())
            return Result<void>::Success();
        const auto &bytes = field.canonicalDefault->canonicalBytes;
        Result<ReplicationRuntimeValue> decoded = entry.serializer->Decode(bytes);
        if (decoded.HasError() || KindOf(decoded.Value()) != entry.descriptor.valueKind ||
            ElementCount(decoded.Value()) > field.limits.maximumElementCount)
            return Fail<void>(NetworkErrors::ReplicationSerializerValueInvalid);
        Result<std::vector<std::byte>> encoded = entry.serializer->Encode(decoded.Value());
        if (encoded.HasError() || encoded.Value() != bytes)
            return Fail<void>(NetworkErrors::ReplicationSerializerValueInvalid);
        return Result<void>::Success();
    }

    /** @brief Verifies complete exact schema coverage and rejects unbound contributions. */
    Result<void> ReplicationSerializerRegistry::ValidateBindings(const ReplicationDescriptorSnapshotPtr &schemas,
                                                                 const std::span<const Entry> entries) {
        std::vector<bool> used(entries.size(), false);
        for (const ReplicationSchemaDescriptor &schema : schemas->Schemas()) {
            for (const ReplicationFieldDescriptor &field : schema.fields) {
                const Entry *entry = FindEntry(entries, schema.owner, field);
                if (entry == nullptr)
                    return Fail<void>(NetworkErrors::ReplicationSerializerUnknown);
                if (!FieldFitsAdapter(field, entry->descriptor))
                    return Fail<void>(NetworkErrors::ReplicationSerializerCapacityExceeded);
                if (const Result<void> validDefault = ValidateDefault(field, *entry); validDefault.HasError())
                    return validDefault;
                used[static_cast<std::size_t>(entry - entries.data())] = true;
            }
        }
        return std::ranges::all_of(used,
                                   [](const bool value) {
            return value;
        })
                   ? Result<void>::Success()
                   : Fail<void>(NetworkErrors::ReplicationSerializerInvalid);
    }

    /** @brief Validates, copies, sorts, and de-duplicates adapter contributions. */
    Result<std::vector<ReplicationSerializerRegistry::Entry>> ReplicationSerializerRegistry::BuildEntries(
        const std::span<const std::shared_ptr<const IReplicationFieldSerializer>> serializers,
        const ReplicationSerializerRegistryLimits &limits) {
        std::vector<Entry> entries;
        entries.reserve(serializers.size());
        for (const auto &serializer : serializers) {
            if (serializer == nullptr)
                return Fail<std::vector<Entry>>(NetworkErrors::ReplicationSerializerInvalid);
            const ReplicationSerializerDescriptor descriptor = serializer->Descriptor();
            if (!ValidDescriptor(descriptor, limits))
                return Fail<std::vector<Entry>>(NetworkErrors::ReplicationSerializerInvalid);
            entries.push_back({descriptor, serializer});
        }
        std::ranges::sort(entries, {}, [](const Entry &entry) {
            return EntryKey(entry.descriptor);
        });
        if (std::ranges::adjacent_find(entries, {}, [](const Entry &entry) {
            return EntryKey(entry.descriptor);
        }) != entries.end())
            return Fail<std::vector<Entry>>(NetworkErrors::ReplicationSerializerConflict);
        return Result<std::vector<Entry>>::Success(std::move(entries));
    }

    /** @brief Resolves one schema field and its pinned adapter without fallback. */
    Result<ReplicationSerializerRegistry::Binding> ReplicationSerializerRegistry::Resolve(const ReplicationSchemaId schemaId,
                                                                                          const FieldId fieldId) const {
        const auto schema = schemas_->Find(schemaId);
        if (schema.HasError())
            return Result<Binding>::Failure(schema.ErrorValue());
        const ReplicationFieldDescriptor *field = FindField(*schema.Value(), fieldId);
        if (field == nullptr)
            return Fail<Binding>(NetworkErrors::ReplicationSerializerUnknown);
        const Entry *entry = FindEntry(entries_, schema.Value()->owner, *field);
        return entry == nullptr ? Fail<Binding>(NetworkErrors::ReplicationSerializerUnknown)
                                : Result<Binding>::Success({schema.Value(), field, entry});
    }

    /** @copydoc ReplicationSerializerRegistry::Create */
    Result<ReplicationSerializerRegistry> ReplicationSerializerRegistry::Create(
        ReplicationDescriptorSnapshotPtr schemas, const std::span<const std::shared_ptr<const IReplicationFieldSerializer>> serializers,
        const ReplicationSerializerRegistryLimits &limits) {
        if (schemas == nullptr || limits.maximumSerializers == 0 || limits.maximumEncodedBytes == 0 || limits.maximumElementCount == 0)
            return Fail<ReplicationSerializerRegistry>(NetworkErrors::ReplicationSerializerInvalid);
        if (serializers.size() > limits.maximumSerializers)
            return Fail<ReplicationSerializerRegistry>(NetworkErrors::ReplicationSerializerCapacityExceeded);
        try {
            ReplicationSerializerRegistry result;
            result.schemas_ = std::move(schemas);
            Result<std::vector<Entry>> entries = BuildEntries(serializers, limits);
            if (entries.HasError())
                return Result<ReplicationSerializerRegistry>::Failure(entries.ErrorValue());
            result.entries_ = std::move(entries).Value();
            if (const Result<void> bindings = ValidateBindings(result.schemas_, result.entries_); bindings.HasError())
                return Result<ReplicationSerializerRegistry>::Failure(bindings.ErrorValue());
            return Result<ReplicationSerializerRegistry>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Fail<ReplicationSerializerRegistry>(NetworkErrors::ReplicationSerializerCapacityExceeded);
        }
    }

    /** @copydoc ReplicationSerializerRegistry::Encode */
    Result<ReplicationEncodedValue> ReplicationSerializerRegistry::Encode(const ReplicationSchemaId schema, const FieldId field,
                                                                          const ReplicationRuntimeValue &value) const {
        try {
            const Result<Binding> binding = Resolve(schema, field);
            if (binding.HasError())
                return Result<ReplicationEncodedValue>::Failure(binding.ErrorValue());
            if (KindOf(value) != binding.Value().entry->descriptor.valueKind ||
                ElementCount(value) > binding.Value().field->limits.maximumElementCount)
                return Fail<ReplicationEncodedValue>(NetworkErrors::ReplicationSerializerValueInvalid);
            Result<std::vector<std::byte>> encoded = binding.Value().entry->serializer->Encode(value);
            if (encoded.HasError())
                return Result<ReplicationEncodedValue>::Failure(encoded.ErrorValue());
            if (encoded.Value().size() > binding.Value().field->limits.maximumEncodedBytes)
                return Fail<ReplicationEncodedValue>(NetworkErrors::ReplicationSerializerCapacityExceeded);
            return Result<ReplicationEncodedValue>::Success(
                {binding.Value().field->valueType, binding.Value().field->codec, std::move(encoded).Value()});
        } catch (const std::bad_alloc &) {
            return Fail<ReplicationEncodedValue>(NetworkErrors::ReplicationSerializerCapacityExceeded);
        }
    }

    /** @copydoc ReplicationSerializerRegistry::Decode */
    Result<ReplicationRuntimeValue> ReplicationSerializerRegistry::Decode(const ReplicationSchemaId schema, const FieldId field,
                                                                          const ReplicationEncodedValue &value) const {
        try {
            const Result<Binding> binding = Resolve(schema, field);
            if (binding.HasError())
                return Result<ReplicationRuntimeValue>::Failure(binding.ErrorValue());
            if (value.valueType != binding.Value().field->valueType || value.codec != binding.Value().field->codec)
                return Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerIncompatible);
            if (value.canonicalBytes.size() > binding.Value().field->limits.maximumEncodedBytes)
                return Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerCapacityExceeded);
            Result<ReplicationRuntimeValue> decoded = binding.Value().entry->serializer->Decode(value.canonicalBytes);
            if (decoded.HasError())
                return decoded;
            if (KindOf(decoded.Value()) != binding.Value().entry->descriptor.valueKind ||
                ElementCount(decoded.Value()) > binding.Value().field->limits.maximumElementCount)
                return Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerValueInvalid);
            return decoded;
        } catch (const std::bad_alloc &) {
            return Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerCapacityExceeded);
        }
    }

    /** @copydoc ReplicationSerializerRegistry::CanonicallyEqual */
    Result<bool> ReplicationSerializerRegistry::CanonicallyEqual(const ReplicationSchemaId schema, const FieldId field,
                                                                 const ReplicationRuntimeValue &left,
                                                                 const ReplicationRuntimeValue &right) const {
        const Result<ReplicationEncodedValue> encodedLeft = Encode(schema, field, left);
        if (encodedLeft.HasError())
            return Result<bool>::Failure(encodedLeft.ErrorValue());
        const Result<ReplicationEncodedValue> encodedRight = Encode(schema, field, right);
        if (encodedRight.HasError())
            return Result<bool>::Failure(encodedRight.ErrorValue());
        return Result<bool>::Success(encodedLeft.Value() == encodedRight.Value());
    }

    /** @copydoc ReplicationSerializerRegistry::Schemas */
    const ReplicationDescriptorSnapshotPtr &ReplicationSerializerRegistry::Schemas() const noexcept {
        return schemas_;
    }

    /** @copydoc CanonicalScalarReplicationSerializer::Create */
    Result<std::shared_ptr<const CanonicalScalarReplicationSerializer>> CanonicalScalarReplicationSerializer::Create(
        const ReplicationSerializerDescriptor &descriptor) {
        const ReplicationSerializerRegistryLimits limits;
        if (!ValidDescriptor(descriptor, limits) || descriptor.valueKind > ReplicationValueKind::FloatingPoint ||
            descriptor.maximumElementCount != 1 ||
            descriptor.maximumEncodedBytes < (descriptor.valueKind == ReplicationValueKind::Boolean ? 1U : ScalarBytes))
            return Fail<std::shared_ptr<const CanonicalScalarReplicationSerializer>>(NetworkErrors::ReplicationSerializerInvalid);
        try {
            return Result<std::shared_ptr<const CanonicalScalarReplicationSerializer>>::Success(
                std::shared_ptr<const CanonicalScalarReplicationSerializer>{new CanonicalScalarReplicationSerializer(descriptor)});
        } catch (const std::bad_alloc &) {
            return Fail<std::shared_ptr<const CanonicalScalarReplicationSerializer>>(NetworkErrors::ReplicationSerializerCapacityExceeded);
        }
    }

    /** @copydoc CanonicalScalarReplicationSerializer::CanonicalScalarReplicationSerializer */
    CanonicalScalarReplicationSerializer::CanonicalScalarReplicationSerializer(ReplicationSerializerDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}

    /** @copydoc CanonicalScalarReplicationSerializer::Descriptor */
    const ReplicationSerializerDescriptor &CanonicalScalarReplicationSerializer::Descriptor() const noexcept {
        return descriptor_;
    }

    /** @copydoc CanonicalScalarReplicationSerializer::Encode */
    Result<std::vector<std::byte>> CanonicalScalarReplicationSerializer::Encode(const ReplicationRuntimeValue &value) const {
        if (KindOf(value) != descriptor_.valueKind)
            return Fail<std::vector<std::byte>>(NetworkErrors::ReplicationSerializerValueInvalid);
        try {
            return EncodeScalar(descriptor_, value);
        } catch (const std::bad_alloc &) {
            return Fail<std::vector<std::byte>>(NetworkErrors::ReplicationSerializerCapacityExceeded);
        }
    }

    /** @copydoc CanonicalScalarReplicationSerializer::Decode */
    Result<ReplicationRuntimeValue> CanonicalScalarReplicationSerializer::Decode(const std::span<const std::byte> canonicalBytes) const {
        const std::size_t expected = descriptor_.valueKind == ReplicationValueKind::Boolean ? 1U : ScalarBytes;
        if (canonicalBytes.size() != expected)
            return Fail<ReplicationRuntimeValue>(NetworkErrors::ReplicationSerializerValueInvalid);
        return DecodeScalar(descriptor_, canonicalBytes);
    }
}  // namespace Horo::Network
