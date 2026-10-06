#include "ReplicationStateCodecInternal.h"

#include <new>

namespace Horo::Network {
    using namespace StateCodecDetail;

    /** @copydoc ReplicationStateCodec::ContinueEncoding */
    Result<void> ReplicationStateCodec::ContinueEncoding(const ReplicationCapturedStatePin &source,
                                                         const ReplicationAcknowledgedBaseline *baseline,
                                                         const CancellationToken &cancellation) const {
        if (const auto admitted = Admit(cancellation); admitted.HasError())
            return admitted;
        if (!CurrentSource(source) || (baseline && !UsableBaseline(source, *baseline)))
            return Fail<void>(ReplicationStateErrors::Stale);
        return Result<void>::Success();
    }

    /** @copydoc ReplicationStateCodec::EncodeField */
    Result<std::optional<ReplicationEncodedValue>> ReplicationStateCodec::EncodeField(const FieldId field,
                                                                                      const ReplicationCapturedStatePin &source,
                                                                                      const ReplicationAcknowledgedBaseline *baseline,
                                                                                      const CancellationToken &cancellation) const {
        const auto *value = Find(source->Fields(), field);
        const auto *previous = baseline ? Find(baseline->state->Fields(), field) : nullptr;
        if (!value || (baseline && !previous))
            return Fail<std::optional<ReplicationEncodedValue>>(ReplicationStateErrors::Invalid);
        if (previous) {
            const auto equal = serializers_->CanonicallyEqual(recipient_.schema, field, previous->value, value->value);
            if (equal.HasError())
                return Result<std::optional<ReplicationEncodedValue>>::Failure(equal.ErrorValue());
            if (const auto current = ContinueEncoding(source, baseline, cancellation); current.HasError())
                return Result<std::optional<ReplicationEncodedValue>>::Failure(current.ErrorValue());
            if (equal.Value())
                return Result<std::optional<ReplicationEncodedValue>>::Success(std::nullopt);
        }
        auto encoded = serializers_->Encode(recipient_.schema, field, value->value);
        if (encoded.HasError())
            return Result<std::optional<ReplicationEncodedValue>>::Failure(encoded.ErrorValue());
        if (const auto current = ContinueEncoding(source, baseline, cancellation); current.HasError())
            return Result<std::optional<ReplicationEncodedValue>>::Failure(current.ErrorValue());
        return Result<std::optional<ReplicationEncodedValue>>::Success(std::move(encoded).Value());
    }

    /** @copydoc ReplicationStateCodec::AppendHeader */
    void ReplicationStateCodec::AppendHeader(std::vector<std::byte> &wire, const ReplicationCapturedStatePin &source,
                                             const ReplicationAcknowledgedBaseline *baseline) const {
        Append(wire, Magic, 4);
        Append(wire, 1, 1);
        Append(wire, baseline ? 1 : 0, 1);
        Append(wire, 0, 2);
        AppendIdentity(wire, recipient_, generation_);
        Append(wire, source->SimulationTick(), 8);
        Append(wire, source->PublicationRevision(), 8);
        Append(wire, baseline ? baseline->state->SimulationTick() : 0, 8);
        Append(wire, baseline ? baseline->publicationRevision : 0, 8);
        AppendDigest(wire, serializers_->Schemas()->Fingerprint());
        AppendDigest(wire, fingerprint_);
        Append(wire, 0, 4);
    }

    /** @copydoc ReplicationStateCodec::Encode */
    Result<std::vector<std::byte>> ReplicationStateCodec::Encode(ReplicationCapturedStatePin source,
                                                                 const ReplicationAcknowledgedBaseline &baseline,
                                                                 const CancellationToken &cancellation) {
        if (const auto admitted = Admit(cancellation); admitted.HasError())
            return Result<std::vector<std::byte>>::Failure(admitted.ErrorValue());
        if (operating_)
            return Fail<std::vector<std::byte>>(ReplicationStateErrors::Invalid);
        if (!CurrentSource(source))
            return Fail<std::vector<std::byte>>(ReplicationStateErrors::Stale);
        const ReplicationAcknowledgedBaseline acknowledged = baseline;  // Pins callback lifetime independently of the caller's borrow.
        OperationGuard guard{operating_};
        const bool delta = UsableBaseline(source, acknowledged);
        try {
            std::vector<std::byte> wire;
            wire.reserve(wireCapacity_);
            AppendHeader(wire, source, delta ? &acknowledged : nullptr);
            std::uint32_t count{};
            for (const auto field : projection_) {
                const auto encoded = EncodeField(field, source, delta ? &acknowledged : nullptr, cancellation);
                if (encoded.HasError())
                    return Result<std::vector<std::byte>>::Failure(encoded.ErrorValue());
                if (!encoded.Value())
                    continue;
                const auto &bytes = encoded.Value()->canonicalBytes;
                if (const auto remaining = limits_.maximumWireBytes - wire.size();
                    remaining < FieldHeaderBytes || bytes.size() > remaining - FieldHeaderBytes)
                    return Fail<std::vector<std::byte>>(ReplicationStateErrors::Capacity);
                Append(wire, field.Value(), 4);
                Append(wire, encoded.Value()->valueType.Value(), 4);
                Append(wire, encoded.Value()->codec.Value(), 4);
                Append(wire, bytes.size(), 4);
                wire.insert(wire.end(), bytes.begin(), bytes.end());
                ++count;
            }
            for (std::size_t index{}; index < 4; ++index)
                wire[HeaderBytes - 4 + index] = static_cast<std::byte>((count >> (index * 8)) & 0xff);
            return Result<std::vector<std::byte>>::Success(std::move(wire));
        } catch (const std::bad_alloc &) {
            return Fail<std::vector<std::byte>>(ReplicationStateErrors::Capacity);
        } catch (...) {
            // Module codecs may throw non-standard values; no exception can escape the session owner boundary.
            return Fail<std::vector<std::byte>>(ReplicationStateErrors::CallbackFault);
        }
    }
}  // namespace Horo::Network
