#include "Horo/Network/NetworkDebugger.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace Horo::Network {
    namespace {
        /** @brief Saturates overflow accounting rather than wrapping it back to zero. */
        void Increment(std::uint64_t &count) noexcept {
            if (count != std::numeric_limits<std::uint64_t>::max())
                ++count;
        }

        /** @brief Retains a finite prefix; excess incoming records never grow storage. */
        template <typename T, std::size_t N> void Retain(NetworkDiagnosticHistory<T, N> &history, const T &record) noexcept {
            if (history.size == N)
                Increment(history.dropped);
            else
                history.records[history.size++] = record;
        }

        /** @brief Keeps recent current evidence with bounded work and explicit eviction accounting. */
        template <typename T, std::size_t N> void Recent(NetworkDiagnosticHistory<T, N> &history, const T &record) noexcept {
            if (history.size == N) {
                std::move(history.records.begin() + 1, history.records.end(), history.records.begin());
                history.records.back() = record;
                Increment(history.dropped);
            } else
                Retain(history, record);
        }
    }  // namespace

    /** @copydoc NetworkDiagnosticSource::Valid */
    bool NetworkDiagnosticSource::Valid() const noexcept {
        return process && session && scene && sceneGeneration;
    }

    NetworkDebugger::NetworkDebugger() : owner_(std::this_thread::get_id()) {}

    /** @copydoc NetworkDebugger::Begin */
    bool NetworkDebugger::Begin(const NetworkDiagnosticSource &source, const bool enabled, const NetworkDiagnosticProvider provider) {
        if (std::this_thread::get_id() != owner_ || !source.Valid() || provider > NetworkDiagnosticProvider::Other ||
            (current_.source.Valid() && std::tie(source.process, source.session, source.sceneGeneration) <=
                                            std::tie(current_.source.process, current_.source.session, current_.source.sceneGeneration)) ||
            current_.revision == std::numeric_limits<std::uint64_t>::max())
            return false;
        const auto revision = current_.revision;
        current_ = {};
        current_.source = source;
        current_.provider = enabled ? provider : NetworkDiagnosticProvider::Unavailable;
        current_.revision = revision + 1;
        current_.attached = true;
        current_.enabled = enabled;
        sequence_ = 0;
        std::scoped_lock lock{mutex_};
        commandCount_ = 0;
        published_ = current_;
        return true;
    }

    /** @copydoc NetworkDebugger::Source */
    NetworkDiagnosticSource NetworkDebugger::Source() const noexcept {
        return std::this_thread::get_id() == owner_ ? current_.source : NetworkDiagnosticSource{};
    }

    /** @brief Checks producer thread and exact lifetime before touching retained state. */
    bool NetworkDebugger::Admits(const NetworkDiagnosticSource &source) const noexcept {
        return std::this_thread::get_id() == owner_ && current_.attached && current_.enabled && source == current_.source;
    }

    /** @brief Captures fixed metadata under the elected capture pause policy. */
    void NetworkDebugger::Capture(const NetworkCaptureKind kind, const ConnectionHandle connection, const std::uint64_t amount) noexcept {
        if (current_.capturePaused)
            return;
        Increment(sequence_);
        Retain(current_.capture, NetworkCaptureRecord{sequence_, kind, connection, amount});
    }

    /** @copydoc NetworkDebugger::Observe */
    bool NetworkDebugger::Observe(const NetworkDiagnosticSource &source, const NetworkConnectionRecord &record) noexcept {
        if (!Admits(source) || !record.connection.IsValid() || record.event > NetworkTransportEventKind::Failed)
            return false;
        Recent(current_.connections, record);
        Capture(NetworkCaptureKind::Connection, record.connection, record.bytes);
        return true;
    }

    /** @copydoc NetworkDebugger::Observe */
    bool NetworkDebugger::Observe(const NetworkDiagnosticSource &source, const NetworkReplicationRecord &record) noexcept {
        if (!Admits(source))
            return false;
        Recent(current_.replication, record);
        Capture(NetworkCaptureKind::Replication, {}, record.published);
        return true;
    }

    /** @copydoc NetworkDebugger::Observe */
    bool NetworkDebugger::Observe(const NetworkDiagnosticSource &source, const NetworkRpcRecord &record) noexcept {
        if (!Admits(source))
            return false;
        Recent(current_.rpc, record);
        Capture(NetworkCaptureKind::Rpc, {}, record.accepted);
        return true;
    }

    /** @copydoc NetworkDebugger::Observe */
    bool NetworkDebugger::Observe(const NetworkDiagnosticSource &source, const NetworkPredictionRecord &record) noexcept {
        if (!Admits(source))
            return false;
        Recent(current_.prediction, record);
        Capture(NetworkCaptureKind::Prediction, {}, record.localTick);
        return true;
    }

    /** @copydoc NetworkDebugger::Observe */
    bool NetworkDebugger::Observe(const NetworkDiagnosticSource &source, const NetworkInterestRecord &record) noexcept {
        if (!Admits(source))
            return false;
        Recent(current_.interest, record);
        Capture(NetworkCaptureKind::Interest, {}, record.relevant);
        return true;
    }

    /** @copydoc NetworkDebugger::Publish */
    bool NetworkDebugger::Publish(const NetworkDiagnosticSource &source, const std::uint64_t now,
                                  const NetworkMetricSnapshot *metrics) noexcept {
        if (std::this_thread::get_id() != owner_ || !current_.attached || source != current_.source || now == 0 ||
            now < current_.publishedNanoseconds || current_.revision == std::numeric_limits<std::uint64_t>::max() ||
            (metrics && metrics->ownerGeneration != source.session))
            return false;
        std::scoped_lock lock{mutex_};
        using enum NetworkCaptureAction;
        for (std::size_t i = 0; i < commandCount_; ++i) {
            if (commands_[i].source != source)
                continue;
            switch (commands_[i].action) {
                case Pause:
                    current_.capturePaused = true;
                    break;
                case Resume:
                    current_.capturePaused = false;
                    break;
                case Clear:
                    current_.connections = {};
                    current_.replication = {};
                    current_.rpc = {};
                    current_.prediction = {};
                    current_.interest = {};
                    current_.capture = {};
                    break;
            }
        }
        commandCount_ = 0;
        if (metrics)
            current_.metrics = *metrics;
        current_.publishedNanoseconds = now;
        ++current_.revision;
        published_ = current_;
        return true;
    }

    /** @copydoc NetworkDebugger::Snapshot */
    NetworkDebuggerSnapshot NetworkDebugger::Snapshot() const {
        std::scoped_lock lock{mutex_};
        return published_;
    }

    /** @copydoc NetworkDebugger::Request */
    bool NetworkDebugger::Request(const NetworkDiagnosticSource &source, const std::uint64_t revision, const NetworkCaptureAction action) {
        std::scoped_lock lock{mutex_};
        if (!published_.attached || !published_.enabled || source != published_.source || revision != published_.revision ||
            action > NetworkCaptureAction::Clear || commandCount_ == commands_.size())
            return false;
        commands_[commandCount_++] = {source, action};
        return true;
    }

    /** @copydoc NetworkDebugger::Detach */
    void NetworkDebugger::Detach() noexcept {
        if (std::this_thread::get_id() != owner_)
            return;
        current_.attached = false;
        if (current_.revision != std::numeric_limits<std::uint64_t>::max())
            ++current_.revision;
        std::scoped_lock lock{mutex_};
        commandCount_ = 0;
        published_ = current_;
    }
}  // namespace Horo::Network
