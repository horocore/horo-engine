#include "Horo/Network/NetworkMetrics.h"

#include <limits>

namespace Horo::Network {
    namespace {
        template <typename Enum> [[nodiscard]] bool Known(const Enum value) noexcept {
            return static_cast<std::size_t>(value) < static_cast<std::size_t>(Enum::Count);
        }
    }  // namespace

    /** @copydoc NetworkMetrics::NetworkMetrics */
    NetworkMetrics::NetworkMetrics(const std::uint64_t generation, const bool enabled)
        : ownerThread_(std::this_thread::get_id()), admission_(std::make_shared<std::atomic<bool>>(enabled && generation != 0)) {
        current_.ownerGeneration = generation;
        current_.enabled = enabled && generation != 0;
        published_ = current_;
    }

    NetworkMetrics::~NetworkMetrics() {
        admission_->store(false);
    }

    bool NetworkMetrics::CanRecord() const noexcept {
        return std::this_thread::get_id() == ownerThread_ && admission_->load() && !current_.closed;
    }

    bool NetworkMetrics::Invalid() noexcept {
        AddSaturating(current_.invalidObservations, 1, current_.saturated);
        return false;
    }

    void NetworkMetrics::AddSaturating(std::uint64_t &target, const std::uint64_t delta, bool &saturated) noexcept {
        if (delta > std::numeric_limits<std::uint64_t>::max() - target) {
            target = std::numeric_limits<std::uint64_t>::max();
            saturated = true;
            return;
        }
        target += delta;
    }

    /** @copydoc NetworkMetrics::RecordMessage */
    bool NetworkMetrics::RecordMessage(const NetworkMetricDirection direction, const NetworkMetricCategory category,
                                       const std::uint64_t bytes) noexcept {
        if (!CanRecord())
            return false;
        if (!Known(direction) || !Known(category))
            return Invalid();
        const auto d = static_cast<std::size_t>(direction);
        const auto c = static_cast<std::size_t>(category);
        AddSaturating(current_.messages[d][c], 1, current_.saturated);
        AddSaturating(current_.bytes[d][c], bytes, current_.saturated);
        return true;
    }

    /** @copydoc NetworkMetrics::RecordLoss */
    bool NetworkMetrics::RecordLoss(const std::uint64_t count) noexcept {
        if (!CanRecord())
            return false;
        current_.lossAvailable = true;
        AddSaturating(current_.packetsLost, count, current_.saturated);
        return true;
    }

    /** @copydoc NetworkMetrics::RecordDrop */
    bool NetworkMetrics::RecordDrop(const NetworkMetricDrop reason, const std::uint64_t count) noexcept {
        if (!CanRecord())
            return false;
        if (!Known(reason))
            return Invalid();
        AddSaturating(current_.drops[static_cast<std::size_t>(reason)], count, current_.saturated);
        return true;
    }

    /** @copydoc NetworkMetrics::RecordFailure */
    bool NetworkMetrics::RecordFailure(const NetworkMetricFailure reason, const std::uint64_t count) noexcept {
        if (!CanRecord())
            return false;
        if (!Known(reason))
            return Invalid();
        AddSaturating(current_.failures[static_cast<std::size_t>(reason)], count, current_.saturated);
        return true;
    }

    /** @copydoc NetworkMetrics::RecordReplication */
    bool NetworkMetrics::RecordReplication(const NetworkMetricReplication kind, const std::uint64_t count) noexcept {
        if (!CanRecord())
            return false;
        if (!Known(kind))
            return Invalid();
        AddSaturating(current_.replication[static_cast<std::size_t>(kind)], count, current_.saturated);
        return true;
    }

    /** @copydoc NetworkMetrics::SetQueueDepth */
    bool NetworkMetrics::SetQueueDepth(const NetworkMetricQueue queue, const std::uint64_t depth) noexcept {
        if (!CanRecord())
            return false;
        if (!Known(queue))
            return Invalid();
        current_.queueDepth[static_cast<std::size_t>(queue)] = depth;
        return true;
    }

    /** @copydoc NetworkMetrics::SetActiveConnections */
    bool NetworkMetrics::SetActiveConnections(const std::uint64_t count) noexcept {
        if (!CanRecord())
            return false;
        current_.activeConnections = count;
        return true;
    }

    /** @copydoc NetworkMetrics::RecordRttMilliseconds */
    bool NetworkMetrics::RecordRttMilliseconds(const std::uint64_t value) noexcept {
        if (!CanRecord())
            return false;
        AddSaturating(rttSum_, value, current_.saturated);
        AddSaturating(rttSamples_, 1, current_.saturated);
        return true;
    }

    /** @copydoc NetworkMetrics::Publish */
    bool NetworkMetrics::Publish() noexcept {
        if (std::this_thread::get_id() != ownerThread_ || current_.closed || current_.revision == std::numeric_limits<std::uint64_t>::max())
            return false;
        current_.rttAvailable = rttSamples_ != 0;
        current_.rttMilliseconds = rttSamples_ != 0 ? rttSum_ / rttSamples_ : 0;
        rttSum_ = 0;
        rttSamples_ = 0;
        ++current_.revision;
        std::scoped_lock lock{publishedMutex_};
        published_ = current_;
        return true;
    }

    /** @copydoc NetworkMetrics::Snapshot */
    NetworkMetricSnapshot NetworkMetrics::Snapshot() const noexcept {
        std::scoped_lock lock{publishedMutex_};
        return published_;
    }

    /** @copydoc NetworkMetrics::IsCollecting */
    bool NetworkMetrics::IsCollecting() const noexcept {
        return CanRecord();
    }

    /** @copydoc NetworkMetrics::Close */
    bool NetworkMetrics::Close() noexcept {
        if (std::this_thread::get_id() != ownerThread_)
            return false;
        if (current_.closed)
            return true;
        if (current_.revision == std::numeric_limits<std::uint64_t>::max())
            return false;
        admission_->store(false);
        current_.closed = true;
        current_.queueDepth = {};
        current_.activeConnections = 0;
        current_.rttAvailable = false;
        current_.rttMilliseconds = 0;
        ++current_.revision;
        std::scoped_lock lock{publishedMutex_};
        published_ = current_;
        return true;
    }
}  // namespace Horo::Network
