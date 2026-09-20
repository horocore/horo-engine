#include "Horo/Foundation/Sha256.h"
#include "Horo/Navigation/NavigationDataSerialization.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        constexpr std::array<std::uint8_t, 4> EnvelopeMagic{'H', 'N', 'A', 'V'};
        constexpr std::size_t EnvelopeHeaderBytes = 28;
        constexpr std::size_t AuthoredRecordHeaderBytes = 26;
        constexpr std::size_t GeneratedPayloadHeaderBytes = 18;
        constexpr std::size_t ChecksumBytes = 32;
        constexpr std::uint8_t RequiredRecordFlag = 0x01U;
        constexpr std::uint8_t OpaqueRecordFlag = 0x02U;
        constexpr std::uint8_t QuarantinedGeneratedFlag = 0x01U;
        constexpr std::uint8_t UnsupportedGeneratedVersionFlag = 0x02U;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] constexpr bool IsSupportedEnvelopeVersion(const NavigationSourceSchemaVersion version) noexcept {
            return version.major == CurrentNavigationSourceSchemaVersion.major &&
                   version.minor <= CurrentNavigationSourceSchemaVersion.minor;
        }

        [[nodiscard]] constexpr bool IsValidRecordVersion(const NavigationSourceSchemaVersion version) noexcept {
            return version.major != 0;
        }

        [[nodiscard]] bool IsValidEnvelopeLimit(const NavigationSerializationLimits &limits) noexcept {
            return limits.maximumEnvelopeBytes >= EnvelopeHeaderBytes + ChecksumBytes &&
                   limits.maximumEnvelopeBytes <= NavigationSerializationLimits::MaximumEnvelopeBytes;
        }

        [[nodiscard]] bool IsValidLimits(const NavigationSerializationLimits &limits) noexcept {
            return IsValidEnvelopeLimit(limits) && limits.maximumAuthoredRecords <= NavigationSerializationLimits::MaximumAuthoredRecords &&
                   limits.maximumGeneratedPayloads <= NavigationSerializationLimits::MaximumGeneratedPayloads &&
                   limits.maximumRecordPayloadBytes <= NavigationSerializationLimits::MaximumRecordPayloadBytes &&
                   limits.maximumGeneratedPayloadBytes <= NavigationSerializationLimits::MaximumGeneratedPayloadBytes &&
                   limits.maximumTotalPayloadBytes <= NavigationSerializationLimits::MaximumTotalPayloadBytes;
        }

        [[nodiscard]] bool IsValidSupportTable(const std::span<const NavigationAuthoredRecordSupport> supports) noexcept {
            for (std::size_t index = 0; index < supports.size(); ++index) {
                const auto &support = supports[index];
                if (!support.type.IsValid() || !IsValidRecordVersion(support.minimum) || !IsValidRecordVersion(support.maximum) ||
                    support.minimum > support.maximum)
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (supports[previous].type == support.type)
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] constexpr bool IsValidUnknownRecordPolicy(const NavigationUnknownRecordPolicy policy) noexcept {
            return static_cast<std::uint8_t>(policy) < static_cast<std::uint8_t>(NavigationUnknownRecordPolicy::Count);
        }

        [[nodiscard]] const NavigationAuthoredRecordSupport *FindSupport(const std::span<const NavigationAuthoredRecordSupport> supports,
                                                                         const NavigationAuthoredRecordTypeId type) noexcept {
            for (const auto &support : supports) {
                if (support.type == type)
                    return &support;
            }
            return nullptr;
        }

        [[nodiscard]] bool IsSupported(const NavigationAuthoredRecordSupport *support,
                                       const NavigationSourceSchemaVersion version) noexcept {
            return support != nullptr && version >= support->minimum && version <= support->maximum;
        }

        [[nodiscard]] bool CheckedAdd(const std::size_t left, const std::size_t right, std::size_t &result) noexcept {
            if (right > std::numeric_limits<std::size_t>::max() - left)
                return false;
            result = left + right;
            return true;
        }

        struct Reader final {
            std::span<const std::byte> bytes;
            std::size_t offset{};

            [[nodiscard]] bool ReadU8(std::uint8_t &value) noexcept {
                if (offset >= bytes.size())
                    return false;
                value = std::to_integer<std::uint8_t>(bytes[offset++]);
                return true;
            }

            [[nodiscard]] bool ReadU16(std::uint16_t &value) noexcept {
                std::uint8_t first{};
                std::uint8_t second{};
                if (!ReadU8(first) || !ReadU8(second))
                    return false;
                const auto decoded = static_cast<std::uint16_t>(first) | (static_cast<std::uint16_t>(second) << 8U);
                value = static_cast<std::uint16_t>(decoded);
                return true;
            }

            [[nodiscard]] bool ReadU32(std::uint32_t &value) noexcept {
                std::uint32_t decoded{};
                for (std::uint32_t shift = 0; shift < 32; shift += 8) {
                    std::uint8_t byte{};
                    if (!ReadU8(byte))
                        return false;
                    decoded |= static_cast<std::uint32_t>(byte) << shift;
                }
                value = decoded;
                return true;
            }

            [[nodiscard]] bool ReadU64(std::uint64_t &value) noexcept {
                std::uint64_t decoded{};
                for (std::uint32_t shift = 0; shift < 64; shift += 8) {
                    std::uint8_t byte{};
                    if (!ReadU8(byte))
                        return false;
                    decoded |= static_cast<std::uint64_t>(byte) << shift;
                }
                value = decoded;
                return true;
            }

            [[nodiscard]] bool ReadBytes(const std::size_t count, std::span<const std::byte> &values) noexcept {
                if (offset > bytes.size() || count > bytes.size() - offset)
                    return false;
                values = bytes.subspan(offset, count);
                offset += count;
                return true;
            }
        };

        [[nodiscard]] Result<void> ValidateContext(const NavigationSourceLoadContext &context) {
            if (!IsValidLimits(context.limits) || !IsValidSupportTable(context.supportedRecords) ||
                !IsValidSupportTable(context.supportedGeneratedPayloads) || !IsValidUnknownRecordPolicy(context.unknownPolicy))
                return Result<void>::Failure(MakeError(NavigationErrors::SourceEnvelopeInvalid));
            return Result<void>::Success();
        }

        [[nodiscard]] bool ChecksumMatches(const std::span<const std::byte> bytes) noexcept {
            const auto body = bytes.first(bytes.size() - ChecksumBytes);
            const auto encodedChecksum = bytes.last(ChecksumBytes);
            const auto actualChecksum = ComputeSha256(body);
            const auto actualBytes = std::as_bytes(std::span{actualChecksum.bytes});
            return std::ranges::equal(actualBytes, encodedChecksum);
        }

        struct DecodedEnvelopeHeader final {
            NavigationSourceSchemaVersion schemaVersion;
            std::uint32_t authoredRecordCount{};
            std::uint32_t generatedPayloadCount{};
            std::uint64_t declaredPayloadBytes{};
        };

        [[nodiscard]] Result<DecodedEnvelopeHeader> ReadEnvelopeHeader(Reader &reader) {
            if (const auto magic = [&reader] {
                std::span<const std::byte> value;
                if (!reader.ReadBytes(EnvelopeMagic.size(), value))
                    return std::span<const std::byte>{};
                return value;
            }(); magic.size() != EnvelopeMagic.size() || !std::ranges::equal(magic, std::as_bytes(std::span{EnvelopeMagic})))
                return Failure<DecodedEnvelopeHeader>(NavigationErrors::SourceEnvelopeInvalid);

            std::uint16_t schemaMajor{};
            std::uint16_t schemaMinor{};
            std::uint32_t reserved{};
            std::uint32_t authoredRecordCount{};
            std::uint32_t generatedPayloadCount{};
            std::uint64_t declaredPayloadBytes{};
            if (!reader.ReadU16(schemaMajor) || !reader.ReadU16(schemaMinor) || !reader.ReadU32(reserved) ||
                !reader.ReadU32(authoredRecordCount) || !reader.ReadU32(generatedPayloadCount) || !reader.ReadU64(declaredPayloadBytes) ||
                reserved != 0)
                return Failure<DecodedEnvelopeHeader>(NavigationErrors::SourceEnvelopeInvalid);

            return Result<DecodedEnvelopeHeader>::Success({.schemaVersion = {schemaMajor, schemaMinor},
                                                           .authoredRecordCount = authoredRecordCount,
                                                           .generatedPayloadCount = generatedPayloadCount,
                                                           .declaredPayloadBytes = declaredPayloadBytes});
        }

        [[nodiscard]] Result<std::vector<std::byte>> ReadPayload(Reader &reader, const std::uint32_t payloadBytes,
                                                                 const std::size_t maximumPayloadBytes,
                                                                 const std::size_t maximumTotalPayloadBytes,
                                                                 std::size_t &actualPayloadBytes) {
            if (payloadBytes > maximumPayloadBytes)
                return Failure<std::vector<std::byte>>(NavigationErrors::SourceSerializationCapacityExceeded);
            std::span<const std::byte> payload;
            if (!reader.ReadBytes(payloadBytes, payload))
                return Failure<std::vector<std::byte>>(NavigationErrors::SourceEnvelopeInvalid);
            if (!CheckedAdd(actualPayloadBytes, payload.size(), actualPayloadBytes))
                return Failure<std::vector<std::byte>>(NavigationErrors::SourceSerializationCapacityExceeded);
            if (actualPayloadBytes > maximumTotalPayloadBytes)
                return Failure<std::vector<std::byte>>(NavigationErrors::SourceSerializationCapacityExceeded);
            return Result<std::vector<std::byte>>::Success({payload.begin(), payload.end()});
        }

        struct AuthoredRecordHeader final {
            std::uint64_t idValue{};
            std::uint64_t typeValue{};
            std::uint16_t versionMajor{};
            std::uint16_t versionMinor{};
            std::uint8_t flags{};
            std::uint32_t payloadBytes{};
        };

        [[nodiscard]] Result<AuthoredRecordHeader> ReadAuthoredRecordHeader(Reader &reader) {
            AuthoredRecordHeader header;
            std::uint8_t recordReserved{};
            if (!reader.ReadU64(header.idValue) || !reader.ReadU64(header.typeValue) || !reader.ReadU16(header.versionMajor) ||
                !reader.ReadU16(header.versionMinor) || !reader.ReadU8(header.flags) || !reader.ReadU8(recordReserved) ||
                !reader.ReadU32(header.payloadBytes))
                return Failure<AuthoredRecordHeader>(NavigationErrors::SourceEnvelopeInvalid);
            const auto recordFlags = std::byte{header.flags};
            if (const auto knownFlags = static_cast<std::byte>(RequiredRecordFlag) | static_cast<std::byte>(OpaqueRecordFlag);
                recordReserved != 0 || (recordFlags | knownFlags) != knownFlags)
                return Failure<AuthoredRecordHeader>(NavigationErrors::SourceEnvelopeInvalid);
            return Result<AuthoredRecordHeader>::Success(header);
        }

        struct GeneratedPayloadHeader final {
            std::uint64_t typeValue{};
            std::uint16_t versionMajor{};
            std::uint16_t versionMinor{};
            std::uint8_t flags{};
            std::uint32_t payloadBytes{};
        };

        [[nodiscard]] Result<GeneratedPayloadHeader> ReadGeneratedPayloadHeader(Reader &reader) {
            GeneratedPayloadHeader header;
            std::uint8_t payloadReserved{};
            if (!reader.ReadU64(header.typeValue) || !reader.ReadU16(header.versionMajor) || !reader.ReadU16(header.versionMinor) ||
                !reader.ReadU8(header.flags) || !reader.ReadU8(payloadReserved) || !reader.ReadU32(header.payloadBytes))
                return Failure<GeneratedPayloadHeader>(NavigationErrors::SourceEnvelopeInvalid);
            const auto payloadFlags = std::byte{header.flags};
            if (const auto knownFlags =
                    static_cast<std::byte>(QuarantinedGeneratedFlag) | static_cast<std::byte>(UnsupportedGeneratedVersionFlag);
                payloadReserved != 0 || (payloadFlags | knownFlags) != knownFlags || payloadFlags == knownFlags)
                return Failure<GeneratedPayloadHeader>(NavigationErrors::SourceEnvelopeInvalid);
            return Result<GeneratedPayloadHeader>::Success(header);
        }

        [[nodiscard]] Result<NavigationAuthoredRecord> ReadAuthoredRecord(Reader &reader, const NavigationSourceLoadContext &context,
                                                                          std::size_t &actualPayloadBytes) {
            auto header = ReadAuthoredRecordHeader(reader);
            if (header.HasError())
                return Result<NavigationAuthoredRecord>::Failure(header.ErrorValue());
            const auto decodedHeader = std::move(header).Value();

            auto payload = ReadPayload(reader, decodedHeader.payloadBytes, context.limits.maximumRecordPayloadBytes,
                                       context.limits.maximumTotalPayloadBytes, actualPayloadBytes);
            if (payload.HasError())
                return Result<NavigationAuthoredRecord>::Failure(payload.ErrorValue());
            const auto id = NavigationAuthoredRecordId::Create(decodedHeader.idValue);
            const auto type = NavigationAuthoredRecordTypeId::Create(decodedHeader.typeValue);
            if (id.HasError() || type.HasError())
                return Failure<NavigationAuthoredRecord>(NavigationErrors::SourceEnvelopeInvalid);
            const auto recordFlags = std::byte{decodedHeader.flags};
            return Result<NavigationAuthoredRecord>::Success({.id = id.Value(),
                                                              .type = type.Value(),
                                                              .version = {decodedHeader.versionMajor, decodedHeader.versionMinor},
                                                              .required = (recordFlags & std::byte{RequiredRecordFlag}) != std::byte{0},
                                                              .payload = std::move(payload).Value(),
                                                              .opaque = (recordFlags & std::byte{OpaqueRecordFlag}) != std::byte{0}});
        }

        [[nodiscard]] Result<std::vector<NavigationAuthoredRecord>> ReadAuthoredRecords(Reader &reader, const std::uint32_t count,
                                                                                        const NavigationSourceLoadContext &context,
                                                                                        std::size_t &actualPayloadBytes) {
            std::vector<NavigationAuthoredRecord> records;
            records.reserve(static_cast<std::size_t>(count));
            for (std::uint32_t index = 0; index < count; ++index) {
                auto record = ReadAuthoredRecord(reader, context, actualPayloadBytes);
                if (record.HasError())
                    return Result<std::vector<NavigationAuthoredRecord>>::Failure(record.ErrorValue());
                records.push_back(std::move(record).Value());
            }
            return Result<std::vector<NavigationAuthoredRecord>>::Success(std::move(records));
        }

        [[nodiscard]] Result<NavigationGeneratedPayload> ReadGeneratedPayload(Reader &reader, const NavigationSourceLoadContext &context,
                                                                              std::size_t &actualPayloadBytes) {
            auto header = ReadGeneratedPayloadHeader(reader);
            if (header.HasError())
                return Result<NavigationGeneratedPayload>::Failure(header.ErrorValue());
            const auto decodedHeader = std::move(header).Value();

            auto payload = ReadPayload(reader, decodedHeader.payloadBytes, context.limits.maximumGeneratedPayloadBytes,
                                       context.limits.maximumTotalPayloadBytes, actualPayloadBytes);
            if (payload.HasError())
                return Result<NavigationGeneratedPayload>::Failure(payload.ErrorValue());
            const auto type = NavigationAuthoredRecordTypeId::Create(decodedHeader.typeValue);
            const NavigationSourceSchemaVersion version{decodedHeader.versionMajor, decodedHeader.versionMinor};
            if (type.HasError() || !IsValidRecordVersion(version))
                return Failure<NavigationGeneratedPayload>(NavigationErrors::SourceEnvelopeInvalid);
            const auto *support = FindSupport(context.supportedGeneratedPayloads, type.Value());
            const bool supported = IsSupported(support, version);
            if (const auto expectedFlags = [&]() noexcept {
                if (supported)
                    return std::uint8_t{0};
                return support == nullptr ? QuarantinedGeneratedFlag : UnsupportedGeneratedVersionFlag;
            }(); decodedHeader.flags != expectedFlags)
                return Failure<NavigationGeneratedPayload>(NavigationErrors::SourceEnvelopeInvalid);
            return Result<NavigationGeneratedPayload>::Success(
                {.type = type.Value(), .version = version, .payload = std::move(payload).Value()});
        }

        [[nodiscard]] Result<std::vector<NavigationGeneratedPayload>> ReadGeneratedPayloads(Reader &reader, const std::uint32_t count,
                                                                                            const NavigationSourceLoadContext &context,
                                                                                            std::size_t &actualPayloadBytes) {
            std::vector<NavigationGeneratedPayload> payloads;
            payloads.reserve(static_cast<std::size_t>(count));
            for (std::uint32_t index = 0; index < count; ++index) {
                auto payload = ReadGeneratedPayload(reader, context, actualPayloadBytes);
                if (payload.HasError())
                    return Result<std::vector<NavigationGeneratedPayload>>::Failure(payload.ErrorValue());
                payloads.push_back(std::move(payload).Value());
            }
            return Result<std::vector<NavigationGeneratedPayload>>::Success(std::move(payloads));
        }

        [[nodiscard]] Result<void> ValidateDecodeInput(const std::span<const std::byte> bytes, const NavigationSourceLoadContext &context) {
            if (const auto valid = ValidateContext(context); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            if (bytes.size() < EnvelopeHeaderBytes + ChecksumBytes)
                return Result<void>::Failure(MakeError(NavigationErrors::SourceEnvelopeInvalid));
            if (bytes.size() > context.limits.maximumEnvelopeBytes)
                return Result<void>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
            if (!ChecksumMatches(bytes))
                return Result<void>::Failure(MakeError(NavigationErrors::SourceChecksumMismatch));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDecodedHeader(const DecodedEnvelopeHeader &header, const NavigationSourceLoadContext &context) {
            if (!IsSupportedEnvelopeVersion(header.schemaVersion))
                return Result<void>::Failure(MakeError(NavigationErrors::SourceUnsupportedVersion));
            if (header.authoredRecordCount > context.limits.maximumAuthoredRecords ||
                header.generatedPayloadCount > context.limits.maximumGeneratedPayloads)
                return Result<void>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
            if (header.declaredPayloadBytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
                static_cast<std::size_t>(header.declaredPayloadBytes) > context.limits.maximumTotalPayloadBytes)
                return Result<void>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<NavigationSourceRecords> Decode(const std::span<const std::byte> bytes,
                                                             const NavigationSourceLoadContext &context) {
            if (const auto valid = ValidateDecodeInput(bytes, context); valid.HasError())
                return Result<NavigationSourceRecords>::Failure(valid.ErrorValue());

            const auto body = bytes.first(bytes.size() - ChecksumBytes);
            Reader reader{.bytes = body};
            auto header = ReadEnvelopeHeader(reader);
            if (header.HasError())
                return Result<NavigationSourceRecords>::Failure(header.ErrorValue());
            const auto decodedHeader = std::move(header).Value();
            if (const auto valid = ValidateDecodedHeader(decodedHeader, context); valid.HasError())
                return Result<NavigationSourceRecords>::Failure(valid.ErrorValue());

            if (reader.offset > body.size() ||
                decodedHeader.authoredRecordCount > (body.size() - reader.offset) / AuthoredRecordHeaderBytes)
                return Failure<NavigationSourceRecords>(NavigationErrors::SourceEnvelopeInvalid);

            std::size_t actualPayloadBytes{};
            auto records = ReadAuthoredRecords(reader, decodedHeader.authoredRecordCount, context, actualPayloadBytes);
            if (records.HasError())
                return Result<NavigationSourceRecords>::Failure(records.ErrorValue());

            if (reader.offset > body.size() ||
                decodedHeader.generatedPayloadCount > (body.size() - reader.offset) / GeneratedPayloadHeaderBytes)
                return Failure<NavigationSourceRecords>(NavigationErrors::SourceEnvelopeInvalid);

            auto generatedPayloads = ReadGeneratedPayloads(reader, decodedHeader.generatedPayloadCount, context, actualPayloadBytes);
            if (generatedPayloads.HasError())
                return Result<NavigationSourceRecords>::Failure(generatedPayloads.ErrorValue());

            if (reader.offset != body.size() || actualPayloadBytes != static_cast<std::size_t>(decodedHeader.declaredPayloadBytes))
                return Failure<NavigationSourceRecords>(NavigationErrors::SourceEnvelopeInvalid);
            return NavigationSourceRecords::Create(decodedHeader.schemaVersion, std::move(records).Value(),
                                                   std::move(generatedPayloads).Value(), context);
        }
    }  // namespace

    /** @copydoc DeserializeNavigationSourceRecords */
    Result<NavigationSourceRecords> DeserializeNavigationSourceRecords(const std::span<const std::byte> bytes,
                                                                       const NavigationSourceLoadContext &context) {
        return Decode(bytes, context);
    }

    /** @copydoc DeserializeNavigationSourceRecords */
    Result<NavigationSourceRecords> DeserializeNavigationSourceRecords(const std::vector<std::byte> &bytes,
                                                                       const NavigationSourceLoadContext &context) {
        return DeserializeNavigationSourceRecords(std::span<const std::byte>{bytes.data(), bytes.size()}, context);
    }
}  // namespace Horo::Navigation
