#include "Horo/Navigation/NavigationDataSerialization.h"

#include "Horo/Foundation/Sha256.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationDataSerializationInternal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

namespace Horo::Navigation {
    namespace {
        using namespace SerializationInternal;

        struct Writer final {
            std::vector<std::byte> bytes;

            void U8(const std::uint8_t value) {
                bytes.push_back(std::byte{value});
            }

            void U16(const std::uint16_t value) {
                U8(static_cast<std::uint8_t>(value));
                U8(static_cast<std::uint8_t>(value >> 8U));
            }

            void U32(const std::uint32_t value) {
                for (std::uint32_t shift = 0; shift < 32; shift += 8)
                    U8(static_cast<std::uint8_t>(value >> shift));
            }

            void U64(const std::uint64_t value) {
                for (std::uint32_t shift = 0; shift < 64; shift += 8)
                    U8(static_cast<std::uint8_t>(value >> shift));
            }

            void Bytes(const std::span<const std::byte> values) {
                bytes.insert(bytes.end(), values.begin(), values.end());
            }
        };

        [[nodiscard]] Result<void> ValidateRecordIdentityAndPayload(const NavigationAuthoredRecord &record,
                                                                    const NavigationSourceLoadContext &context) {
            if (!record.id.IsValid() || !record.type.IsValid() || !IsValidRecordVersion(record.version))
                return Result<void>::Failure(MakeError(NavigationErrors::SourceEnvelopeInvalid));
            if (record.payload.size() > context.limits.maximumRecordPayloadBytes)
                return Result<void>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRecordSupport(const NavigationAuthoredRecord &record,
                                                         const NavigationSourceLoadContext &context) {
            const auto *support = FindSupport(context.supportedRecords, record.type);
            const bool supported = IsSupported(support, record.version);
            if (record.opaque) {
                if (record.required || context.unknownPolicy != NavigationUnknownRecordPolicy::PreserveOptionalInert)
                    return Result<void>::Failure(MakeError(NavigationErrors::SourceUnknownAuthoredRecord));
                if (supported)
                    return Result<void>::Failure(MakeError(NavigationErrors::SourceEnvelopeInvalid));
                return Result<void>::Success();
            }
            if (supported)
                return Result<void>::Success();
            if (context.unknownPolicy == NavigationUnknownRecordPolicy::PreserveOptionalInert && !record.required)
                return Result<void>::Success();
            return Result<void>::Failure(MakeError(NavigationErrors::SourceUnknownAuthoredRecord));
        }

        [[nodiscard]] Result<void> ValidateRecordShape(const NavigationAuthoredRecord &record, const NavigationSourceLoadContext &context) {
            if (const auto valid = ValidateRecordIdentityAndPayload(record, context); valid.HasError())
                return valid;
            return ValidateRecordSupport(record, context);
        }

        struct ValidatedCandidate final {
            NavigationSourceSchemaVersion schemaVersion;
            std::vector<NavigationAuthoredRecord> records;
            std::vector<NavigationGeneratedPayload> generatedPayloads;
            std::vector<NavigationQuarantinedGeneratedPayload> quarantinedGeneratedPayloads;
        };

        [[nodiscard]] Result<void> ValidateAndCanonicalizeAuthored(std::vector<NavigationAuthoredRecord> &records,
                                                                   const NavigationSourceLoadContext &context,
                                                                   std::size_t &totalPayloadBytes) {
            for (auto &record : records) {
                if (const auto valid = ValidateRecordShape(record, context); valid.HasError())
                    return valid;
                if (!record.opaque && !IsSupported(FindSupport(context.supportedRecords, record.type), record.version) &&
                    context.unknownPolicy == NavigationUnknownRecordPolicy::PreserveOptionalInert && !record.required)
                    record.opaque = true;
                if (!CheckedAdd(totalPayloadBytes, record.payload.size(), totalPayloadBytes))
                    return Result<void>::Failure(MakeError(NavigationErrors::SourceSerializationCapacityExceeded));
            }
            std::ranges::sort(records, [](const NavigationAuthoredRecord &left, const NavigationAuthoredRecord &right) {
                return left.id.Value() < right.id.Value();
            });
            if (std::ranges::adjacent_find(records, [](const NavigationAuthoredRecord &left, const NavigationAuthoredRecord &right) {
                return left.id == right.id;
            }) != records.end())
                return Result<void>::Failure(MakeError(NavigationErrors::SourceDuplicateIdentity));
            return Result<void>::Success();
        }

        struct GeneratedCandidate final {
            std::vector<NavigationGeneratedPayload> supported;
            std::vector<NavigationQuarantinedGeneratedPayload> quarantined;
        };

        [[nodiscard]] Result<GeneratedCandidate> CanonicalizeGenerated(std::vector<NavigationGeneratedPayload> payloads,
                                                                       const NavigationSourceLoadContext &context,
                                                                       std::size_t &totalPayloadBytes) {
            GeneratedCandidate result;
            result.supported.reserve(payloads.size());
            result.quarantined.reserve(payloads.size());
            for (auto &payload : payloads) {
                if (!payload.type.IsValid() || !IsValidRecordVersion(payload.version))
                    return Failure<GeneratedCandidate>(NavigationErrors::SourceEnvelopeInvalid);
                if (payload.payload.size() > context.limits.maximumGeneratedPayloadBytes ||
                    !CheckedAdd(totalPayloadBytes, payload.payload.size(), totalPayloadBytes))
                    return Failure<GeneratedCandidate>(NavigationErrors::SourceSerializationCapacityExceeded);
                const auto *support = FindSupport(context.supportedGeneratedPayloads, payload.type);
                if (IsSupported(support, payload.version))
                    result.supported.push_back(std::move(payload));
                else
                    result.quarantined.push_back({.payload = std::move(payload),
                                                  .reason = support == nullptr
                                                                ? NavigationGeneratedPayloadQuarantineReason::UnknownType
                                                                : NavigationGeneratedPayloadQuarantineReason::UnsupportedVersion});
            }
            std::ranges::sort(result.supported, IsCanonicalGeneratedOrder);
            std::ranges::sort(result.quarantined, QuarantineLess);
            return Result<GeneratedCandidate>::Success(std::move(result));
        }

        [[nodiscard]] Result<ValidatedCandidate> CreateRecords(const NavigationSourceSchemaVersion schemaVersion,
                                                               std::vector<NavigationAuthoredRecord> records,
                                                               std::vector<NavigationGeneratedPayload> generatedPayloads,
                                                               const NavigationSourceLoadContext &context) {
            if (ValidateContext(context).HasError())
                return Failure<ValidatedCandidate>(NavigationErrors::SourceEnvelopeInvalid);
            if (!IsSupportedEnvelopeVersion(schemaVersion))
                return Failure<ValidatedCandidate>(NavigationErrors::SourceUnsupportedVersion);
            if (records.size() > context.limits.maximumAuthoredRecords ||
                generatedPayloads.size() > context.limits.maximumGeneratedPayloads)
                return Failure<ValidatedCandidate>(NavigationErrors::SourceSerializationCapacityExceeded);

            std::size_t totalPayloadBytes{};
            if (const auto valid = ValidateAndCanonicalizeAuthored(records, context, totalPayloadBytes); valid.HasError())
                return Result<ValidatedCandidate>::Failure(valid.ErrorValue());
            auto generated = CanonicalizeGenerated(std::move(generatedPayloads), context, totalPayloadBytes);
            if (generated.HasError())
                return Result<ValidatedCandidate>::Failure(generated.ErrorValue());
            auto generatedCandidate = std::move(generated).Value();
            if (totalPayloadBytes > context.limits.maximumTotalPayloadBytes)
                return Failure<ValidatedCandidate>(NavigationErrors::SourceSerializationCapacityExceeded);
            return Result<ValidatedCandidate>::Success({.schemaVersion = schemaVersion,
                                                        .records = std::move(records),
                                                        .generatedPayloads = std::move(generatedCandidate.supported),
                                                        .quarantinedGeneratedPayloads = std::move(generatedCandidate.quarantined)});
        }

        [[nodiscard]] bool IsCanonicalInput(const NavigationSourceRecords &source) noexcept {
            const auto records = source.Records();
            for (std::size_t index = 1; index < records.size(); ++index) {
                if (records[index - 1].id.Value() >= records[index].id.Value())
                    return false;
            }
            const auto generated = source.GeneratedPayloads();
            for (std::size_t index = 1; index < generated.size(); ++index) {
                if (!IsCanonicalGeneratedOrder(generated[index - 1], generated[index]))
                    return false;
            }
            const auto quarantined = source.QuarantinedGeneratedPayloads();
            return std::ranges::is_sorted(quarantined, QuarantineLess);
        }

        struct EncodedSize final {
            std::size_t payloadBytes{};
            std::size_t encodedBytes{EnvelopeHeaderBytes + ChecksumBytes};
            std::size_t generatedCount{};
        };

        [[nodiscard]] bool AccumulateEncodedSize(const std::size_t payloadSize, const std::size_t maximumPayloadBytes,
                                                 const std::size_t headerBytes, EncodedSize &size) noexcept {
            return payloadSize <= maximumPayloadBytes && CheckedAdd(size.payloadBytes, payloadSize, size.payloadBytes) &&
                   CheckedAdd(size.encodedBytes, headerBytes + payloadSize, size.encodedBytes);
        }

        [[nodiscard]] bool AccumulateAuthoredSizes(const std::span<const NavigationAuthoredRecord> records,
                                                   const NavigationSerializationLimits &limits, EncodedSize &size) noexcept {
            for (const auto &record : records) {
                if (!AccumulateEncodedSize(record.payload.size(), limits.maximumRecordPayloadBytes, AuthoredRecordHeaderBytes, size))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool AccumulateGeneratedSizes(const std::span<const NavigationGeneratedPayload> payloads,
                                                    const std::size_t maximumPayloadBytes, EncodedSize &size) noexcept {
            for (const auto &payload : payloads) {
                if (!AccumulateEncodedSize(payload.payload.size(), maximumPayloadBytes, GeneratedPayloadHeaderBytes, size))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool AccumulateQuarantinedSizes(const std::span<const NavigationQuarantinedGeneratedPayload> payloads,
                                                      const std::size_t maximumPayloadBytes, EncodedSize &size) noexcept {
            for (const auto &payload : payloads) {
                if (!AccumulateEncodedSize(payload.payload.payload.size(), maximumPayloadBytes, GeneratedPayloadHeaderBytes, size))
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<EncodedSize> CalculateEncodedSize(const NavigationSourceRecords &source,
                                                               const NavigationSerializationLimits &limits) {
            EncodedSize size{.generatedCount = source.GeneratedPayloads().size() + source.QuarantinedGeneratedPayloads().size()};
            if (source.Records().size() > limits.maximumAuthoredRecords || size.generatedCount > limits.maximumGeneratedPayloads)
                return Failure<EncodedSize>(NavigationErrors::SourceSerializationCapacityExceeded);
            if (!AccumulateAuthoredSizes(source.Records(), limits, size) ||
                !AccumulateGeneratedSizes(source.GeneratedPayloads(), limits.maximumGeneratedPayloadBytes, size) ||
                !AccumulateQuarantinedSizes(source.QuarantinedGeneratedPayloads(), limits.maximumGeneratedPayloadBytes, size))
                return Failure<EncodedSize>(NavigationErrors::SourceSerializationCapacityExceeded);
            if (size.payloadBytes > limits.maximumTotalPayloadBytes || size.encodedBytes > limits.maximumEnvelopeBytes ||
                size.encodedBytes > std::numeric_limits<std::uint32_t>::max())
                return Failure<EncodedSize>(NavigationErrors::SourceSerializationCapacityExceeded);
            return Result<EncodedSize>::Success(size);
        }

        void WriteAuthoredRecords(Writer &writer, const std::span<const NavigationAuthoredRecord> records) {
            for (const auto &record : records) {
                writer.U64(record.id.Value());
                writer.U64(record.type.Value());
                writer.U16(record.version.major);
                writer.U16(record.version.minor);
                writer.U8(static_cast<std::uint8_t>((record.required ? RequiredRecordFlag : 0U) | (record.opaque ? OpaqueRecordFlag : 0U)));
                writer.U8(0);
                writer.U32(static_cast<std::uint32_t>(record.payload.size()));
                writer.Bytes(record.payload);
            }
        }

        void WriteGeneratedPayload(Writer &writer, const NavigationGeneratedPayload &payload, const std::uint8_t flags) {
            writer.U64(payload.type.Value());
            writer.U16(payload.version.major);
            writer.U16(payload.version.minor);
            writer.U8(flags);
            writer.U8(0);
            writer.U32(static_cast<std::uint32_t>(payload.payload.size()));
            writer.Bytes(payload.payload);
        }

        void WriteGeneratedPayloads(Writer &writer, const NavigationSourceRecords &source) {
            for (const auto &payload : source.GeneratedPayloads())
                WriteGeneratedPayload(writer, payload, 0);
            for (const auto &quarantined : source.QuarantinedGeneratedPayloads())
                WriteGeneratedPayload(writer, quarantined.payload, QuarantineFlag(quarantined.reason));
        }

        [[nodiscard]] Result<std::vector<std::byte>> Encode(const NavigationSourceRecords &source,
                                                            const NavigationSerializationLimits &limits) {
            if (!IsValidLimits(limits) || !IsCanonicalInput(source))
                return Failure<std::vector<std::byte>>(NavigationErrors::SourceEnvelopeInvalid);
            auto size = CalculateEncodedSize(source, limits);
            if (size.HasError())
                return Result<std::vector<std::byte>>::Failure(size.ErrorValue());
            const auto encodedSize = std::move(size).Value();

            Writer writer;
            writer.bytes.reserve(encodedSize.encodedBytes);
            writer.Bytes(std::as_bytes(std::span{EnvelopeMagic}));
            writer.U16(source.SchemaVersion().major);
            writer.U16(source.SchemaVersion().minor);
            writer.U32(0);
            writer.U32(static_cast<std::uint32_t>(source.Records().size()));
            writer.U32(static_cast<std::uint32_t>(encodedSize.generatedCount));
            writer.U64(static_cast<std::uint64_t>(encodedSize.payloadBytes));
            WriteAuthoredRecords(writer, source.Records());
            WriteGeneratedPayloads(writer, source);
            const Sha256Digest checksum = ComputeSha256(writer.bytes);
            writer.Bytes(std::as_bytes(std::span{checksum.bytes}));
            return Result<std::vector<std::byte>>::Success(std::move(writer.bytes));
        }

    }  // namespace

    NavigationSourceRecords::NavigationSourceRecords(
        const NavigationSourceSchemaVersion schemaVersion, std::vector<NavigationAuthoredRecord> records,
        std::vector<NavigationGeneratedPayload> generatedPayloads,
        std::vector<NavigationQuarantinedGeneratedPayload> quarantinedGeneratedPayloads) noexcept
        : schemaVersion_(schemaVersion), records_(std::move(records)), generatedPayloads_(std::move(generatedPayloads)),
          quarantinedGeneratedPayloads_(std::move(quarantinedGeneratedPayloads)) {}

    /** @copydoc NavigationSourceRecords::Create(NavigationSourceSchemaVersion, std::vector<NavigationAuthoredRecord>, const
     * NavigationSourceLoadContext &) */
    Result<NavigationSourceRecords> NavigationSourceRecords::Create(const NavigationSourceSchemaVersion schemaVersion,
                                                                    std::vector<NavigationAuthoredRecord> records,
                                                                    const NavigationSourceLoadContext &context) {
        auto candidate = CreateRecords(schemaVersion, std::move(records), {}, context);
        if (candidate.HasError())
            return Result<NavigationSourceRecords>::Failure(candidate.ErrorValue());
        auto created = std::move(candidate).Value();
        return Result<NavigationSourceRecords>::Success(NavigationSourceRecords{created.schemaVersion, std::move(created.records),
                                                                                std::move(created.generatedPayloads),
                                                                                std::move(created.quarantinedGeneratedPayloads)});
    }

    /** @copydoc NavigationSourceRecords::Create(NavigationSourceSchemaVersion, std::vector<NavigationAuthoredRecord>,
     * std::vector<NavigationGeneratedPayload>, const NavigationSourceLoadContext &) */
    Result<NavigationSourceRecords> NavigationSourceRecords::Create(const NavigationSourceSchemaVersion schemaVersion,
                                                                    std::vector<NavigationAuthoredRecord> records,
                                                                    std::vector<NavigationGeneratedPayload> generatedPayloads,
                                                                    const NavigationSourceLoadContext &context) {
        auto candidate = CreateRecords(schemaVersion, std::move(records), std::move(generatedPayloads), context);
        if (candidate.HasError())
            return Result<NavigationSourceRecords>::Failure(candidate.ErrorValue());
        auto created = std::move(candidate).Value();
        return Result<NavigationSourceRecords>::Success(NavigationSourceRecords{created.schemaVersion, std::move(created.records),
                                                                                std::move(created.generatedPayloads),
                                                                                std::move(created.quarantinedGeneratedPayloads)});
    }

    /** @copydoc SerializeNavigationSourceRecords */
    Result<std::vector<std::byte>> SerializeNavigationSourceRecords(const NavigationSourceRecords &source,
                                                                    const NavigationSerializationLimits &limits) {
        return Encode(source, limits);
    }

}  // namespace Horo::Navigation
