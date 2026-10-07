#include "ReplicationStateCodecInternal.h"

namespace Horo::Network {
    using namespace StateCodecDetail;

    namespace {
        /** @brief Header transaction identity validated before any field work. */
        struct Header final {
            std::uint64_t delta{};
            std::uint64_t tick{};
            std::uint64_t revision{};
            std::uint64_t baselineTick{};
            std::uint64_t baselineRevision{};
            std::uint64_t count{};
        };

        /** @brief Parses closed version-one framing and exact negotiated identity. */
        Result<Header> ReadHeader(Reader &reader, const ReplicationRoleBinding &recipient, const std::uint64_t generation,
                                  const Sha256Digest &schemas, const Sha256Digest &projection) {
            Header header;
            if (!reader.Match(Magic, 4) || !reader.Match(1, 1) || !reader.Read(1, header.delta) || header.delta > 1 || !reader.Match(0, 2))
                return Fail<Header>(ReplicationStateErrors::Invalid);
            if (!MatchIdentity(reader, recipient, generation))
                return Fail<Header>(ReplicationStateErrors::Stale);
            if (!reader.Read(8, header.tick) || !reader.Read(8, header.revision) || !reader.Read(8, header.baselineTick) ||
                !reader.Read(8, header.baselineRevision) || header.tick == 0 || header.revision == 0)
                return Fail<Header>(ReplicationStateErrors::Invalid);
            if (!MatchDigest(reader, schemas) || !MatchDigest(reader, projection))
                return Fail<Header>(ReplicationStateErrors::Stale);
            if (!reader.Read(4, header.count))
                return Fail<Header>(ReplicationStateErrors::Invalid);
            return Result<Header>::Success(header);
        }

        /** @brief Complete allocation-free field framing pass; malformed suffixes cannot invoke earlier codecs. */
        Result<void> ValidateFields(Reader reader, const std::uint64_t count, const std::span<const FieldId> projection,
                                    const ReplicationSchemaDescriptor &schema) {
            std::uint64_t previous{};
            for (std::uint64_t index{}; index < count; ++index) {
                std::uint64_t id{};
                std::uint64_t type{};
                std::uint64_t codec{};
                std::uint64_t length{};
                if (!reader.Read(4, id) || !reader.Read(4, type) || !reader.Read(4, codec) || !reader.Read(4, length) || id <= previous)
                    return Fail<void>(ReplicationStateErrors::Invalid);
                previous = id;
                const auto field = std::ranges::lower_bound(schema.fields, id, {}, [](const auto &item) {
                    return item.id.Value();
                });
                if (field == schema.fields.end() || field->id.Value() != id || !std::ranges::binary_search(projection, field->id) ||
                    field->valueType.Value() != type || field->codec.Value() != codec || length > field->limits.maximumEncodedBytes ||
                    !reader.Skip(static_cast<std::size_t>(length)))
                    return Fail<void>(ReplicationStateErrors::Invalid);
            }
            return reader.offset == reader.bytes.size() ? Result<void>::Success() : Fail<void>(ReplicationStateErrors::Invalid);
        }

        /** @brief Extracts already-validated tagged bytes for one bounded codec call. */
        std::pair<FieldId, ReplicationEncodedValue> ReadField(Reader &reader) {
            std::uint64_t id{};
            std::uint64_t type{};
            std::uint64_t codec{};
            std::uint64_t length{};
            static_cast<void>(reader.Read(4, id));
            static_cast<void>(reader.Read(4, type));
            static_cast<void>(reader.Read(4, codec));
            static_cast<void>(reader.Read(4, length));
            ReplicationEncodedValue value{ReplicationValueTypeId::Create(static_cast<std::uint32_t>(type)).Value(),
                                          ReplicationCodecId::Create(static_cast<std::uint32_t>(codec)).Value(),
                                          {}};
            const auto bytes = reader.bytes.subspan(reader.offset, static_cast<std::size_t>(length));
            value.canonicalBytes.assign(bytes.begin(), bytes.end());
            static_cast<void>(reader.Skip(static_cast<std::size_t>(length)));
            return {FieldId::Create(static_cast<std::uint32_t>(id)).Value(), std::move(value)};
        }
    }  // namespace

    /** @copydoc ReplicationStateCodec::DecodeField */
    Result<ReplicationRuntimeValue> ReplicationStateCodec::DecodeField(const FieldId field, const ReplicationEncodedValue &encoded,
                                                                       const CancellationToken &cancellation) const {
        auto value = serializers_->Decode(recipient_.schema, field, encoded);
        if (value.HasError())
            return value;
        if (const auto admitted = Admit(cancellation); admitted.HasError())
            return Result<ReplicationRuntimeValue>::Failure(admitted.ErrorValue());
        const auto canonical = serializers_->Encode(recipient_.schema, field, value.Value());
        if (canonical.HasError())
            return Result<ReplicationRuntimeValue>::Failure(canonical.ErrorValue());
        if (const auto admitted = Admit(cancellation); admitted.HasError())
            return Result<ReplicationRuntimeValue>::Failure(admitted.ErrorValue());
        return canonical.Value() == encoded ? std::move(value) : Fail<ReplicationRuntimeValue>(ReplicationStateErrors::Invalid);
    }

    /** @copydoc ReplicationStateCodec::Reconstruct */
    Result<std::vector<ReplicationCapturedField>> ReplicationStateCodec::Reconstruct(const std::span<const std::byte> fieldBytes,
                                                                                     const std::uint64_t count,
                                                                                     const ReplicationDecodedState *baseline,
                                                                                     const CancellationToken &cancellation) const {
        std::vector<ReplicationCapturedField> fields;
        if (baseline)
            fields = baseline->fields_;
        else
            fields.reserve(projection_.size());
        Reader reader{fieldBytes};
        for (std::uint64_t index{}; index < count; ++index) {
            auto [field, encoded] = ReadField(reader);
            auto value = DecodeField(field, encoded, cancellation);
            if (value.HasError())
                return Result<std::vector<ReplicationCapturedField>>::Failure(value.ErrorValue());
            if (baseline) {
                const auto found = std::ranges::lower_bound(fields, field, {}, &ReplicationCapturedField::field);
                if (found == fields.end() || found->field != field)
                    return Fail<std::vector<ReplicationCapturedField>>(ReplicationStateErrors::Stale);
                found->value = std::move(value).Value();
            } else {
                fields.emplace_back(field, std::move(value).Value());
            }
        }
        return Result<std::vector<ReplicationCapturedField>>::Success(std::move(fields));
    }

    /** @copydoc ReplicationStateCodec::Decode */
    Result<ReplicationDecodedState> ReplicationStateCodec::Decode(const std::span<const std::byte> wire,
                                                                  const NetworkObjectMappingEntry &expected,
                                                                  const ReplicationDecodedState *baseline,
                                                                  const CancellationToken &cancellation) {
        if (const auto admitted = Admit(cancellation); admitted.HasError())
            return Result<ReplicationDecodedState>::Failure(admitted.ErrorValue());
        if (operating_)
            return Fail<ReplicationDecodedState>(ReplicationStateErrors::Invalid);
        if (wire.size() > limits_.maximumWireBytes)
            return Fail<ReplicationDecodedState>(ReplicationStateErrors::Capacity);
        if (wire.size() < HeaderBytes)
            return Fail<ReplicationDecodedState>(ReplicationStateErrors::Invalid);
        if (expected.object != recipient_.object || expected.provenance.schema != recipient_.schema ||
            expected.provenance.schemaVersion != recipient_.schemaVersion || !expected.entity.IsValid())
            return Fail<ReplicationDecodedState>(ReplicationStateErrors::Stale);
        Reader reader{wire};
        const auto parsed = ReadHeader(reader, recipient_, generation_, serializers_->Schemas()->Fingerprint(), fingerprint_);
        if (parsed.HasError())
            return Result<ReplicationDecodedState>::Failure(parsed.ErrorValue());
        const auto &header = parsed.Value();
        if (header.count > projection_.size() || (!header.delta && header.count != projection_.size()))
            return Fail<ReplicationDecodedState>(ReplicationStateErrors::Invalid);
        if (header.delta) {
            if (!baseline || !baseline->IsCurrent() || baseline->admission_ != admission_ || baseline->serializers_ != serializers_ ||
                baseline->mapping_ != expected || baseline->schema_ != recipient_.schema ||
                baseline->version_ != recipient_.schemaVersion || baseline->tick_ != header.baselineTick ||
                baseline->revision_ != header.baselineRevision || header.baselineTick > header.tick ||
                header.baselineRevision > header.revision)
                return Fail<ReplicationDecodedState>(ReplicationStateErrors::Stale);
            if (header.revision == header.baselineRevision && (header.tick != header.baselineTick || header.count != 0))
                return Fail<ReplicationDecodedState>(ReplicationStateErrors::Invalid);
        } else if (header.baselineTick != 0 || header.baselineRevision != 0) {
            return Fail<ReplicationDecodedState>(ReplicationStateErrors::Invalid);
        }
        const auto schema = serializers_->Schemas()->Find(recipient_.schema);
        if (const auto framed = ValidateFields(reader, header.count, projection_, *schema.Value()); framed.HasError())
            return Result<ReplicationDecodedState>::Failure(framed.ErrorValue());
        ReplicationDecodedState state;
        state.object_ = expected.object;
        state.mapping_ = expected;
        state.schema_ = recipient_.schema;
        state.version_ = recipient_.schemaVersion;
        state.tick_ = header.tick;
        state.revision_ = header.revision;
        state.serializers_ = serializers_;
        state.admission_ = admission_;
        return CompleteState(std::move(state), wire.subspan(reader.offset), header.count, header.delta ? baseline : nullptr, cancellation);
    }

    /** @copydoc ReplicationStateCodec::CompleteState */
    Result<ReplicationDecodedState> ReplicationStateCodec::CompleteState(ReplicationDecodedState state,
                                                                         const std::span<const std::byte> fields, const std::uint64_t count,
                                                                         const ReplicationDecodedState *baseline,
                                                                         const CancellationToken &cancellation) {
        OperationGuard guard{operating_};
        std::optional<Result<ReplicationDecodedState>> result;
        const auto fault = ContainCodecFault([this, &result, &state, fields, count, baseline, &cancellation] {
            result.emplace([this, &state, fields, count, baseline, &cancellation] {
                // Foreign callbacks cannot invalidate the already-validated suffix by mutating the caller's borrowed record.
                const std::vector<std::byte> fieldBytes{fields.begin(), fields.end()};
                auto reconstructed = Reconstruct(fieldBytes, count, baseline, cancellation);
                if (reconstructed.HasError())
                    return Result<ReplicationDecodedState>::Failure(reconstructed.ErrorValue());
                state.fields_ = std::move(reconstructed).Value();
                return Result<ReplicationDecodedState>::Success(std::move(state));
            }());
        });
        if (fault == CodecFault::None)
            return std::move(*result);
        return Fail<ReplicationDecodedState>(fault == CodecFault::Capacity ? ReplicationStateErrors::Capacity
                                                                           : ReplicationStateErrors::CallbackFault);
    }
}  // namespace Horo::Network
