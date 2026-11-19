#include "Horo/Network/NetworkInitialStateStream.h"

#include "Horo/Network/NetworkErrors.h"
#include "NetworkAdmissionWire.h"
#include "ReplicationStateCodecInternal.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Network {
    namespace {
        using AdmissionWire::Reader;
        using AdmissionWire::Writer;
        constexpr std::uint16_t Version = 1;
        constexpr std::size_t ScopeBytes = 28;
        constexpr std::size_t ManifestBytes = ScopeBytes + 6;
        constexpr std::size_t MaximumRecords = ReplicationWorldLimits{}.maximumObjects;
        constexpr std::size_t MinimumRecordBytes = StateCodecDetail::HeaderBytes;
        constexpr std::size_t MaximumRecordBytes = 16 * 1024 * 1024;
        constexpr std::size_t MaximumSnapshotBytes = 64 * 1024 * 1024;
        static_assert(NetworkInitialStateSegmentHeaderBytes == ScopeBytes + 20);

        enum class FrameKind : std::uint16_t {
            Manifest = 1,
            Segment = 2
        };

        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &error = NetworkErrors::MessageEnvelopeInvalid) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Writes the scope shared by the manifest and every segment. */
        bool WriteScope(Writer &writer, FrameKind kind, const ReplicationInitialStateSnapshot &snapshot) {
            return writer.Number(Version, 2) && writer.Number(static_cast<std::uint16_t>(kind), 2) &&
                   writer.Number(snapshot.session.Value(), 8) && writer.Number(snapshot.authority.Value(), 8) &&
                   writer.Number(snapshot.tick, 8);
        }

        /** @brief Checks an encoded record's scope and payload budget without decoding its fields. */
        bool ValidRecord(const ReplicationInitialStateSnapshot &snapshot, const ReplicationInitialStateRecord &record) {
            return record.object.IsValid() && record.object.Epoch() == snapshot.authority && record.wire.size() >= MinimumRecordBytes &&
                   record.wire.size() <= MaximumRecordBytes;
        }

        /** @brief Admits the exact sorted object inventory and computes the non-overflowing wire total. */
        Result<std::size_t> InventoryBytes(const ReplicationInitialStateSnapshot &snapshot) {
            std::size_t total{};
            NetworkObjectId previous;
            for (const auto &record : snapshot.records) {
                if (!ValidRecord(snapshot, record) || (previous.IsValid() && previous >= record.object))
                    return Reject<std::size_t>();
                if (record.wire.size() > MaximumSnapshotBytes - total)
                    return Reject<std::size_t>(NetworkErrors::MessageEnvelopeCapacityExceeded);
                previous = record.object;
                total += record.wire.size();
            }
            return Result<std::size_t>::Success(total);
        }

        /** @brief Limits scope validity before reading per-record fields. */
        bool ValidScope(const ReplicationInitialStateSnapshot &snapshot) {
            return snapshot.session.IsValid() && snapshot.authority.IsValid() && snapshot.tick != 0;
        }

        /** @brief Verifies the aggregate size can hold exactly the given number of records. */
        bool ValidRecordBudget(std::uint64_t count, std::uint64_t bytes) {
            return bytes >= count * MinimumRecordBytes && bytes <= count * MaximumRecordBytes;
        }

        /** @brief Frame metadata and payload borrowed for the duration of admission. */
        struct FrameView final {
            FrameKind kind{};
            std::uint64_t session{};
            std::uint64_t epoch{};
            std::uint64_t tick{};
            std::span<const std::byte> body;
        };

        /** @brief Admits a versioned manifest or segment up to the Control frame ceiling. */
        Result<FrameView> ReadFrame(std::span<const std::byte> payload) {
            if (payload.size() > MaximumNetworkInitialStateFrameBytes)
                return Reject<FrameView>(NetworkErrors::MessageEnvelopeCapacityExceeded);
            Reader reader{payload};
            std::uint16_t version{};
            FrameView frame;
            if (!reader.Number(version, 2) || version != Version || !reader.Number(frame.kind, 2) || !reader.Number(frame.session, 8) ||
                !reader.Number(frame.epoch, 8) || !reader.Number(frame.tick, 8))
                return Reject<FrameView>();
            if (frame.tick == 0 || (frame.kind != FrameKind::Manifest && frame.kind != FrameKind::Segment))
                return Reject<FrameView>();
            frame.body = payload.subspan(ScopeBytes);
            return Result<FrameView>::Success(frame);
        }

        /** @brief Matches only the negotiated raw-payload route and its exact schema version. */
        bool MatchesControl(const MessageEnvelope &message, const MessageIdentityDescriptor &route) {
            return message.fields.empty() && message.protocol == route.protocol && message.message == route.id &&
                   message.schema == route.schema && message.schemaVersion == route.version;
        }

        /** @brief Snapshot chunk that borrows only the admitted envelope payload. */
        struct SegmentView final {
            NetworkObjectId object;
            std::size_t total{};
            std::size_t offset{};
            std::span<const std::byte> bytes;
        };

        /** @brief Rejects empty, truncated and overflowing slices before any assembly allocation. */
        Result<SegmentView> ReadSegment(std::span<const std::byte> body, ReplicationAuthorityEpoch epoch) {
            Reader reader{body};
            std::uint64_t slot{};
            std::uint32_t generation{}, total{}, offset{};
            if (!reader.Number(slot, 8) || !reader.Number(generation, 4) || !reader.Number(total, 4) || !reader.Number(offset, 4))
                return Reject<SegmentView>();
            const auto bytes = body.subspan(NetworkInitialStateSegmentHeaderBytes - ScopeBytes);
            if (total < MinimumRecordBytes || total > MaximumRecordBytes || offset > total || bytes.empty() ||
                bytes.size() > total - offset)
                return Reject<SegmentView>();
            auto object = NetworkObjectId::Create(epoch, slot, generation);
            if (object.HasError())
                return Reject<SegmentView>();
            return Result<SegmentView>::Success({object.Value(), total, offset, bytes});
        }
    }  // namespace

    /** @copydoc EncodeNetworkInitialStateManifest */
    Result<std::vector<std::byte>> EncodeNetworkInitialStateManifest(const ReplicationInitialStateSnapshot &snapshot) {
        if (!ValidScope(snapshot))
            return Reject<std::vector<std::byte>>();
        if (snapshot.records.size() > MaximumRecords)
            return Reject<std::vector<std::byte>>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        const auto total = InventoryBytes(snapshot);
        if (total.HasError())
            return Result<std::vector<std::byte>>::Failure(total.ErrorValue());
        try {
            std::vector<std::byte> bytes(ManifestBytes);
            Writer writer{bytes};
            if (!WriteScope(writer, FrameKind::Manifest, snapshot) || !writer.Number(snapshot.records.size(), 2) ||
                !writer.Number(total.Value(), 4))
                return Reject<std::vector<std::byte>>();
            return Result<std::vector<std::byte>>::Success(std::move(bytes));
        } catch (const std::bad_alloc &) {
            return Reject<std::vector<std::byte>>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        }
    }

    /** @copydoc EncodeNetworkInitialStateSegment */
    Result<NetworkInitialStateSegment> EncodeNetworkInitialStateSegment(const ReplicationInitialStateSnapshot &snapshot, std::size_t index,
                                                                        std::size_t offset, std::size_t maximumPayloadBytes) {
        if (!ValidScope(snapshot) || index >= snapshot.records.size())
            return Reject<NetworkInitialStateSegment>();
        if (maximumPayloadBytes <= NetworkInitialStateSegmentHeaderBytes || maximumPayloadBytes > MaximumNetworkInitialStateFrameBytes)
            return Reject<NetworkInitialStateSegment>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        const auto &record = snapshot.records[index];
        if (!ValidRecord(snapshot, record) || offset >= record.wire.size())
            return Reject<NetworkInitialStateSegment>();
        const auto count = std::min(maximumPayloadBytes - NetworkInitialStateSegmentHeaderBytes, record.wire.size() - offset);
        try {
            std::vector<std::byte> bytes(NetworkInitialStateSegmentHeaderBytes + count);
            Writer writer{bytes};
            if (!WriteScope(writer, FrameKind::Segment, snapshot) || !writer.Number(record.object.Slot(), 8) ||
                !writer.Number(record.object.Generation(), 4) || !writer.Number(record.wire.size(), 4) || !writer.Number(offset, 4) ||
                !writer.Bytes(std::span{record.wire}.subspan(offset, count)))
                return Reject<NetworkInitialStateSegment>();
            return Result<NetworkInitialStateSegment>::Success({std::move(bytes), offset + count});
        } catch (const std::bad_alloc &) {
            return Reject<NetworkInitialStateSegment>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        }
    }

    /** @copydoc NetworkInitialStateInbox::NetworkInitialStateInbox */
    NetworkInitialStateInbox::NetworkInitialStateInbox(ReplicationInboundAuthority authority, MessageIdentityDescriptor message,
                                                       std::size_t maximumWireBytes) noexcept
        : authority_(std::move(authority)), message_(message), maximumWireBytes_(maximumWireBytes) {}

    /** @copydoc NetworkInitialStateInbox::Handle */
    Result<void> NetworkInitialStateInbox::Handle(const MessageEnvelope &) {
        return Reject<void>(NetworkErrors::GameplayDispatchRejected);
    }

    /** @copydoc NetworkInitialStateInbox::Admit */
    Result<void> NetworkInitialStateInbox::Admit(const InboundMessageContext &context, const MessageEnvelope &message) const {
        if (std::this_thread::get_id() != owner_ || transferred_ || !authority_.session || context.cancellation.IsCancellationRequested())
            return Reject<void>(NetworkErrors::NetworkModeUnavailable);
        if (context.connection != authority_.connection || context.session != authority_.session ||
            context.generation != authority_.generation)
            return Reject<void>(NetworkErrors::NetworkModeStale);
        if (context.delivery != DeliveryPolicy::ReliableOrdered || !MatchesControl(message, message_) ||
            message_.protocol != authority_.protocol || message_.id != authority_.message)
            return Reject<void>(NetworkErrors::GameplayDispatchRejected);
        return authority_.session->AdmitGameplay(context.connection, context.generation, context.ownerTick);
    }

    /** @copydoc NetworkInitialStateInbox::HandleAdmitted */
    Result<void> NetworkInitialStateInbox::HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) {
        if (auto admitted = Admit(context, message); admitted.HasError())
            return admitted;
        if (delivery_.IsCancellationRequested())
            return Reject<void>(NetworkErrors::SessionCancelled);
        const auto decoded = ReadFrame(message.payload);
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        const auto &frame = decoded.Value();
        if (frame.session == 0 || frame.epoch == 0 || frame.session != authority_.networkSession.Value() ||
            frame.epoch != authority_.epoch.Value())
            return Reject<void>(NetworkErrors::ReplicationWorldStale);
        auto accepted = frame.kind == FrameKind::Manifest ? Begin(frame.tick, frame.body) : Append(frame.tick, frame.body);
        if (accepted.HasValue())
            delivery_ = context.cancellation;
        return accepted;
    }

    /** @copydoc NetworkInitialStateInbox::Begin */
    Result<void> NetworkInitialStateInbox::Begin(std::uint64_t tick, std::span<const std::byte> body) {
        Reader reader{body};
        std::uint16_t count{};
        std::uint32_t bytes{};
        if (begun_ || !reader.Number(count, 2) || !reader.Number(bytes, 4) || !reader.Done())
            return Reject<void>();
        if (count > MaximumRecords || bytes > std::min(maximumWireBytes_, MaximumSnapshotBytes))
            return Reject<void>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        if (!ValidRecordBudget(count, bytes))
            return Reject<void>();
        try {
            snapshot_.records.reserve(count);
            snapshot_.session = authority_.networkSession;
            snapshot_.authority = authority_.epoch;
            snapshot_.tick = tick;
            expectedRecords_ = count;
            remainingBytes_ = bytes;
            begun_ = true;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Reject<void>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        }
    }

    /** @copydoc NetworkInitialStateInbox::Append */
    Result<void> NetworkInitialStateInbox::Append(std::uint64_t tick, std::span<const std::byte> body) {
        if (!begun_ || Complete() || tick != snapshot_.tick)
            return Reject<void>();
        auto decoded = ReadSegment(body, snapshot_.authority);
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        const auto &segment = decoded.Value();
        if (pendingBytes_ == 0) {
            if (segment.offset != 0)
                return Reject<void>();
            if (auto started = StartRecord(segment.object, segment.total); started.HasError())
                return started;
        }
        auto &record = snapshot_.records.back();
        if (segment.object != record.object || segment.offset != record.wire.size() || segment.total != pendingBytes_)
            return Reject<void>();
        record.wire.insert(record.wire.end(), segment.bytes.begin(), segment.bytes.end());
        remainingBytes_ -= segment.bytes.size();
        if (record.wire.size() == pendingBytes_)
            pendingBytes_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc NetworkInitialStateInbox::StartRecord */
    Result<void> NetworkInitialStateInbox::StartRecord(NetworkObjectId object, std::size_t bytes) {
        if (snapshot_.records.size() >= expectedRecords_ || bytes > remainingBytes_)
            return Reject<void>();
        if (!snapshot_.records.empty() && snapshot_.records.back().object >= object)
            return Reject<void>();
        const auto remainingRecords = expectedRecords_ - snapshot_.records.size() - 1;
        if (!ValidRecordBudget(remainingRecords, remainingBytes_ - bytes))
            return Reject<void>();
        try {
            ReplicationInitialStateRecord record{object};
            record.wire.reserve(bytes);
            snapshot_.records.push_back(std::move(record));
            pendingBytes_ = bytes;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Reject<void>(NetworkErrors::MessageEnvelopeCapacityExceeded);
        }
    }

    /** @copydoc NetworkInitialStateInbox::Complete */
    bool NetworkInitialStateInbox::Complete() const noexcept {
        return begun_ && pendingBytes_ == 0 && remainingBytes_ == 0 && snapshot_.records.size() == expectedRecords_;
    }

    /** @copydoc NetworkInitialStateInbox::Take */
    Result<ReplicationInitialStateSnapshot> NetworkInitialStateInbox::Take(std::uint64_t now) {
        if (std::this_thread::get_id() != owner_ || transferred_ || !authority_.session)
            return Reject<ReplicationInitialStateSnapshot>(NetworkErrors::NetworkModeUnavailable);
        const auto admitted = authority_.session->AdmitGameplay(authority_.connection, authority_.generation, now);
        const bool cancelled = delivery_.IsCancellationRequested();
        if (cancelled || admitted.HasError()) {
            transferred_ = true;
            snapshot_ = {};
            if (cancelled)
                return Reject<ReplicationInitialStateSnapshot>(NetworkErrors::SessionCancelled);
            return Result<ReplicationInitialStateSnapshot>::Failure(admitted.ErrorValue());
        }
        if (!Complete())
            return Reject<ReplicationInitialStateSnapshot>(NetworkErrors::NetworkModeUnavailable);
        transferred_ = true;
        return Result<ReplicationInitialStateSnapshot>::Success(std::move(snapshot_));
    }
}  // namespace Horo::Network
