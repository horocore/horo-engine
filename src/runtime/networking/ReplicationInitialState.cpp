#include "Horo/Network/ReplicationStateCodec.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Network {
    namespace {
        using Snapshot = Result<ReplicationInitialStateSnapshot>;
        using Codecs = std::vector<std::shared_ptr<ReplicationStateCodec>>;
        using Sources = Result<std::vector<ReplicationCapturedStatePin>>;
        constexpr std::size_t MaximumInitialWireBytes = 64 * 1024 * 1024;

        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Matches an object's full projection to the same recipient and descriptor generation. */
        bool MatchesProjection(const ReplicationStateCodec &codec, const ReplicationStateCodec &first,
                               const NetworkObjectMappingEntry &object, NetworkSessionGeneration session) {
            const auto &recipient = codec.Recipient();
            const auto &initial = first.Recipient();
            return codec.RecordKind() == ReplicationRecordKind::Spawn && codec.DescriptorGeneration() == first.DescriptorGeneration() &&
                   recipient.session == session && recipient.object == object.object && recipient.localPeer == initial.localPeer &&
                   recipient.role == initial.role && recipient.schema == object.provenance.schema &&
                   recipient.schemaVersion == object.provenance.schemaVersion;
        }

        /** @brief Acquires all immutable source pins before invoking any serializer. */
        Sources PinSources(const ReplicationWorldReadLease &world, const ReplicationStateCapture &capture, std::uint64_t tick,
                           const Codecs &codecs) {
            std::vector<ReplicationCapturedStatePin> pins;
            pins.reserve(codecs.size());
            const auto entries = world.Mapping().Entries();
            for (std::size_t index = 0; index < codecs.size(); ++index) {
                const auto &row = entries[index];
                if (!MatchesProjection(*codecs[index], *codecs.front(), row, world.Descriptor().session))
                    return Fail<std::vector<ReplicationCapturedStatePin>>(ReplicationStateErrors::Stale);
                auto source = capture.Latest(row.object);
                if (source.HasError())
                    return Sources::Failure(source.ErrorValue());
                const auto &pin = source.Value();
                if (pin->SimulationTick() != tick || pin->World().MappingRevision() != world.MappingRevision() ||
                    pin->World().Descriptor() != world.Descriptor() || pin->Object() != row)
                    return Fail<std::vector<ReplicationCapturedStatePin>>(ReplicationStateErrors::Stale);
                pins.push_back(std::move(source).Value());
            }
            return Sources::Success(std::move(pins));
        }

        /** @brief Encodes privately and releases no partial records on codec or budget failure. */
        Snapshot EncodeSnapshot(const ReplicationWorldReadLease &world, std::uint64_t tick, const Codecs &codecs,
                                std::span<const ReplicationCapturedStatePin> sources, std::size_t remaining,
                                const CancellationToken &cancellation) {
            ReplicationInitialStateSnapshot snapshot{world.Descriptor().session, world.Descriptor().authority, tick};
            snapshot.records.reserve(sources.size());
            for (std::size_t index = 0; index < sources.size(); ++index) {
                auto record = codecs[index]->Encode(sources[index], {}, cancellation);
                if (record.HasError())
                    return Snapshot::Failure(record.ErrorValue());
                if (record.Value().size() > remaining)
                    return Fail<ReplicationInitialStateSnapshot>(ReplicationStateErrors::Capacity);
                remaining -= record.Value().size();
                snapshot.records.push_back({sources[index]->Object().object, std::move(record).Value()});
            }
            if (cancellation.IsCancellationRequested() || !world.HasCurrentMapping())
                return Fail<ReplicationInitialStateSnapshot>(ReplicationStateErrors::Stale);
            return Snapshot::Success(std::move(snapshot));
        }

        /** @brief Checks capture readiness and finite budgets before copying codec or source references. */
        Result<void> ValidateRequest(const ReplicationWorldReadLease &world, std::uint64_t tick, std::size_t codecs,
                                     std::size_t maximumWireBytes, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested() || !world.HasCurrentMapping())
                return Fail<void>(ReplicationStateErrors::Stale);
            if (!world.Capabilities().publishAuthority || tick == 0 || maximumWireBytes == 0 || maximumWireBytes > MaximumInitialWireBytes)
                return Fail<void>(ReplicationStateErrors::Invalid);
            if (codecs != world.Mapping().Entries().size() || codecs > ReplicationWorldLimits{}.maximumObjects)
                return Fail<void>(ReplicationStateErrors::Capacity);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EncodeReplicationInitialState */
    Result<ReplicationInitialStateSnapshot> EncodeReplicationInitialState(ReplicationWorldReadLease world,
                                                                          const ReplicationStateCapture &capture, std::uint64_t tick,
                                                                          std::span<const std::shared_ptr<ReplicationStateCodec>> codecs,
                                                                          std::size_t maximumWireBytes,
                                                                          const CancellationToken &cancellation) {
        if (auto valid = ValidateRequest(world, tick, codecs.size(), maximumWireBytes, cancellation); valid.HasError())
            return Snapshot::Failure(valid.ErrorValue());
        if (std::ranges::any_of(codecs, [](const auto &codec) {
            return !codec;
        }))
            return Fail<ReplicationInitialStateSnapshot>(ReplicationStateErrors::Invalid);
        try {
            Codecs ordered{codecs.begin(), codecs.end()};
            std::ranges::sort(ordered, {}, [](const auto &codec) {
                return codec->Recipient().object;
            });
            auto sources = PinSources(world, capture, tick, ordered);
            if (sources.HasError())
                return Snapshot::Failure(sources.ErrorValue());
            return EncodeSnapshot(world, tick, ordered, sources.Value(), maximumWireBytes, cancellation);
        } catch (const std::bad_alloc &) {
            return Fail<ReplicationInitialStateSnapshot>(ReplicationStateErrors::Capacity);
        }
    }
}  // namespace Horo::Network
