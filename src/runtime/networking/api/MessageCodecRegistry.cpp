#include "Horo/Network/MessageCodecRegistry.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <tuple>
#include <utility>

namespace Horo::Network {
    namespace {
        constexpr std::array<std::byte, 4> EnvelopeMagic{static_cast<std::byte>('H'), static_cast<std::byte>('M'),
                                                         static_cast<std::byte>('S'), static_cast<std::byte>('G')};
        constexpr std::uint8_t EnvelopeVersion = 1;
        constexpr std::size_t FixedHeaderBytes = 32;
        constexpr std::size_t FieldHeaderBytes = 6;

        template <typename T> [[nodiscard]] Result<T> Fail(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        [[nodiscard]] bool CheckedAdd(const std::size_t left, const std::size_t right, std::size_t &sum) noexcept {
            if (left > std::numeric_limits<std::size_t>::max() - right)
                return false;
            sum = left + right;
            return true;
        }

        [[nodiscard]] std::optional<std::size_t> CheckedSum(const std::size_t left, const std::size_t right) noexcept {
            std::size_t sum{};
            return CheckedAdd(left, right, sum) ? std::optional{sum} : std::nullopt;
        }

        [[nodiscard]] auto CodecKey(const MessageCodecDescriptor &value) noexcept {
            return std::tuple{value.protocol.Value(), value.message.Value()};
        }

        [[nodiscard]] auto FieldKey(const MessageEnvelopeFieldDescriptor &value) noexcept {
            return std::tuple{value.protocol.Value(), value.message.Value(), value.id.Value()};
        }

        [[nodiscard]] bool ValidRegistryLimits(const MessageCodecRegistryLimits &limits) noexcept {
            return limits.maximumCodecs > 0 && limits.maximumPayloadBytes <= std::numeric_limits<std::uint32_t>::max() &&
                   limits.maximumFieldBytes <= std::numeric_limits<std::uint16_t>::max();
        }

        [[nodiscard]] bool WithinRegistryCapacity(const MessageCodecContributions &contributions,
                                                  const MessageCodecRegistryLimits &limits) noexcept {
            return contributions.codecs.size() <= limits.maximumCodecs && contributions.fields.size() <= limits.maximumFields;
        }

        [[nodiscard]] bool ValidCodecs(const std::vector<MessageCodecDescriptor> &codecs, const ProtocolIdentityRegistry &identities,
                                       const MessageCodecRegistryLimits &limits) noexcept {
            return std::ranges::all_of(codecs, [&](const MessageCodecDescriptor &codec) {
                const auto message = identities.FindMessage(codec.protocol, codec.message);
                return message.has_value() && codec.schema.IsValid() && message->schema == codec.schema &&
                       codec.supportedVersions.IsValid() && codec.supportedVersions.Contains(message->version) &&
                       codec.maximumPayloadBytes <= limits.maximumPayloadBytes;
            });
        }

        [[nodiscard]] bool ValidFields(const std::vector<MessageEnvelopeFieldDescriptor> &fields,
                                       const std::vector<MessageCodecDescriptor> &codecs,
                                       const MessageCodecRegistryLimits &limits) noexcept {
            return std::ranges::all_of(fields, [&](const MessageEnvelopeFieldDescriptor &field) {
                const auto key = std::tuple{field.protocol.Value(), field.message.Value()};
                const auto codec = std::ranges::lower_bound(codecs, key, {}, CodecKey);
                return field.id.IsValid() && field.maximumEncodedBytes <= limits.maximumFieldBytes && codec != codecs.end() &&
                       CodecKey(*codec) == key;
            });
        }

        [[nodiscard]] const ErrorCodeDescriptor *AdmissionError(const MessageEnvelopeAdmissionState state) noexcept {
            using enum MessageEnvelopeAdmissionState;
            switch (state) {
                case Accepting:
                    return nullptr;
                case Cancelled:
                    return &NetworkErrors::MessageEnvelopeCancelled;
                case TimedOut:
                    return &NetworkErrors::MessageEnvelopeTimedOut;
                case ShuttingDown:
                    return &NetworkErrors::MessageEnvelopeShuttingDown;
                case Count:
                    return &NetworkErrors::MessageEnvelopeInvalid;
            }
            return &NetworkErrors::MessageEnvelopeInvalid;
        }

        [[nodiscard]] bool ValidEnvelopeLimits(const MessageEnvelopeLimits &limits) noexcept {
            return limits.maximumFrameBytes >= FixedHeaderBytes && limits.maximumFrameBytes <= std::numeric_limits<std::uint32_t>::max() &&
                   limits.maximumPayloadBytes <= std::numeric_limits<std::uint32_t>::max() &&
                   limits.maximumFields <= std::numeric_limits<std::uint16_t>::max() &&
                   limits.maximumExtensionBytes <= std::numeric_limits<std::uint16_t>::max() - FixedHeaderBytes &&
                   limits.maximumPayloadBytes <= limits.maximumFrameBytes - FixedHeaderBytes &&
                   limits.maximumExtensionBytes <= limits.maximumFrameBytes - FixedHeaderBytes;
        }

        [[nodiscard]] Result<MessageCodecDescriptor> ValidateEnvelopeIdentity(const MessageEnvelope &envelope,
                                                                              const MessageCodecRegistry &registry) {
            if (!envelope.protocol.IsValid() || !envelope.message.IsValid() || !envelope.schema.IsValid() ||
                !envelope.schemaVersion.IsValid())
                return Fail<MessageCodecDescriptor>(NetworkErrors::MessageEnvelopeInvalid);
            const auto found = registry.FindCodec(envelope.protocol, envelope.message);
            if (!found.has_value())
                return Fail<MessageCodecDescriptor>(NetworkErrors::MessageCodecUnknown);
            if (found->schema != envelope.schema || !found->supportedVersions.Contains(envelope.schemaVersion))
                return Fail<MessageCodecDescriptor>(NetworkErrors::MessageSchemaIncompatible);
            return Result<MessageCodecDescriptor>::Success(*found);
        }

        struct EncodingPlan final {
            std::vector<const MessageEnvelopeField *> fields;
            std::size_t headerBytes{};
            std::size_t totalBytes{};
        };

        [[nodiscard]] Result<std::size_t> ValidateEncodingField(const MessageEnvelope &envelope, const MessageEnvelopeField &field,
                                                                const MessageCodecRegistry &registry) {
            if (!field.id.IsValid() || field.requirement >= MessageEnvelopeFieldRequirement::Count)
                return Fail<std::size_t>(NetworkErrors::MessageEnvelopeInvalid);
            const auto descriptor = registry.FindField(envelope.protocol, envelope.message, field.id);
            if (!descriptor.has_value())
                return Fail<std::size_t>(NetworkErrors::MessageCodecUnknown);
            if (field.value.size() > descriptor->maximumEncodedBytes || field.value.size() > std::numeric_limits<std::uint16_t>::max())
                return Fail<std::size_t>(NetworkErrors::MessageEnvelopeCapacityExceeded);
            std::size_t encodedBytes{};
            return CheckedAdd(FieldHeaderBytes, field.value.size(), encodedBytes)
                       ? Result<std::size_t>::Success(encodedBytes)
                       : Fail<std::size_t>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        }

        [[nodiscard]] Result<void> CompleteEncodingPlan(EncodingPlan &plan, const std::size_t extensionBytes,
                                                        const std::size_t payloadBytes, const MessageEnvelopeLimits &limits) {
            if (extensionBytes > limits.maximumExtensionBytes || !CheckedAdd(FixedHeaderBytes, extensionBytes, plan.headerBytes))
                return Fail<void>(NetworkErrors::MessageEnvelopeCapacityExceeded);
            if (plan.headerBytes > std::numeric_limits<std::uint16_t>::max() ||
                !CheckedAdd(plan.headerBytes, payloadBytes, plan.totalBytes) || plan.totalBytes > limits.maximumFrameBytes)
                return Fail<void>(NetworkErrors::MessageEnvelopeCapacityExceeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<EncodingPlan> BuildEncodingPlan(const MessageEnvelope &envelope, const MessageCodecRegistry &registry,
                                                             const MessageCodecDescriptor &codec, const MessageEnvelopeLimits &limits) {
            if (envelope.payload.size() > limits.maximumPayloadBytes || envelope.payload.size() > codec.maximumPayloadBytes ||
                envelope.fields.size() > limits.maximumFields)
                return Fail<EncodingPlan>(NetworkErrors::MessageEnvelopeCapacityExceeded);

            try {
                EncodingPlan plan;
                plan.fields.reserve(envelope.fields.size());
                std::size_t extensionBytes{};
                for (const MessageEnvelopeField &field : envelope.fields) {
                    const Result<std::size_t> encodedFieldBytes = ValidateEncodingField(envelope, field, registry);
                    if (encodedFieldBytes.HasError())
                        return Result<EncodingPlan>::Failure(encodedFieldBytes.ErrorValue());
                    if (!CheckedAdd(extensionBytes, encodedFieldBytes.Value(), extensionBytes))
                        return Fail<EncodingPlan>(NetworkErrors::MessageEnvelopeCapacityExceeded);
                    plan.fields.push_back(&field);
                }
                if (const Result<void> complete = CompleteEncodingPlan(plan, extensionBytes, envelope.payload.size(), limits);
                    complete.HasError())
                    return Result<EncodingPlan>::Failure(complete.ErrorValue());

                std::ranges::sort(plan.fields, {}, [](const MessageEnvelopeField *field) {
                    return field->id;
                });
                if (std::ranges::adjacent_find(plan.fields, {}, [](const MessageEnvelopeField *field) {
                    return field->id;
                }) != plan.fields.end())
                    return Fail<EncodingPlan>(NetworkErrors::MessageEnvelopeInvalid);
                return Result<EncodingPlan>::Success(std::move(plan));
            } catch (const std::bad_alloc &) {
                return Fail<EncodingPlan>(NetworkErrors::MessageEnvelopeCapacityExceeded);
            }
        }

        template <class Output> void WriteU8(Output &output, const std::uint8_t value) {
            output.push_back(static_cast<std::byte>(value));
        }

        template <class Output> void WriteU16(Output &output, const std::uint16_t value) {
            WriteU8(output, static_cast<std::uint8_t>(value >> 8U));
            WriteU8(output, static_cast<std::uint8_t>(value));
        }

        template <class Output> void WriteU32(Output &output, const std::uint32_t value) {
            WriteU16(output, static_cast<std::uint16_t>(value >> 16U));
            WriteU16(output, static_cast<std::uint16_t>(value));
        }

        /** @brief One canonical fixed-header writer shared by owned and secure borrowed payload encoding. */
        template <class Output, class Envelope>
        void WriteFixedHeader(Output &output, const Envelope &envelope, const std::size_t headerBytes, const std::size_t payloadBytes,
                              const std::size_t fieldCount) {
            for (const auto byte : EnvelopeMagic)
                output.push_back(byte);
            WriteU8(output, EnvelopeVersion);
            WriteU8(output, 0);
            WriteU16(output, static_cast<std::uint16_t>(headerBytes));
            WriteU16(output, envelope.protocol.Value());
            WriteU16(output, envelope.message.Value());
            WriteU16(output, envelope.schema.Value());
            WriteU16(output, envelope.schemaVersion.major);
            WriteU16(output, envelope.schemaVersion.minor);
            WriteU32(output, envelope.sequence.Value());
            WriteU32(output, envelope.acknowledgement.Value());
            WriteU32(output, static_cast<std::uint32_t>(payloadBytes));
            WriteU16(output, static_cast<std::uint16_t>(fieldCount));
        }

        struct BoundedOutput final {
            std::span<std::byte> bytes;
            std::size_t offset{};

            void push_back(const std::byte value) noexcept {
                bytes[offset++] = value;
            }
        };

        [[nodiscard]] std::uint8_t ReadU8(const std::span<const std::byte> input, std::size_t &offset) noexcept {
            return std::to_integer<std::uint8_t>(input[offset++]);
        }

        [[nodiscard]] std::uint16_t ReadU16(const std::span<const std::byte> input, std::size_t &offset) noexcept {
            const std::uint16_t high = ReadU8(input, offset);
            return static_cast<std::uint16_t>((high << 8U) | ReadU8(input, offset));
        }

        [[nodiscard]] std::uint32_t ReadU32(const std::span<const std::byte> input, std::size_t &offset) noexcept {
            const std::uint32_t high = ReadU16(input, offset);
            return (high << 16U) | ReadU16(input, offset);
        }

        struct ParsedHeader final {
            MessageEnvelope envelope;
            std::size_t headerBytes{};
            std::size_t payloadBytes{};
            std::size_t fieldCount{};
        };

        [[nodiscard]] const ErrorCodeDescriptor *FramePreambleError(const std::span<const std::byte> frame,
                                                                    const MessageEnvelopeLimits &limits) noexcept {
            if (!ValidEnvelopeLimits(limits))
                return &NetworkErrors::MessageEnvelopeInvalid;
            if (frame.size() > limits.maximumFrameBytes)
                return &NetworkErrors::MessageEnvelopeCapacityExceeded;
            if (frame.size() < FixedHeaderBytes)
                return &NetworkErrors::MessageEnvelopeInvalid;
            return std::ranges::equal(EnvelopeMagic, frame.first(EnvelopeMagic.size())) ? nullptr : &NetworkErrors::MessageEnvelopeInvalid;
        }

        [[nodiscard]] bool ValidParsedIdentities(const Result<ProtocolId> &protocol, const Result<MessageTypeId> &message,
                                                 const Result<MessageSchemaId> &schema, const MessageSchemaVersion version) noexcept {
            return protocol.HasValue() && message.HasValue() && schema.HasValue() && version.IsValid();
        }

        [[nodiscard]] Result<ParsedHeader> ParseHeader(const std::span<const std::byte> frame, const MessageEnvelopeLimits &limits) {
            if (const ErrorCodeDescriptor *preamble = FramePreambleError(frame, limits); preamble != nullptr)
                return Fail<ParsedHeader>(*preamble);

            std::size_t offset = EnvelopeMagic.size();
            if (ReadU8(frame, offset) != EnvelopeVersion || ReadU8(frame, offset) != 0)
                return Fail<ParsedHeader>(NetworkErrors::MessageEnvelopeInvalid);
            ParsedHeader parsed;
            parsed.headerBytes = ReadU16(frame, offset);
            const auto protocol = ProtocolId::Create(ReadU16(frame, offset));
            const auto message = MessageTypeId::Create(ReadU16(frame, offset));
            const auto schema = MessageSchemaId::Create(ReadU16(frame, offset));
            parsed.envelope.schemaVersion = {ReadU16(frame, offset), ReadU16(frame, offset)};
            parsed.envelope.sequence = MessageSequenceNumber{ReadU32(frame, offset)};
            parsed.envelope.acknowledgement = MessageAcknowledgementNumber{ReadU32(frame, offset)};
            parsed.payloadBytes = ReadU32(frame, offset);
            parsed.fieldCount = ReadU16(frame, offset);

            if (!ValidParsedIdentities(protocol, message, schema, parsed.envelope.schemaVersion))
                return Fail<ParsedHeader>(NetworkErrors::MessageEnvelopeInvalid);
            parsed.envelope.protocol = protocol.Value();
            parsed.envelope.message = message.Value();
            parsed.envelope.schema = schema.Value();
            return Result<ParsedHeader>::Success(std::move(parsed));
        }

        struct FieldHeader final {
            MessageEnvelopeFieldId id;
            MessageEnvelopeFieldRequirement requirement;
            std::size_t valueOffset{};
            std::size_t valueBytes{};
        };

        [[nodiscard]] Result<FieldHeader> ParseFieldHeader(const std::span<const std::byte> frame, std::size_t &offset,
                                                           const std::size_t headerBytes) {
            if (const auto headerEnd = CheckedSum(offset, FieldHeaderBytes); !headerEnd.has_value() || *headerEnd > headerBytes)
                return Fail<FieldHeader>(NetworkErrors::MessageEnvelopeInvalid);
            const auto id = MessageEnvelopeFieldId::Create(ReadU16(frame, offset));
            const auto requirement = static_cast<MessageEnvelopeFieldRequirement>(ReadU8(frame, offset));
            const std::uint8_t reserved = ReadU8(frame, offset);
            const std::size_t valueBytes = ReadU16(frame, offset);
            std::size_t valueEnd{};
            if (id.HasError() || requirement >= MessageEnvelopeFieldRequirement::Count || reserved != 0 ||
                !CheckedAdd(offset, valueBytes, valueEnd) || valueEnd > headerBytes)
                return Fail<FieldHeader>(NetworkErrors::MessageEnvelopeInvalid);
            const FieldHeader field{id.Value(), requirement, offset, valueBytes};
            offset = valueEnd;
            return Result<FieldHeader>::Success(field);
        }

        [[nodiscard]] Result<bool> ValidateEncodedField(const FieldHeader &field, const ParsedHeader &parsed,
                                                        const MessageCodecRegistry &registry) {
            const auto descriptor = registry.FindField(parsed.envelope.protocol, parsed.envelope.message, field.id);
            if (!descriptor.has_value()) {
                if (field.requirement == MessageEnvelopeFieldRequirement::Required)
                    return Fail<bool>(NetworkErrors::MessageEnvelopeUnknownRequiredField);
                return Result<bool>::Success(false);
            }
            if (field.valueBytes > descriptor->maximumEncodedBytes)
                return Fail<bool>(NetworkErrors::MessageEnvelopeCapacityExceeded);
            return Result<bool>::Success(true);
        }

        [[nodiscard]] Result<void> ValidateEncodedFieldRegion(const std::span<const std::byte> frame, const ParsedHeader &parsed,
                                                              const MessageEnvelopeLimits &limits) {
            if (parsed.headerBytes < FixedHeaderBytes || parsed.headerBytes > frame.size())
                return Fail<void>(NetworkErrors::MessageEnvelopeInvalid);
            if (parsed.headerBytes - FixedHeaderBytes > limits.maximumExtensionBytes || parsed.fieldCount > limits.maximumFields)
                return Fail<void>(NetworkErrors::MessageEnvelopeCapacityExceeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::size_t> ValidateEncodedFields(const std::span<const std::byte> frame, const ParsedHeader &parsed,
                                                                const MessageCodecRegistry &registry, const MessageEnvelopeLimits &limits) {
            if (const Result<void> region = ValidateEncodedFieldRegion(frame, parsed, limits); region.HasError())
                return Result<std::size_t>::Failure(region.ErrorValue());

            std::size_t offset = FixedHeaderBytes;
            std::size_t knownFields{};
            MessageEnvelopeFieldId previous;
            for (std::size_t index = 0; index < parsed.fieldCount; ++index) {
                const auto field = ParseFieldHeader(frame, offset, parsed.headerBytes);
                if (field.HasError())
                    return Result<std::size_t>::Failure(field.ErrorValue());
                if (previous.IsValid() && field.Value().id <= previous)
                    return Fail<std::size_t>(NetworkErrors::MessageEnvelopeInvalid);
                previous = field.Value().id;
                const Result<bool> known = ValidateEncodedField(field.Value(), parsed, registry);
                if (known.HasError())
                    return Result<std::size_t>::Failure(known.ErrorValue());
                knownFields += known.Value() ? 1U : 0U;
            }
            return offset == parsed.headerBytes ? Result<std::size_t>::Success(knownFields)
                                                : Fail<std::size_t>(NetworkErrors::MessageEnvelopeInvalid);
        }

        [[nodiscard]] Result<MessageEnvelope> CopyDecodedEnvelope(const std::span<const std::byte> frame, ParsedHeader parsed,
                                                                  const MessageCodecRegistry &registry, const std::size_t knownFields) {
            try {
                parsed.envelope.fields.reserve(knownFields);
                std::size_t offset = FixedHeaderBytes;
                for (std::size_t index = 0; index < parsed.fieldCount; ++index) {
                    const FieldHeader field = ParseFieldHeader(frame, offset, parsed.headerBytes).Value();
                    if (!registry.FindField(parsed.envelope.protocol, parsed.envelope.message, field.id).has_value())
                        continue;
                    const auto value = frame.subspan(field.valueOffset, field.valueBytes);
                    parsed.envelope.fields.emplace_back(field.id, field.requirement, std::vector<std::byte>{value.begin(), value.end()});
                }
                const auto payload = frame.subspan(parsed.headerBytes, parsed.payloadBytes);
                parsed.envelope.payload.assign(payload.begin(), payload.end());
                return Result<MessageEnvelope>::Success(std::move(parsed.envelope));
            } catch (const std::bad_alloc &) {
                return Fail<MessageEnvelope>(NetworkErrors::MessageEnvelopeCapacityExceeded);
            }
        }

        [[nodiscard]] Result<MessageCodecDescriptor> ValidateDecodedCodec(const ParsedHeader &parsed, const MessageCodecRegistry &registry,
                                                                          const MessageEnvelopeLimits &limits) {
            const auto codec = registry.FindCodec(parsed.envelope.protocol, parsed.envelope.message);
            if (!codec.has_value())
                return Fail<MessageCodecDescriptor>(NetworkErrors::MessageCodecUnknown);
            if (codec->schema != parsed.envelope.schema || !codec->supportedVersions.Contains(parsed.envelope.schemaVersion))
                return Fail<MessageCodecDescriptor>(NetworkErrors::MessageSchemaIncompatible);
            if (parsed.payloadBytes > limits.maximumPayloadBytes || parsed.payloadBytes > codec->maximumPayloadBytes)
                return Fail<MessageCodecDescriptor>(NetworkErrors::MessageEnvelopeCapacityExceeded);
            return Result<MessageCodecDescriptor>::Success(*codec);
        }

        /** @brief Shares the complete framing validation between borrowed and owned decode paths. */
        struct ValidatedFrame final {
            ParsedHeader parsed;
            std::size_t knownFields{};
        };

        [[nodiscard]] Result<ValidatedFrame> ValidateFrame(const std::span<const std::byte> frame, const MessageCodecRegistry &registry,
                                                           const MessageEnvelopeLimits &limits, const MessageEnvelopeAdmissionState state) {
            if (const ErrorCodeDescriptor *admission = AdmissionError(state); admission != nullptr)
                return Fail<ValidatedFrame>(*admission);
            auto parsed = ParseHeader(frame, limits);
            if (parsed.HasError())
                return Result<ValidatedFrame>::Failure(parsed.ErrorValue());
            if (const auto totalBytes = CheckedSum(parsed.Value().headerBytes, parsed.Value().payloadBytes);
                !totalBytes.has_value() || *totalBytes != frame.size())
                return Fail<ValidatedFrame>(NetworkErrors::MessageEnvelopeInvalid);
            if (const auto codec = ValidateDecodedCodec(parsed.Value(), registry, limits); codec.HasError())
                return Result<ValidatedFrame>::Failure(codec.ErrorValue());
            const auto known = ValidateEncodedFields(frame, parsed.Value(), registry, limits);
            if (known.HasError())
                return Result<ValidatedFrame>::Failure(known.ErrorValue());
            return Result<ValidatedFrame>::Success({std::move(parsed).Value(), known.Value()});
        }

    }  // namespace

    /** @copydoc MessageCodecRegistry::Create */
    Result<MessageCodecRegistry> MessageCodecRegistry::Create(const MessageCodecContributions &contributions,
                                                              const ProtocolIdentityRegistry &identities,
                                                              const MessageCodecRegistryLimits &limits) {
        if (!ValidRegistryLimits(limits))
            return Fail<MessageCodecRegistry>(NetworkErrors::MessageEnvelopeInvalid);
        if (!WithinRegistryCapacity(contributions, limits))
            return Fail<MessageCodecRegistry>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        try {
            MessageCodecRegistry result;
            result.codecs_.assign(contributions.codecs.begin(), contributions.codecs.end());
            result.fields_.assign(contributions.fields.begin(), contributions.fields.end());
            std::ranges::sort(result.codecs_, {}, CodecKey);
            std::ranges::sort(result.fields_, {}, FieldKey);
            if (std::ranges::adjacent_find(result.codecs_, {}, CodecKey) != result.codecs_.end() ||
                std::ranges::adjacent_find(result.fields_, {}, FieldKey) != result.fields_.end())
                return Fail<MessageCodecRegistry>(NetworkErrors::MessageCodecConflict);
            if (!ValidCodecs(result.codecs_, identities, limits) || !ValidFields(result.fields_, result.codecs_, limits))
                return Fail<MessageCodecRegistry>(NetworkErrors::MessageEnvelopeInvalid);
            return Result<MessageCodecRegistry>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Fail<MessageCodecRegistry>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        }
    }

    /** @copydoc MessageCodecRegistry::FindCodec */
    std::optional<MessageCodecDescriptor> MessageCodecRegistry::FindCodec(const ProtocolId protocol,
                                                                          const MessageTypeId message) const noexcept {
        const auto key = std::tuple{protocol.Value(), message.Value()};
        const auto found = std::ranges::lower_bound(codecs_, key, {}, CodecKey);
        return found == codecs_.end() || CodecKey(*found) != key ? std::nullopt : std::optional{*found};
    }

    /** @copydoc MessageCodecRegistry::FindField */
    std::optional<MessageEnvelopeFieldDescriptor> MessageCodecRegistry::FindField(const ProtocolId protocol, const MessageTypeId message,
                                                                                  const MessageEnvelopeFieldId field) const noexcept {
        const auto key = std::tuple{protocol.Value(), message.Value(), field.Value()};
        const auto found = std::ranges::lower_bound(fields_, key, {}, FieldKey);
        return found == fields_.end() || FieldKey(*found) != key ? std::nullopt : std::optional{*found};
    }

    /** @copydoc EncodeMessageEnvelope */
    Result<std::vector<std::byte>> EncodeMessageEnvelope(const MessageEnvelope &envelope, const MessageCodecRegistry &registry,
                                                         const MessageEnvelopeLimits &limits, const MessageEnvelopeAdmissionState state) {
        if (const ErrorCodeDescriptor *admission = AdmissionError(state); admission != nullptr)
            return Fail<std::vector<std::byte>>(*admission);
        if (!ValidEnvelopeLimits(limits))
            return Fail<std::vector<std::byte>>(NetworkErrors::MessageEnvelopeInvalid);
        const Result<MessageCodecDescriptor> codec = ValidateEnvelopeIdentity(envelope, registry);
        if (codec.HasError())
            return Result<std::vector<std::byte>>::Failure(codec.ErrorValue());
        Result<EncodingPlan> plan = BuildEncodingPlan(envelope, registry, codec.Value(), limits);
        if (plan.HasError())
            return Result<std::vector<std::byte>>::Failure(plan.ErrorValue());

        try {
            std::vector<std::byte> output;
            output.reserve(plan.Value().totalBytes);
            WriteFixedHeader(output, envelope, plan.Value().headerBytes, envelope.payload.size(), plan.Value().fields.size());
            for (const MessageEnvelopeField *field : plan.Value().fields) {
                WriteU16(output, field->id.Value());
                WriteU8(output, static_cast<std::uint8_t>(field->requirement));
                WriteU8(output, 0);
                WriteU16(output, static_cast<std::uint16_t>(field->value.size()));
                output.insert(output.end(), field->value.begin(), field->value.end());
            }
            output.insert(output.end(), envelope.payload.begin(), envelope.payload.end());
            return Result<std::vector<std::byte>>::Success(std::move(output));
        } catch (const std::bad_alloc &) {
            return Fail<std::vector<std::byte>>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        }
    }

    /** @copydoc EncodeMessageEnvelopePayloadInto */
    Result<std::size_t> EncodeMessageEnvelopePayloadInto(const MessageEnvelopePayloadView &envelope, const MessageCodecRegistry &registry,
                                                         const std::span<std::byte> output, const MessageEnvelopeLimits &limits) {
        if (!ValidEnvelopeLimits(limits) || envelope.fieldCount != 0)
            return Fail<std::size_t>(NetworkErrors::MessageEnvelopeInvalid);
        ParsedHeader parsed;
        parsed.envelope.protocol = envelope.protocol;
        parsed.envelope.message = envelope.message;
        parsed.envelope.schema = envelope.schema;
        parsed.envelope.schemaVersion = envelope.schemaVersion;
        parsed.payloadBytes = envelope.payload.size();
        if (const auto codec = ValidateDecodedCodec(parsed, registry, limits); codec.HasError())
            return Result<std::size_t>::Failure(codec.ErrorValue());
        const auto total = CheckedSum(FixedHeaderBytes, envelope.payload.size());
        if (!total || *total > limits.maximumFrameBytes || *total > output.size() ||
            envelope.payload.size() > std::numeric_limits<std::uint32_t>::max())
            return Fail<std::size_t>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        BoundedOutput writer{output};
        WriteFixedHeader(writer, envelope, FixedHeaderBytes, envelope.payload.size(), 0);
        std::ranges::copy(envelope.payload, output.begin() + static_cast<std::ptrdiff_t>(writer.offset));
        return Result<std::size_t>::Success(*total);
    }

    /** @copydoc InspectMessageEnvelope */
    Result<MessageEnvelopePayloadView> InspectMessageEnvelope(const std::span<const std::byte> frame, const MessageCodecRegistry &registry,
                                                              const MessageEnvelopeLimits &limits,
                                                              const MessageEnvelopeAdmissionState state) {
        auto validated = ValidateFrame(frame, registry, limits, state);
        if (validated.HasError())
            return Result<MessageEnvelopePayloadView>::Failure(validated.ErrorValue());
        const auto &parsed = validated.Value().parsed;
        const auto &header = parsed.envelope;
        return Result<MessageEnvelopePayloadView>::Success({header.protocol, header.message, header.schema, header.schemaVersion,
                                                            header.sequence, header.acknowledgement, parsed.fieldCount,
                                                            frame.subspan(parsed.headerBytes, parsed.payloadBytes)});
    }

    /** @copydoc DecodeMessageEnvelope */
    Result<MessageEnvelope> DecodeMessageEnvelope(const std::span<const std::byte> frame, const MessageCodecRegistry &registry,
                                                  const MessageEnvelopeLimits &limits, const MessageEnvelopeAdmissionState state) {
        auto validated = ValidateFrame(frame, registry, limits, state);
        if (validated.HasError())
            return Result<MessageEnvelope>::Failure(validated.ErrorValue());
        auto owned = std::move(validated).Value();
        return CopyDecodedEnvelope(frame, std::move(owned.parsed), registry, owned.knownFields);
    }
}  // namespace Horo::Network
