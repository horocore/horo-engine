#include "Horo/Network/ReplicationSnapshotHistory.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Preserves typed failure descriptors without exposing codec-private helpers. */
        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Computes a conservative bounded charge without overflowing size arithmetic. */
        std::optional<std::size_t> Charge(const ReplicationCapturedState &state, const std::size_t bound) noexcept {
            std::size_t bytes = sizeof(ReplicationCapturedState) + sizeof(ReplicationSnapshotAcknowledgement);
            if (bytes > bound || state.Fields().size() > (bound - bytes) / sizeof(ReplicationCapturedField))
                return std::nullopt;
            bytes += state.Fields().size() * sizeof(ReplicationCapturedField);
            for (const auto &field : state.Fields()) {
                std::size_t payload{};
                if (const auto *text = std::get_if<std::string>(&field.value))
                    payload = text->capacity();
                if (const auto *data = std::get_if<std::vector<std::byte>>(&field.value))
                    payload = data->capacity();
                if (payload > bound - bytes)
                    return std::nullopt;
                bytes += payload;
            }
            return bytes;
        }
    }  // namespace

    /** @copydoc ReplicationSnapshotHistory::Create */
    Result<std::unique_ptr<ReplicationSnapshotHistory>> ReplicationSnapshotHistory::Create(const ReplicationHistoryScope &scope,
                                                                                           const ReplicationHistoryLimits &limits) {
        if (!scope.connection.IsValid() || !scope.session.IsValid() || !scope.scene.IsValid() || scope.incarnation == 0 ||
            limits.maximumEntries == 0 || limits.maximumEntries > 4096 || limits.maximumRetainedBytes == 0 ||
            limits.maximumRetainedBytes > 64 * 1024 * 1024 || limits.leaseTicks == 0 ||
            (limits.overflow != ReplicationHistoryOverflow::FullSnapshot && limits.overflow != ReplicationHistoryOverflow::Disconnect))
            return Fail<std::unique_ptr<ReplicationSnapshotHistory>>(ReplicationStateErrors::Invalid);
        try {
            // make_unique cannot invoke the private constructor that fences validated factory creation.
            auto history =
                std::unique_ptr<ReplicationSnapshotHistory>(new ReplicationSnapshotHistory(scope, limits));  // NOSONAR(cpp:S5950)
            return Result<std::unique_ptr<ReplicationSnapshotHistory>>::Success(std::move(history));
        } catch (const std::bad_alloc &) {
            return Fail<std::unique_ptr<ReplicationSnapshotHistory>>(ReplicationStateErrors::Capacity);
        }
    }

    /** @copydoc ReplicationSnapshotHistory::ReplicationSnapshotHistory */
    ReplicationSnapshotHistory::ReplicationSnapshotHistory(const ReplicationHistoryScope &scope, const ReplicationHistoryLimits &limits)
        : scope_(scope), limits_(limits), owner_(std::this_thread::get_id()) {
        entries_.reserve(limits.maximumEntries);
    }

    /** @copydoc ReplicationSnapshotHistory::Current */
    bool ReplicationSnapshotHistory::Current(const ReplicationCapturedStatePin &state) const noexcept {
        return state && state->IsCurrent() && state->World().Descriptor().session == scope_.session &&
               state->World().Descriptor().scene == scope_.scene &&
               state->World().Descriptor().role == ReplicationExecutionRole::AuthorityServer;
    }

    /** @copydoc ReplicationSnapshotHistory::Advance */
    Result<void> ReplicationSnapshotHistory::Advance(const std::uint64_t now, const CancellationToken &cancellation) {
        if (owner_ != std::this_thread::get_id())
            return Fail<void>(ReplicationStateErrors::Invalid);
        if (closed_)
            return Fail<void>(ReplicationStateErrors::Closed);
        if (cancellation.IsCancellationRequested())
            return Fail<void>(NetworkErrors::ReplicationWorldCancelled);
        if (now < clock_)
            return Fail<void>(ReplicationStateErrors::Invalid);
        clock_ = now;
        std::erase_if(entries_, [this, now](const Entry &entry) {
            if (entry.expires > now && Current(entry.baseline.state))
                return false;
            bytes_ -= entry.bytes;
            return true;
        });
        return Result<void>::Success();
    }

    /** @copydoc ReplicationSnapshotHistory::RetainSent */
    Result<ReplicationSnapshotAcknowledgement> ReplicationSnapshotHistory::RetainSent(ReplicationAcknowledgedBaseline sent,
                                                                                      const std::uint64_t now,
                                                                                      const CancellationToken &cancellation) {
        if (const auto admitted = Advance(now, cancellation); admitted.HasError())
            return Result<ReplicationSnapshotAcknowledgement>::Failure(admitted.ErrorValue());
        if (!Current(sent.state) || sent.publicationRevision != sent.state->PublicationRevision() || !sent.roleRevision.IsValid() ||
            sent.descriptorGeneration == 0)
            return Fail<ReplicationSnapshotAcknowledgement>(ReplicationStateErrors::Stale);
        if (sequence_ == std::numeric_limits<std::uint64_t>::max() ||
            now > std::numeric_limits<std::uint64_t>::max() - limits_.leaseTicks) {
            Clear();
            closed_ = true;
            return Fail<ReplicationSnapshotAcknowledgement>(ReplicationStateErrors::Closed);
        }
        const auto charge = Charge(*sent.state, limits_.maximumRetainedBytes);
        if (!charge || entries_.size() == limits_.maximumEntries || *charge > limits_.maximumRetainedBytes - bytes_) {
            Clear();
            closed_ = limits_.overflow == ReplicationHistoryOverflow::Disconnect;
            return Fail<ReplicationSnapshotAcknowledgement>(ReplicationStateErrors::Capacity);
        }
        const ReplicationSnapshotAcknowledgement token{scope_, ++sequence_, sent.state->Object().object, sent.publicationRevision};
        entries_.emplace_back(token, std::move(sent), now + limits_.leaseTicks, *charge, false);
        bytes_ += *charge;
        return Result<ReplicationSnapshotAcknowledgement>::Success(token);
    }

    /** @copydoc ReplicationSnapshotHistory::Acknowledge */
    Result<void> ReplicationSnapshotHistory::Acknowledge(const ReplicationSnapshotAcknowledgement &ack, const std::uint64_t now,
                                                         const CancellationToken &cancellation) {
        // Reject foreign routing before maintenance so an old session cannot release current pins.
        if (owner_ != std::this_thread::get_id() || ack.scope != scope_)
            return Fail<void>(ReplicationStateErrors::Stale);
        if (const auto admitted = Advance(now, cancellation); admitted.HasError())
            return admitted;
        const auto found = std::ranges::find(entries_, ack, &Entry::token);
        if (found == entries_.end())
            return Fail<void>(ReplicationStateErrors::Stale);
        found->acknowledged = true;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationSnapshotHistory::Baseline */
    Result<ReplicationAcknowledgedBaseline> ReplicationSnapshotHistory::Baseline(const ReplicationCapturedStatePin &source,
                                                                                 const ReplicationRoleRevision role,
                                                                                 const std::uint64_t generation,
                                                                                 const Sha256Digest &projection, const std::uint64_t now,
                                                                                 const CancellationToken &cancellation) {
        if (const auto admitted = Advance(now, cancellation); admitted.HasError())
            return Result<ReplicationAcknowledgedBaseline>::Failure(admitted.ErrorValue());
        if (!Current(source))
            return Fail<ReplicationAcknowledgedBaseline>(ReplicationStateErrors::Stale);
        ReplicationAcknowledgedBaseline baseline;
        for (const auto &entry : entries_) {
            const auto &candidate = entry.baseline;
            if (entry.acknowledged && candidate.state->Object() == source->Object() &&
                candidate.state->World().MappingRevision() == source->World().MappingRevision() &&
                candidate.state->Descriptors() == source->Descriptors() && candidate.roleRevision == role &&
                candidate.descriptorGeneration == generation && candidate.projectionFingerprint == projection &&
                candidate.state->SimulationTick() <= source->SimulationTick() &&
                candidate.publicationRevision <= source->PublicationRevision() &&
                (!baseline.state || candidate.publicationRevision > baseline.publicationRevision))
                baseline = candidate;
        }
        return Result<ReplicationAcknowledgedBaseline>::Success(std::move(baseline));
    }

    /** @copydoc ReplicationSnapshotHistory::CancelSent */
    Result<void> ReplicationSnapshotHistory::CancelSent(const ReplicationSnapshotAcknowledgement &token, const std::uint64_t now) {
        if (owner_ != std::this_thread::get_id() || token.scope != scope_)
            return Fail<void>(ReplicationStateErrors::Stale);
        if (const auto admitted = Advance(now, {}); admitted.HasError())
            return admitted;
        const auto found = std::ranges::find(entries_, token, &Entry::token);
        if (found == entries_.end())
            return Fail<void>(ReplicationStateErrors::Stale);
        bytes_ -= found->bytes;
        entries_.erase(found);
        return Result<void>::Success();
    }

    /** @copydoc ReplicationSnapshotHistory::Clear */
    void ReplicationSnapshotHistory::Clear() noexcept {
        entries_.clear();
        bytes_ = 0;
    }

    /** @copydoc ReplicationSnapshotHistory::ReleaseForMemoryPressure */
    Result<void> ReplicationSnapshotHistory::ReleaseForMemoryPressure() {
        if (const auto admitted = Advance(clock_, {}); admitted.HasError())
            return admitted;
        Clear();
        return Result<void>::Success();
    }

    /** @copydoc ReplicationSnapshotHistory::Shutdown */
    Result<void> ReplicationSnapshotHistory::Shutdown() {
        if (owner_ != std::this_thread::get_id())
            return Fail<void>(ReplicationStateErrors::Invalid);
        Clear();
        closed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationSnapshotHistory::Expire */
    Result<void> ReplicationSnapshotHistory::Expire(const std::uint64_t now, const CancellationToken &cancellation) {
        return Advance(now, cancellation);
    }
}  // namespace Horo::Network
